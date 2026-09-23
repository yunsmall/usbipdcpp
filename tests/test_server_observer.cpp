// ServerObserver 通知机制测试：各事件的触发时机与入参、多观察者、幂等注册、
// 注销后不再收到、回调抛异常不波及服务器与其他观察者、回调中查询/操作 Server
// 不死锁（锁外通知的证明）、观察者数量上限
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
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
// 记录所有事件的观察者。回调跑在会话/后端线程上，内部加锁
class RecordingObserver : public ServerObserver {
public:
    struct Event {
        std::string kind;
        std::uint64_t session_id = 0;
        std::string text; // started 记 peer，attached/released 记 busid
        DeviceReleaseReason reason = DeviceReleaseReason::ClientDisconnected;
    };

    void on_session_started(std::uint64_t session_id, const std::string &peer) override {
        record({"started", session_id, peer, DeviceReleaseReason::ClientDisconnected});
    }

    void on_session_ended(std::uint64_t session_id) override {
        record({"ended", session_id, {}, DeviceReleaseReason::ClientDisconnected});
    }

    void on_device_attached(const std::string &busid) override {
        record({"attached", 0, busid, DeviceReleaseReason::ClientDisconnected});
    }

    void on_device_released(const std::string &busid, DeviceReleaseReason reason) override {
        record({"released", 0, busid, reason});
    }

    std::vector<Event> snapshot() const {
        std::lock_guard lock(mutex);
        return events;
    }

    std::size_t count(const std::string &kind) const {
        std::lock_guard lock(mutex);
        return std::count_if(events.begin(), events.end(), [&](const Event &e) { return e.kind == kind; });
    }

private:
    void record(Event event) {
        std::lock_guard lock(mutex);
        events.push_back(std::move(event));
    }

    mutable std::mutex mutex;
    std::vector<Event> events;
};

const RecordingObserver::Event *find_event(const std::vector<RecordingObserver::Event> &events,
                                           const std::string &kind) {
    for (const auto &e: events) {
        if (e.kind == kind) {
            return &e;
        }
    }
    return nullptr;
}

std::size_t count_events(const std::vector<RecordingObserver::Event> &events, const std::string &kind,
                         const std::string &text) {
    return std::count_if(events.begin(), events.end(), [&](const RecordingObserver::Event &e) {
        return e.kind == kind && e.text == text;
    });
}

// 轮询等待观察者收到至少 expected 个指定事件。用于"连接后立刻就断开"的
// 场景：此时连接可能还在 accept 队列里（session 尚未创建，
// get_session_count 为 0），wait_sessions_gone 会提前返回而事件还没发出
bool wait_for_event_count(const RecordingObserver &observer, const std::string &kind, std::size_t expected,
                          std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (observer.count(kind) >= expected) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return observer.count(kind) >= expected;
}

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

// 每次回调都抛异常的观察者（on_session_started 抛 std::exception，
// on_device_released 抛非 std::exception，两条 catch 分支都要覆盖）
class ThrowingObserver : public ServerObserver {
public:
    void on_session_started(std::uint64_t, const std::string &) override {
        started_calls.fetch_add(1);
        throw std::runtime_error("测试异常");
    }

    void on_device_released(const std::string &, DeviceReleaseReason) override {
        released_calls.fetch_add(1);
        throw 42;
    }

    std::atomic<int> started_calls{0};
    std::atomic<int> released_calls{0};
};

// 在回调里查询 Server 的观察者：通知若在锁内发出，这里必然死锁
// （devices_mutex / session_list_mutex 都是非递归锁），测试会超时失败
class QueryingObserver : public ServerObserver {
public:
    Server *server = nullptr;
    std::atomic<std::size_t> session_count_seen{0};
    std::atomic<bool> attached_device_bound{false};
    std::atomic<std::size_t> using_count_seen{0};

    void on_session_started(std::uint64_t, const std::string &) override {
        session_count_seen.store(server->get_session_count());
    }

    void on_device_attached(const std::string &busid) override {
        attached_device_bound.store(server->has_bound_device(busid));
        std::shared_lock lock(server->get_devices_mutex());
        using_count_seen.store(server->get_using_devices().size());
    }
};

// 收到第一个会话开始事件就注销自己：通知遍历用的是锁内快照，
// 回调里改注册表（含注销自己）是安全的
class SelfRemovingObserver : public ServerObserver {
public:
    Server *server = nullptr;
    std::atomic<int> started_count{0};

    void on_session_started(std::uint64_t, const std::string &) override {
        started_count.fetch_add(1);
        server->remove_observer(this);
    }
};

// 可标记"已物理移除"的设备级 handler
class ToggleRemovedDeviceHandler : public SimpleVirtualDeviceHandler {
public:
    using SimpleVirtualDeviceHandler::SimpleVirtualDeviceHandler;

    std::shared_ptr<std::atomic_bool> removed_flag;

    bool is_device_removed() const override {
        return removed_flag && removed_flag->load();
    }
};

// 构造测试用虚拟键盘；removed_flag 非空时设备级 handler 可被标记"已物理移除"
std::shared_ptr<UsbDevice> make_keyboard(StringPool &string_pool,
                                         std::shared_ptr<std::atomic_bool> removed_flag = nullptr,
                                         const std::string &busid = "1-1") {
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
            .busid = busid,
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
    if (removed_flag) {
        auto handler = mock_keyboard->with_handler<ToggleRemovedDeviceHandler>(string_pool);
        handler->removed_flag = removed_flag;
        handler->setup_interface_handlers();
    }
    else {
        mock_keyboard->with_handler<SimpleVirtualDeviceHandler>(string_pool)->setup_interface_handlers();
    }
    return mock_keyboard;
}
} // namespace

TEST(TestServerObserver, SessionAndDeviceEventsOnClientDisconnect) {
    // 完整一轮：连接 → 导入 → 断开。事件顺序：started 最先、ended 最后，
    // attached / released 居中（导入成功与客户端断开各一次）
    asio::io_context io;
    StringPool string_pool;
    RecordingObserver observer;
    Server server;
    ServerStopper stopper(server);
    server.add_device(make_keyboard(string_pool));
    server.add_observer(&observer);
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u);
    client.close();
    // 等事件发完而不是等会话计数：remove_session 是锁内 erase、锁外通知，
    // get_session_count 归零时 on_session_ended 可能还在发出途中
    ASSERT_TRUE(wait_for_event_count(observer, "ended", 1));

    const auto events = observer.snapshot();
    ASSERT_FALSE(events.empty());
    EXPECT_EQ(events.front().kind, "started");
    EXPECT_NE(events.front().session_id, 0u);
    EXPECT_FALSE(events.front().text.empty()); // peer 形如 "ip:port"

    const auto *attached = find_event(events, "attached");
    ASSERT_NE(attached, nullptr);
    EXPECT_EQ(attached->text, "1-1");

    const auto *released = find_event(events, "released");
    ASSERT_NE(released, nullptr);
    EXPECT_EQ(released->text, "1-1");
    EXPECT_EQ(released->reason, DeviceReleaseReason::ClientDisconnected);

    EXPECT_EQ(events.back().kind, "ended");
    EXPECT_EQ(events.back().session_id, events.front().session_id);

    // 每个事件各一次，没有重复
    EXPECT_EQ(observer.count("started"), 1u);
    EXPECT_EQ(observer.count("attached"), 1u);
    EXPECT_EQ(observer.count("released"), 1u);
    EXPECT_EQ(observer.count("ended"), 1u);

    server.stop();
}

TEST(TestServerObserver, DeviceReleasedReasonDeviceRemoved) {
    // 传输期间设备被物理拔出，客户端断开释放时归因 DeviceRemoved
    asio::io_context io;
    StringPool string_pool;
    RecordingObserver observer;
    Server server;
    ServerStopper stopper(server);
    auto removed_flag = std::make_shared<std::atomic_bool>(false);
    server.add_device(make_keyboard(string_pool, removed_flag));
    server.add_observer(&observer);
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u);

    removed_flag->store(true); // 传输期间设备被物理拔出
    client.close();
    ASSERT_TRUE(wait_for_event_count(observer, "ended", 1));

    const auto events = observer.snapshot();
    const auto *released = find_event(events, "released");
    ASSERT_NE(released, nullptr);
    EXPECT_EQ(released->text, "1-1");
    EXPECT_EQ(released->reason, DeviceReleaseReason::DeviceRemoved);

    server.stop();
}

TEST(TestServerObserver, DeviceReleasedReasonServerStopped) {
    // 客户端还连着直接 stop()：设备释放归因 ServerStopped
    asio::io_context io;
    StringPool string_pool;
    RecordingObserver observer;
    Server server;
    ServerStopper stopper(server);
    server.add_device(make_keyboard(string_pool));
    server.add_observer(&observer);
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u);

    server.stop(); // 不关客户端，走 stop 打断路径

    const auto events = observer.snapshot();
    const auto *attached = find_event(events, "attached");
    ASSERT_NE(attached, nullptr);
    const auto *released = find_event(events, "released");
    ASSERT_NE(released, nullptr);
    EXPECT_EQ(released->text, "1-1");
    EXPECT_EQ(released->reason, DeviceReleaseReason::ServerStopped);

    client.close();
}

TEST(TestServerObserver, MultipleObserversAllNotified) {
    asio::io_context io;
    StringPool string_pool;
    RecordingObserver observer_a;
    RecordingObserver observer_b;
    Server server;
    ServerStopper stopper(server);
    server.add_device(make_keyboard(string_pool));
    server.add_observer(&observer_a);
    server.add_observer(&observer_b);
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u);
    client.close();
    ASSERT_TRUE(wait_for_event_count(observer_a, "ended", 1));
    ASSERT_TRUE(wait_for_event_count(observer_b, "ended", 1));

    for (auto *observer: {&observer_a, &observer_b}) {
        EXPECT_EQ(observer->count("started"), 1u);
        EXPECT_EQ(observer->count("attached"), 1u);
        EXPECT_EQ(observer->count("released"), 1u);
        EXPECT_EQ(observer->count("ended"), 1u);
    }

    server.stop();
}

TEST(TestServerObserver, AddObserverIsIdempotent) {
    // 同一指针注册两次只保留一个，每个事件仍只通知一次
    asio::io_context io;
    StringPool string_pool;
    RecordingObserver observer;
    Server server;
    ServerStopper stopper(server);
    server.add_device(make_keyboard(string_pool));
    server.add_observer(&observer);
    server.add_observer(&observer);
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u);
    client.close();
    ASSERT_TRUE(wait_for_event_count(observer, "ended", 1));

    EXPECT_EQ(observer.count("started"), 1u);
    EXPECT_EQ(observer.count("ended"), 1u);

    server.stop();
}

TEST(TestServerObserver, RemovedObserverStopsReceiving) {
    // 注销后不再收到任何事件；未注销的观察者不受影响
    asio::io_context io;
    StringPool string_pool;
    RecordingObserver observer_a;
    RecordingObserver observer_b;
    Server server;
    ServerStopper stopper(server);
    server.add_device(make_keyboard(string_pool));
    server.add_observer(&observer_a);
    server.add_observer(&observer_b);
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u);

    server.remove_observer(&observer_a);
    server.remove_observer(&observer_a); // 重复注销无害

    client.close();
    ASSERT_TRUE(wait_for_event_count(observer_b, "ended", 1));

    EXPECT_EQ(observer_a.count("started"), 1u); // 注销前的 started 收到过
    EXPECT_EQ(observer_a.count("ended"), 0u);   // 注销后的 ended 收不到
    EXPECT_EQ(observer_a.count("released"), 0u);
    EXPECT_EQ(observer_b.count("ended"), 1u);
    EXPECT_EQ(observer_b.count("released"), 1u);

    server.stop();
}

TEST(TestServerObserver, CallbackExceptionDoesNotAffectServer) {
    // 抛异常的观察者既不影响服务器正常服务，也不影响其他观察者的通知
    asio::io_context io;
    StringPool string_pool;
    ThrowingObserver thrower;
    RecordingObserver observer;
    Server server;
    ServerStopper stopper(server);
    server.add_device(make_keyboard(string_pool));
    server.add_observer(&thrower);
    server.add_observer(&observer);
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u); // 服务不受异常影响
    client.close();
    ASSERT_TRUE(wait_for_event_count(observer, "ended", 1));

    EXPECT_GT(thrower.started_calls.load(), 0);
    EXPECT_GT(thrower.released_calls.load(), 0);
    EXPECT_EQ(observer.count("started"), 1u);
    EXPECT_EQ(observer.count("attached"), 1u);
    EXPECT_EQ(observer.count("released"), 1u);
    EXPECT_EQ(observer.count("ended"), 1u);

    // 异常之后服务仍然可用：再来一轮完整流程
    asio::ip::tcp::socket client2(io);
    ASSERT_TRUE(connect_with_retry(client2, server.endpoint()));
    EXPECT_EQ(import_device(client2, "1-1"), 0u);
    client2.close();
    ASSERT_TRUE(wait_for_event_count(observer, "ended", 2));
    EXPECT_EQ(observer.count("ended"), 2u);

    server.stop();
}

TEST(TestServerObserver, CallbackCanQueryServerWithoutDeadlock) {
    // 通知在锁外发出：回调里可以调 Server 的加锁接口（锁内通知会死锁）
    asio::io_context io;
    StringPool string_pool;
    QueryingObserver observer;
    Server server;
    ServerStopper stopper(server);
    observer.server = &server;
    server.add_device(make_keyboard(string_pool));
    server.add_observer(&observer);
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u);
    client.close();
    ASSERT_TRUE(wait_sessions_gone(server));

    EXPECT_EQ(observer.session_count_seen.load(), 1u); // started 时会话已在表里
    EXPECT_TRUE(observer.attached_device_bound.load());
    EXPECT_EQ(observer.using_count_seen.load(), 1u);

    server.stop();
}

TEST(TestServerObserver, CallbackCanRemoveItself) {
    // 回调里注销自己：通知遍历用快照，改注册表不影响本次遍历
    asio::io_context io;
    StringPool string_pool;
    SelfRemovingObserver observer;
    Server server;
    ServerStopper stopper(server);
    observer.server = &server;
    server.add_device(make_keyboard(string_pool));
    server.add_observer(&observer);
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u);
    client.close();
    ASSERT_TRUE(wait_sessions_gone(server));
    EXPECT_EQ(observer.started_count.load(), 1);

    // 已注销：第二次连接的 started 收不到
    asio::ip::tcp::socket client2(io);
    ASSERT_TRUE(connect_with_retry(client2, server.endpoint()));
    client2.close();
    ASSERT_TRUE(wait_sessions_gone(server));
    EXPECT_EQ(observer.started_count.load(), 1);

    server.stop();
}

TEST(TestServerObserver, ObserverLimitIgnoresExtraRegistrations) {
    // 上限 8（与 Server::MAX_OBSERVERS 同步）：超出的注册被忽略，
    // 已有观察者不受影响
    constexpr std::size_t kMaxObservers = 8;
    asio::io_context io;
    StringPool string_pool;
    std::vector<std::unique_ptr<RecordingObserver>> observers;
    Server server;
    ServerStopper stopper(server);
    server.add_device(make_keyboard(string_pool));
    for (std::size_t i = 0; i < kMaxObservers + 1; ++i) {
        observers.push_back(std::make_unique<RecordingObserver>());
        server.add_observer(observers.back().get());
    }
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u);
    client.close();
    ASSERT_TRUE(wait_for_event_count(*observers[0], "ended", 1));

    for (std::size_t i = 0; i < kMaxObservers; ++i) {
        EXPECT_EQ(observers[i]->count("started"), 1u) << "观察者 " << i;
    }
    EXPECT_EQ(observers[kMaxObservers]->count("started"), 0u); // 超限的被忽略

    server.stop();
}

TEST(TestServerObserver, NoDeviceEventsWhenNeverImported) {
    // 连接后不导入就直接断开：没有设备事件（attached 只在真正导入成功时发）。
    // FIN 断开必定建立会话（内核把 socket 交给 accept，服务器读到 EOF 后
    // 收尾），会话事件必定发出；RST 则可能在 accept 之前就被内核丢弃
    // （accept 返回 connection_aborted，服务器不创建会话），RST 分支只等
    // 可能的会话收尾，不断言会话事件
    for (bool use_rst: {false, true}) {
        asio::io_context io;
        StringPool string_pool;
        RecordingObserver observer;
        Server server;
        ServerStopper stopper(server);
        server.add_device(make_keyboard(string_pool));
        server.add_observer(&observer);
        ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

        asio::ip::tcp::socket client(io);
        ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
        if (use_rst) {
            rst_disconnect(client);
        }
        else {
            client.close();
            // 等事件而不是等会话计数：连接可能还在 accept 队列里
            ASSERT_TRUE(wait_for_event_count(observer, "started", 1));
            ASSERT_TRUE(wait_for_event_count(observer, "ended", 1));
            EXPECT_EQ(observer.count("started"), 1u);
            EXPECT_EQ(observer.count("ended"), 1u);
        }
        ASSERT_TRUE(wait_sessions_gone(server));

        EXPECT_EQ(observer.count("attached"), 0u);
        EXPECT_EQ(observer.count("released"), 0u);

        server.stop();
    }
}

TEST(TestServerObserver, FailedImportEmitsNoDeviceEvents) {
    // 导入不存在的 busid 失败（NoDev）：不发 attached；会话断开时也没有过
    // 导入，不发 released
    asio::io_context io;
    StringPool string_pool;
    RecordingObserver observer;
    Server server;
    ServerStopper stopper(server);
    server.add_device(make_keyboard(string_pool));
    server.add_observer(&observer);
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "9-9"), static_cast<std::uint32_t>(OperationStatuType::NoDev));
    client.close();
    ASSERT_TRUE(wait_for_event_count(observer, "ended", 1));

    EXPECT_EQ(observer.count("started"), 1u);
    EXPECT_EQ(observer.count("ended"), 1u);
    EXPECT_EQ(observer.count("attached"), 0u);
    EXPECT_EQ(observer.count("released"), 0u);

    server.stop();
}

TEST(TestServerObserver, NullObserverIsIgnored) {
    // 空指针注册/注销无害，不影响已有观察者
    asio::io_context io;
    StringPool string_pool;
    RecordingObserver observer;
    Server server;
    ServerStopper stopper(server);
    server.add_device(make_keyboard(string_pool));
    server.add_observer(nullptr);
    server.remove_observer(nullptr);
    server.add_observer(&observer);
    server.add_observer(nullptr);
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u);
    client.close();
    ASSERT_TRUE(wait_for_event_count(observer, "ended", 1));

    EXPECT_EQ(observer.count("started"), 1u);
    EXPECT_EQ(observer.count("ended"), 1u);

    server.stop();
}

TEST(TestServerObserver, ServerRestartKeepsObservers) {
    // 观察者跨 start/stop 保持注册；stop 时若设备未被导入则不发设备事件
    asio::io_context io;
    StringPool string_pool;
    RecordingObserver observer;
    Server server;
    ServerStopper stopper(server);
    server.add_device(make_keyboard(string_pool));
    server.add_observer(&observer);
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u);
    client.close();
    ASSERT_TRUE(wait_for_event_count(observer, "ended", 1));
    server.stop();
    EXPECT_EQ(observer.count("released"), 1u);

    // 重启后观察者仍在：第二轮事件继续送达
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));
    asio::ip::tcp::socket client2(io);
    ASSERT_TRUE(connect_with_retry(client2, server.endpoint()));
    ASSERT_EQ(import_device(client2, "1-1"), 0u);
    client2.close();
    ASSERT_TRUE(wait_for_event_count(observer, "ended", 2));

    EXPECT_EQ(observer.count("started"), 2u);
    EXPECT_EQ(observer.count("attached"), 2u);
    EXPECT_EQ(observer.count("released"), 2u);
    EXPECT_EQ(observer.count("ended"), 2u);

    // 未导入任何设备就 stop：没有释放发生过，不发设备事件
    server.stop();
    EXPECT_EQ(observer.count("released"), 2u);
}

TEST(TestServerObserver, ConcurrentSessionsAllNotified) {
    // 3 个客户端各自导入一台设备并发活动：事件按会话/设备各自配对，
    // 会话 id 互不相同、每台设备各自 attached/released 一次
    asio::io_context io;
    StringPool string_pool;
    RecordingObserver observer;
    Server server;
    ServerStopper stopper(server);
    server.add_device(make_keyboard(string_pool, nullptr, "1-1"));
    server.add_device(make_keyboard(string_pool, nullptr, "1-2"));
    server.add_device(make_keyboard(string_pool, nullptr, "1-3"));
    server.add_observer(&observer);
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    std::vector<asio::ip::tcp::socket> clients;
    for (const auto *busid: {"1-1", "1-2", "1-3"}) {
        clients.emplace_back(io);
        ASSERT_TRUE(connect_with_retry(clients.back(), server.endpoint()));
        ASSERT_EQ(import_device(clients.back(), busid), 0u);
    }
    for (auto &client: clients) {
        client.close();
    }
    ASSERT_TRUE(wait_for_event_count(observer, "ended", 3));

    EXPECT_EQ(observer.count("started"), 3u);
    EXPECT_EQ(observer.count("ended"), 3u);
    EXPECT_EQ(observer.count("attached"), 3u);
    EXPECT_EQ(observer.count("released"), 3u);

    std::set<std::uint64_t> session_ids;
    const auto events = observer.snapshot();
    for (const auto &e: events) {
        if (e.kind == "started") {
            session_ids.insert(e.session_id);
        }
    }
    EXPECT_EQ(session_ids.size(), 3u); // 三个会话 id 互不相同
    for (const auto *busid: {"1-1", "1-2", "1-3"}) {
        EXPECT_EQ(count_events(events, "attached", busid), 1u) << busid;
        EXPECT_EQ(count_events(events, "released", busid), 1u) << busid;
    }

    server.stop();
}

TEST(TestServerObserver, ReRegisterAfterRemoveWithinLimit) {
    // 满员时新注册被忽略；注销一个腾出槽位后新注册成功，且被注销者
    // 不再收到事件（末位顶替不能顶丢其他观察者）
    constexpr std::size_t kMaxObservers = 8;
    asio::io_context io;
    StringPool string_pool;
    std::vector<std::unique_ptr<RecordingObserver>> observers;
    RecordingObserver rejected;
    RecordingObserver newcomer;
    Server server;
    ServerStopper stopper(server);
    server.add_device(make_keyboard(string_pool));
    for (std::size_t i = 0; i < kMaxObservers; ++i) {
        observers.push_back(std::make_unique<RecordingObserver>());
        server.add_observer(observers.back().get());
    }
    server.add_observer(&rejected); // 满员，被忽略
    server.remove_observer(observers[3].get());
    server.add_observer(&newcomer); // 顶替成功
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u);
    client.close();
    ASSERT_TRUE(wait_for_event_count(*observers[0], "ended", 1));

    EXPECT_EQ(newcomer.count("started"), 1u);
    EXPECT_EQ(observers[3]->count("started"), 0u); // 已注销
    EXPECT_EQ(rejected.count("started"), 0u);      // 满员时被忽略
    for (std::size_t i = 0; i < kMaxObservers; ++i) {
        if (i == 3) {
            continue;
        }
        EXPECT_EQ(observers[i]->count("started"), 1u) << "观察者 " << i;
    }

    server.stop();
}

TEST(TestServerObserver, DisconnectAndStopRaceDoesNotDuplicateEvents) {
    // 客户端断开后立刻 stop（会话收尾与 stop 并发）：started/attached/
    // released/ended 每个都恰好一次（remove_session 幂等 + 释放只发一次）
    asio::io_context io;
    StringPool string_pool;
    RecordingObserver observer;
    Server server;
    ServerStopper stopper(server);
    server.add_device(make_keyboard(string_pool));
    server.add_observer(&observer);
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u);
    client.close(); // 不等收尾
    server.stop();  // 立刻 stop，与收尾路径并发

    EXPECT_EQ(observer.count("started"), 1u);
    EXPECT_EQ(observer.count("attached"), 1u);
    EXPECT_EQ(observer.count("released"), 1u);
    EXPECT_EQ(observer.count("ended"), 1u);
}

TEST(TestServerObserver, CallbackCanAddObserver) {
    // 回调里注册新观察者：本次遍历用的是快照，不受影响；新观察者从
    // 之后的第一个事件开始收到通知
    class AddingObserver : public ServerObserver {
    public:
        Server *server = nullptr;
        ServerObserver *to_add = nullptr;
        std::atomic<bool> added{false};

        void on_session_started(std::uint64_t, const std::string &) override {
            if (!added.exchange(true)) {
                server->add_observer(to_add);
            }
        }
    };

    asio::io_context io;
    StringPool string_pool;
    AddingObserver adder;
    RecordingObserver latecomer;
    Server server;
    ServerStopper stopper(server);
    adder.server = &server;
    adder.to_add = &latecomer;
    server.add_device(make_keyboard(string_pool));
    server.add_observer(&adder);
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u);
    client.close();
    ASSERT_TRUE(wait_for_event_count(latecomer, "ended", 1));

    EXPECT_EQ(latecomer.count("started"), 0u); // 注册发生在 started 之后
    EXPECT_EQ(latecomer.count("attached"), 1u);
    EXPECT_EQ(latecomer.count("released"), 1u);
    EXPECT_EQ(latecomer.count("ended"), 1u);

    server.stop();
}