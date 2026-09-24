// Server::set_connection_filter（连接来源准入控制）专项测试：未设置时默认全
// 放行、拒绝语义（连接被关闭且不建会话、不发 on_session_started、不占设备）、
// 按来源（源端口）过滤、过滤器抛异常按拒绝处理（fail-closed）且不打死 accept
// 循环、跨 start/stop 保留、清除后恢复放行
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "test_utils.h"

#include "usbipdcpp/Device.h"
#include "usbipdcpp/Server.h"
#include "usbipdcpp/protocol.h"
#include "usbipdcpp/utils/StringPool.h"
#include "usbipdcpp/virtual_device/SimpleVirtualDeviceHandler.h"
#include "usbipdcpp/virtual_device/devices/KeyboardHandler.h"

using namespace usbipdcpp;
using namespace usbipdcpp::test;

namespace {

// 记录会话开始事件的观察者：被拒连接不得产生该事件（回调在会话线程上执行，
// 内部加锁）
class StartedObserver : public ServerObserver {
public:
    void on_session_started(std::uint64_t session_id, const std::string &peer) override {
        started_count.fetch_add(1);
        std::lock_guard lock(mutex);
        peers.push_back(peer);
    }

    std::vector<std::string> peer_snapshot() const {
        std::lock_guard lock(mutex);
        return peers;
    }

    std::atomic<int> started_count{0};

private:
    mutable std::mutex mutex;
    std::vector<std::string> peers;
};

// Server 析构前必须已 stop（契约见 Server.h），否则网络线程仍 joinable 直接
// terminate。测试中途 ASSERT 失败会提前 return，靠 RAII 保证 stop 一定执行
class ServerStopper {
public:
    explicit ServerStopper(Server &server) : server(server) {
    }

    ~ServerStopper() {
        server.stop();
    }

    ServerStopper(const ServerStopper &) = delete;
    ServerStopper &operator=(const ServerStopper &) = delete;

private:
    Server &server;
};

std::shared_ptr<UsbDevice> make_keyboard(StringPool &string_pool) {
    std::vector<UsbInterface> interfaces = {
            UsbInterface{
                    .interface_class = static_cast<std::uint8_t>(ClassCode::HID),
                    .interface_subclass = 0x01, // Boot Interface Subclass
                    .interface_protocol = 0x01, // Keyboard
                    .endpoints = {{
                            UsbEndpoint{
                                    .address = 0x81, // IN
                                    .attributes = 0x03,
                                    .max_packet_size = 16,
                                    .interval = 10,
                            },
                    }},
            },
    };
    auto mock_keyboard = std::make_shared<UsbDevice>(UsbDevice{
            .path = "/test/mock_keyboard",
            .busid = "1-1",
            .bus_num = 1,
            .dev_num = 1,
            .speed = static_cast<std::uint32_t>(UsbSpeed::Full),
            .vendor_id = 0x1234,
            .product_id = 0x5678,
            .device_bcd = 0xABCD,
            .device_class = 0x00,
            .device_subclass = 0x00,
            .device_protocol = 0x00,
            .configuration_value = 1,
            .num_configurations = 1,
            .interfaces = interfaces,
            .ep0_in = UsbEndpoint::get_ep0_in(UsbSpeed::Full),
            .ep0_out = UsbEndpoint::get_ep0_out(UsbSpeed::Full),
    });
    mock_keyboard->interfaces[0].with_handler<KeyboardHandler>(string_pool);
    mock_keyboard->with_handler<SimpleVirtualDeviceHandler>(string_pool)->setup_interface_handlers();
    return mock_keyboard;
}

// 轮询等待条件成立。被拒连接的"没发生什么"无法用事件等待，统一用
// 过滤器自身的调用计数定序（每个 accept 的连接都会调用一次过滤器）
bool wait_until(const std::function<bool()> &pred, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return pred();
}

// 绑定到指定本地端口后再连服务器：模拟"客户端先占住一个临时源端口、再让
// 服务器只放行该端口"的场景。connect_with_retry 会在重试时重建 socket，
// 绑定的端口会丢失，因此这里自带重试
bool connect_from_port(asio::ip::tcp::socket &client, const asio::ip::tcp::endpoint &server_ep,
                       std::uint16_t local_port) {
    for (int i = 0; i < 200; i++) {
        std::error_code ec;
        client.close();
        client = asio::ip::tcp::socket(client.get_executor());
        // bind 要求 socket 已打开（asio 的 connect 会自动 open，bind 不会）
        client.open(asio::ip::tcp::v4(), ec);
        if (ec) {
            return false;
        }
        client.bind(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), local_port), ec);
        if (ec) {
            // 端口可能还未从上一轮释放，重试
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
            continue;
        }
        client.connect(server_ep, ec);
        if (!ec) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

// 断言连接已被服务器关闭：过滤器处理完该连接后，读取应立刻得到 EOF（FIN）
// 或重置错误（RST），而不是继续阻塞
void expect_closed_by_server(asio::ip::tcp::socket &client) {
    char buf[1] = {};
    asio::error_code ec;
    const std::size_t n = client.read_some(asio::buffer(buf), ec);
    EXPECT_TRUE(ec || n == 0) << "连接未被关闭（读到 " << n << " 字节且无错误）";
}

} // namespace

TEST(TestConnectionFilter, AllowsAllWhenNotSet) {
    // 未设置过滤器：行为与引入过滤器之前完全一致（默认全部放行）
    asio::io_context io;
    StringPool string_pool;
    StartedObserver observer;
    Server server;
    ServerStopper stopper(server);
    server.add_device(make_keyboard(string_pool));
    server.add_observer(&observer);
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    EXPECT_EQ(import_device(client, "1-1"), 0u);
    ASSERT_TRUE(wait_until([&] { return observer.started_count.load() == 1; }));
    EXPECT_EQ(server.get_session_count(), 1u);
    client.close();
}

TEST(TestConnectionFilter, RejectClosesConnectionWithoutSessionOrEvent) {
    // 被拒连接：socket 被关闭、不建会话、不发 on_session_started。
    // 被拒连接的"什么都没发生"用后一个放行连接 + 过滤器调用计数定序
    // （accept 循环串行，放行连接被处理时被拒连接必然已处理完）
    asio::io_context io;
    StringPool string_pool;
    StartedObserver observer;
    Server server;
    ServerStopper stopper(server);
    server.add_device(make_keyboard(string_pool));
    server.add_observer(&observer);

    const std::uint16_t allowed_port = probe_free_port(io);
    std::atomic<int> filter_calls{0};
    server.set_connection_filter([&](const asio::ip::tcp::endpoint &ep) {
        filter_calls.fetch_add(1);
        return ep.port() == allowed_port;
    });
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    // 源端口不在白名单：拒绝
    asio::ip::tcp::socket rejected(io);
    ASSERT_TRUE(connect_with_retry(rejected, server.endpoint()));

    // 源端口在白名单：放行
    asio::ip::tcp::socket allowed(io);
    ASSERT_TRUE(connect_from_port(allowed, server.endpoint(), allowed_port));
    ASSERT_EQ(import_device(allowed, "1-1"), 0u);

    ASSERT_TRUE(wait_until([&] { return filter_calls.load() >= 2; }));
    EXPECT_EQ(observer.started_count.load(), 1) << "被拒连接不得发出 on_session_started";
    EXPECT_EQ(server.get_session_count(), 1u) << "被拒连接不得建立会话";

    expect_closed_by_server(rejected);
}

TEST(TestConnectionFilter, NonWhitelistedPortIsRejected) {
    // 只放行指定源端口（Android 同机回环场景：127.0.0.1 被所有 app 共享，
    // 服务器只信任客户端刚占住的那个端口）——白名单外的连接一律拒绝
    asio::io_context io;
    StringPool string_pool;
    StartedObserver observer;
    Server server;
    ServerStopper stopper(server);
    server.add_device(make_keyboard(string_pool));
    server.add_observer(&observer);

    const std::uint16_t allowed_port = probe_free_port(io);
    server.set_connection_filter(
            [allowed_port](const asio::ip::tcp::endpoint &ep) { return ep.port() == allowed_port; });
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    // 白名单端口：放行并完成一次导入
    asio::ip::tcp::socket allowed(io);
    ASSERT_TRUE(connect_from_port(allowed, server.endpoint(), allowed_port));
    ASSERT_EQ(import_device(allowed, "1-1"), 0u);
    ASSERT_TRUE(wait_until([&] { return observer.started_count.load() == 1; }));
    allowed.close();

    // 非白名单端口：拒绝，且不影响已有会话的收尾
    asio::ip::tcp::socket rejected(io);
    ASSERT_TRUE(connect_with_retry(rejected, server.endpoint()));
    ASSERT_TRUE(wait_until([&] { return server.get_session_count() == 0; }));
    EXPECT_EQ(observer.started_count.load(), 1) << "非白名单连接不得建立会话";
    expect_closed_by_server(rejected);
}

TEST(TestConnectionFilter, FilterSeesPeerEndpoint) {
    // 过滤器拿到的对端地址就是客户端的来源地址（本用例验证源端口按绑定值）
    asio::io_context io;
    StringPool string_pool;
    Server server;
    ServerStopper stopper(server);
    server.add_device(make_keyboard(string_pool));

    std::atomic<std::uint16_t> seen_port{0};
    std::atomic<int> filter_calls{0};
    server.set_connection_filter([&](const asio::ip::tcp::endpoint &ep) {
        seen_port.store(ep.port());
        filter_calls.fetch_add(1);
        return false; // 入参是本用例唯一关注点，一律拒绝
    });
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    const std::uint16_t local_port = probe_free_port(io);
    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_from_port(client, server.endpoint(), local_port));
    ASSERT_TRUE(wait_until([&] { return filter_calls.load() >= 1; }));
    EXPECT_EQ(seen_port.load(), local_port);
}

TEST(TestConnectionFilter, ThrowingFilterRejectsAndServerStaysUsable) {
    // 过滤器抛异常按"拒绝"处理（fail-closed），且不打死 accept 循环：
    // 之后返回 true 时连接照常建立
    asio::io_context io;
    StringPool string_pool;
    StartedObserver observer;
    Server server;
    ServerStopper stopper(server);
    server.add_device(make_keyboard(string_pool));
    server.add_observer(&observer);

    std::atomic<int> filter_calls{0};
    server.set_connection_filter([&](const asio::ip::tcp::endpoint &) -> bool {
        // 前两次调用抛异常（std::exception 分支），之后放行
        if (filter_calls.fetch_add(1) < 2) {
            throw std::runtime_error("测试异常");
        }
        return true;
    });
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket first(io);
    asio::ip::tcp::socket second(io);
    asio::ip::tcp::socket third(io);
    ASSERT_TRUE(connect_with_retry(first, server.endpoint()));
    ASSERT_TRUE(connect_with_retry(second, server.endpoint()));
    ASSERT_TRUE(connect_with_retry(third, server.endpoint()));

    // 三次连接都过滤过：抛异常的两次被拒，第三次放行
    ASSERT_TRUE(wait_until([&] { return filter_calls.load() >= 3; }));
    ASSERT_TRUE(wait_until([&] { return observer.started_count.load() == 1; }));
    EXPECT_EQ(observer.started_count.load(), 1) << "抛异常的连接不得建立会话";
    EXPECT_EQ(server.get_session_count(), 1u);

    // 放行的是哪一个取决于 accept 顺序，按端口找到它并验证可用
    const auto peers = observer.peer_snapshot();
    ASSERT_EQ(peers.size(), 1u);
    const std::string peer = peers.front(); // 形如 "127.0.0.1:port"
    const std::uint16_t winner_port = static_cast<std::uint16_t>(std::stoi(peer.substr(peer.rfind(':') + 1)));
    asio::ip::tcp::socket *winner = nullptr;
    for (auto *candidate: {&first, &second, &third}) {
        if (candidate->local_endpoint().port() == winner_port) {
            winner = candidate;
            break;
        }
    }
    ASSERT_NE(winner, nullptr) << "收到通知的连接必须是三个客户端之一";
    EXPECT_EQ(import_device(*winner, "1-1"), 0u);
}

TEST(TestConnectionFilter, ManyRejectionsThenAllow) {
    // 连续多个被拒连接不破坏 accept 循环（也间接验证 active_sessions 计数
    // 平衡：每次拒绝都会创建又析构一个 Session，计数泄漏会让 stop() 卡住）
    asio::io_context io;
    StringPool string_pool;
    Server server;
    ServerStopper stopper(server);
    server.add_device(make_keyboard(string_pool));

    const std::uint16_t allowed_port = probe_free_port(io);
    std::atomic<int> filter_calls{0};
    server.set_connection_filter([&](const asio::ip::tcp::endpoint &ep) {
        filter_calls.fetch_add(1);
        return ep.port() == allowed_port;
    });
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    std::vector<asio::ip::tcp::socket> rejected;
    for (int i = 0; i < 5; i++) {
        rejected.emplace_back(io);
        ASSERT_TRUE(connect_with_retry(rejected.back(), server.endpoint()));
    }
    ASSERT_TRUE(wait_until([&] { return filter_calls.load() >= 5; }));
    EXPECT_EQ(server.get_session_count(), 0u);

    // 被拒的连接全部被关闭，随后白名单连接照常可用
    for (auto &client: rejected) {
        expect_closed_by_server(client);
    }
    asio::ip::tcp::socket allowed(io);
    ASSERT_TRUE(connect_from_port(allowed, server.endpoint(), allowed_port));
    EXPECT_EQ(import_device(allowed, "1-1"), 0u);
    EXPECT_EQ(server.get_session_count(), 1u);
}

TEST(TestConnectionFilter, PersistsAcrossRestart) {
    // 过滤器是 Server 成员：stop 后再次 start 仍然生效，无需重新设置
    asio::io_context io;
    StringPool string_pool;
    Server server;
    ServerStopper stopper(server);
    server.add_device(make_keyboard(string_pool));

    std::atomic<int> filter_calls{0};
    server.set_connection_filter([&](const asio::ip::tcp::endpoint &) {
        filter_calls.fetch_add(1);
        return false;
    });

    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));
    asio::ip::tcp::socket first_round(io);
    ASSERT_TRUE(connect_with_retry(first_round, server.endpoint()));
    ASSERT_TRUE(wait_until([&] { return filter_calls.load() >= 1; }));
    EXPECT_EQ(server.get_session_count(), 0u);

    server.stop();
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));
    asio::ip::tcp::socket second_round(io);
    ASSERT_TRUE(connect_with_retry(second_round, server.endpoint()));
    ASSERT_TRUE(wait_until([&] { return filter_calls.load() >= 2; }));
    EXPECT_EQ(server.get_session_count(), 0u) << "重启后过滤器必须仍然生效";
}

TEST(TestConnectionFilter, ClearRestoresAllowAll) {
    // 清除过滤器（传空 std::function）后恢复全部放行
    asio::io_context io;
    StringPool string_pool;
    Server server;
    ServerStopper stopper(server);
    server.add_device(make_keyboard(string_pool));

    std::atomic<int> filter_calls{0};
    server.set_connection_filter([&](const asio::ip::tcp::endpoint &) {
        filter_calls.fetch_add(1);
        return false;
    });
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));
    asio::ip::tcp::socket rejected(io);
    ASSERT_TRUE(connect_with_retry(rejected, server.endpoint()));
    ASSERT_TRUE(wait_until([&] { return filter_calls.load() >= 1; }));
    EXPECT_EQ(server.get_session_count(), 0u);
    server.stop();

    server.set_connection_filter(nullptr);
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));
    asio::ip::tcp::socket allowed(io);
    ASSERT_TRUE(connect_with_retry(allowed, server.endpoint()));
    EXPECT_EQ(import_device(allowed, "1-1"), 0u);
    EXPECT_EQ(server.get_session_count(), 1u);
    EXPECT_EQ(filter_calls.load(), 1) << "清除后过滤器不应再被调用";
}
