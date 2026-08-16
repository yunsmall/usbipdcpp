#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

#include "test_utils.h"

#include "usbipdcpp/Server.h"
#include "usbipdcpp/network.h"

using namespace usbipdcpp;
using namespace usbipdcpp::test;

TEST(TestNetwork, ServerCanRestartAfterStop) {
    // stop() 必须关闭 acceptor 释放端口，否则再次 start() 时 bind 同一端口失败
    asio::io_context io;

    // 探测一个空闲端口，固定端口才能验证两次 start 的监听不冲突
    std::uint16_t port;
    {
        asio::ip::tcp::acceptor probe(io);
        probe.open(asio::ip::tcp::v4());
        probe.bind(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
        port = probe.local_endpoint().port();
    }

    usbipdcpp::Server server;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), port);

    for (int round = 0; round < 2; round++) {
        server.start(ep);

        // acceptor 在 start() 内同步 bind，返回时监听已就绪，轮询仅为防御性
        // 等待。超时给 4 秒：覆盖率插桩等慢速构建下服务器启动可能超过 1 秒
        asio::ip::tcp::socket probe_sock(io);
        bool connected = false;
        for (int i = 0; i < 200; i++) {
            std::error_code ec;
            probe_sock.connect(ep, ec);
            if (!ec) {
                connected = true;
                break;
            }
            probe_sock.close();
            probe_sock = asio::ip::tcp::socket(io);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        ASSERT_TRUE(connected);
        // 主动断开，让服务器侧的 session 快速退出
        probe_sock.close();

        server.stop();
    }
}

TEST(TestNetwork, ServerCanStopWithoutConnection) {
    // start() 后没有任何客户端连接就 stop()：挂在 acceptor 上的 async_accept
    // 被 close 打断，协程要能干净退出，且可以再次 start()
    asio::io_context io;

    std::uint16_t port;
    {
        asio::ip::tcp::acceptor probe(io);
        probe.open(asio::ip::tcp::v4());
        probe.bind(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
        port = probe.local_endpoint().port();
    }

    usbipdcpp::Server server;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), port);

    for (int round = 0; round < 3; round++) {
        server.start(ep);
        // 不做任何连接，直接停止
        server.stop();
    }
}


TEST(TestNetwork, ntoh_hton) {
    if (is_little_endian()) {
        ASSERT_EQ(ntoh((uint64_t)0x1234567801020304llu), (uint64_t)0x0403020178563412llu);
        ASSERT_EQ(ntoh((uint32_t)0x12345678u), (uint32_t)0x78563412u);
        ASSERT_EQ(ntoh((uint16_t)0x1234u), (uint16_t)0x3412u);
        ASSERT_EQ(ntoh((uint8_t)0x12u), (uint8_t)0x12u);

        ASSERT_EQ(hton((uint64_t)0x1234567801020304llu), (uint64_t)0x0403020178563412llu);
        ASSERT_EQ(hton((uint32_t)0x12345678u), (uint32_t)0x78563412u);
        ASSERT_EQ(hton((uint16_t)0x1234u), (uint16_t)0x3412);
        ASSERT_EQ(hton((uint8_t)0x12u), (uint8_t)0x12);
    }
    else {
        ASSERT_EQ(ntoh((uint64_t)0x1234567801020304llu), (uint64_t)0x1234567801020304llu);
        ASSERT_EQ(ntoh((uint32_t)0x12345678u), (uint32_t)0x12345678u);
        ASSERT_EQ(ntoh((uint16_t)0x1234u), (uint16_t)0x1234u);
        ASSERT_EQ(ntoh((uint8_t)0x12u), (uint8_t)0x12u);

        ASSERT_EQ(hton((uint64_t)0x1234567801020304llu), (uint64_t)0x0403020178563412llu);
        ASSERT_EQ(hton((uint32_t)0x12345678u), (uint32_t)0x12345678u);
        ASSERT_EQ(hton((uint16_t)0x1234u), (uint16_t)0x1234u);
        ASSERT_EQ(hton((uint8_t)0x12u), (uint8_t)0x12u);
    }
}

TEST(TestNetwork, vector_append) {
    data_type data1;
    vector_mem_order_append(data1, static_cast<std::uint32_t>(0x01030405u),
                            static_cast<std::uint16_t>(0x1517u),
                            array_data_type<5>{0x01u, 0x02u, 0x05u, 0x09u, 0x11u},
                            data_type{0x10u, 0x21u, 0x34u, 0x57u},
                            static_cast<std::uint16_t>(0x0115u),
                            static_cast<std::uint8_t>(0x99u),
                            static_cast<std::uint64_t>(0x9998979695949392llu));
    if constexpr (is_little_endian()) {
        ASSERT_TRUE((
            data1==data_type{0x05u,0x04u,0x03u,0x01u,0x17u,0x15u,0x01u, 0x02u, 0x05u, 0x09u, 0x11u,0x10u, 0x21u, 0x34u,
            0x57u,0x15u,0x01u,0x99u,0x92u,0x93u,0x94u,0x95u,0x96u,0x97u,0x98u,0x99u}
        ));
    }
    else {
        ASSERT_TRUE((
            data1==data_type{ 0x01u, 0x03u, 0x04u, 0x05u, 0x15u, 0x17u, 0x01u, 0x02u, 0x05u, 0x09u, 0x11u, 0x10u, 0x21u,
            0x34u, 0x57u, 0x01u, 0x15u,0x99u,0x99u,0x98u,0x97u,0x96u,0x95u,0x94u,0x93u,0x92u }
        ));
    }


    data_type data2;
    vector_append_to_net(data2, static_cast<std::uint32_t>(0x01030405u),
                         static_cast<std::uint16_t>(0x1517u),
                         array_data_type<5>{0x01u, 0x02u, 0x05u, 0x09u, 0x11u},
                         data_type{0x10u, 0x21u, 0x34u, 0x57u},
                         static_cast<std::uint16_t>(0x0115u),
                         static_cast<std::uint8_t>(0x99u),
                         static_cast<std::uint64_t>(0x9998979695949392llu));
    ASSERT_TRUE((
        data2==data_type{ 0x01u, 0x03u, 0x04u, 0x05u, 0x15u, 0x17u, 0x01u, 0x02u, 0x05u, 0x09u, 0x11u,0x10u, 0x21u,
        0x34u, 0x57u, 0x01u, 0x15u,0x99u,0x99u,0x98u,0x97u,0x96u,0x95u,0x94u,0x93u,0x92u }
    ));
}

TEST(TestNetwork, to_network) {
    auto array = to_network_array(static_cast<std::uint32_t>(0x01030405u),
                                  static_cast<std::uint16_t>(0x1517),
                                  array_data_type<5>{0x01u, 0x02u, 0x05u, 0x09u, 0x11u},
                                  static_cast<std::uint16_t>(0x0115u),
                                  static_cast<std::uint8_t>(0x99u),
                                  static_cast<std::uint64_t>(0x9998979695949392llu));
    ASSERT_TRUE((
        array==decltype(array){ 0x01u, 0x03u, 0x04u, 0x05u, 0x15u, 0x17u, 0x01u, 0x02u, 0x05u, 0x09u, 0x11u, 0x01u,
        0x15u,0x99u,0x99u,0x98u,0x97u,0x96u,0x95u,0x94u,0x93u,0x92u }
    ));

    auto vec = to_network_data(static_cast<std::uint32_t>(0x01030405u),
                               static_cast<std::uint16_t>(0x1517u),
                               array_data_type<5>{0x01u, 0x02u, 0x05u, 0x09u, 0x11u},
                               data_type{0x10u, 0x21u, 0x34u, 0x57u},
                               static_cast<std::uint16_t>(0x0115u),
                               static_cast<std::uint8_t>(0x99u),
                               static_cast<std::uint64_t>(0x9998979695949392llu));
    ASSERT_TRUE((
        vec==data_type{ 0x01u, 0x03u, 0x04u, 0x05u, 0x15u, 0x17u, 0x01u, 0x02u, 0x05u, 0x09u, 0x11u,0x10u, 0x21u,
        0x34u, 0x57u, 0x01u, 0x15u,0x99u,0x99u,0x98u,0x97u,0x96u,0x95u,0x94u,0x93u,0x92u }
    ));
}

// ---------------------------------------------------------------------------
// 以下为 start/stop 循环（合法用法）下的刁钻场景
// ---------------------------------------------------------------------------

TEST(TestNetwork, ManyStartStopCycles) {
    // 无客户端情况下 100 轮 start/stop：acceptor 每轮都要干净释放端口，
    // 网络线程每轮都要干净退出，不能累积残留
    asio::io_context io;
    const std::uint16_t port = probe_free_port(io);
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), port);

    usbipdcpp::Server server;
    for (int round = 0; round < 100; round++) {
        server.start(ep);
        server.stop();
    }
}

TEST(TestNetwork, StopWithSilentClient) {
    // 客户端连接后不发任何数据静默挂着（session 挂在 parse_op 的阻塞读上），
    // 此时 stop()：immediately_stop 要打断挂起的读，session 干净退出。
    // 与导入设备后的传输态打断（ServerCanStopWithImportedDevice）互补
    asio::io_context io;
    const std::uint16_t port = probe_free_port(io);
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), port);

    usbipdcpp::Server server;
    server.start(ep);
    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, ep));

    server.stop(); // 客户端还静默挂着，直接 stop

    client.close();
}

TEST(TestNetwork, StopWithMultipleClients) {
    // 多个客户端同时挂着（多个 session 同时被打断），stop() 要并发打断并
    // join 所有 session 线程，任何一个都不能卡住
    asio::io_context io;
    const std::uint16_t port = probe_free_port(io);
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), port);

    usbipdcpp::Server server;
    server.start(ep);

    std::vector<asio::ip::tcp::socket> clients;
    for (int i = 0; i < 10; i++) {
        clients.emplace_back(io);
        ASSERT_TRUE(connect_with_retry(clients.back(), ep));
    }

    server.stop(); // 10 个 session 同时被打断

    for (auto &client: clients) {
        client.close();
    }
}

TEST(TestNetwork, DisconnectRightAfterDevlistRequest) {
    // 客户端发出 devlist 请求后不等回复立即断开：服务器可能正在收集设备
    // 列表或写回复时发现连接已断，session 要干净退出，不崩
    asio::io_context io;
    const std::uint16_t port = probe_free_port(io);
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), port);

    usbipdcpp::Server server;
    server.start(ep);

    for (int i = 0; i < 5; i++) {
        asio::ip::tcp::socket client(io);
        ASSERT_TRUE(connect_with_retry(client, ep));
        usbipdcpp::error_code send_ec;
        asio::write(client, asio::buffer(UsbIpCommand::OpReqDevlist{}.to_bytes()), send_ec);
        ASSERT_FALSE(send_ec);
        if (i % 2 == 0) {
            rst_disconnect(client); // 不读回复直接 RST
        }
        else {
            client.close(); // 不读回复直接 FIN
        }
    }
    ASSERT_TRUE(wait_sessions_gone(server));
    server.stop();
}

TEST(TestNetwork, StopDuringClientConnectRace) {
    // stop() 与客户端并发连接竞争：客户端线程反复尝试连接（server 停止期间
    // 连接会失败），主线程反复 start/stop。stop 时可能恰有连接刚被 accept、
    // session 刚创建，任何一轮都不能崩或卡死
    asio::io_context io;
    const std::uint16_t port = probe_free_port(io);
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), port);

    usbipdcpp::Server server;
    std::atomic_bool stop_flag = false;

    std::thread client_thread([&]() {
        while (!stop_flag) {
            asio::ip::tcp::socket client(io);
            std::error_code ec;
            client.connect(ep, ec);
            if (!ec) {
                client.close(); // 连上就断，模拟短暂的连接
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    for (int round = 0; round < 50; round++) {
        server.start(ep);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        server.stop();
    }

    stop_flag = true;
    client_thread.join();
}

TEST(TestNetwork, ManyQuickConnections) {
    // 客户端以各种方式快速连接并断开：优雅断开（FIN）、RST 重置、发垃圾
    // 数据后断开。服务器要全部处理干净（session 各自退出），之后 stop() 正常
    asio::io_context io;
    const std::uint16_t port = probe_free_port(io);
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), port);

    usbipdcpp::Server server;
    server.start(ep);

    for (int i = 0; i < 50; i++) {
        asio::ip::tcp::socket client(io);
        ASSERT_TRUE(connect_with_retry(client, ep));
        switch (i % 3) {
            case 0:
                client.close(); // 优雅断开（FIN）
                break;
            case 1:
                rst_disconnect(client); // 异常掉线（RST）
                break;
            case 2: {
                // 发一段垃圾数据再断开：服务器要么解析失败，要么读中断
                const std::array<std::uint8_t, 8> garbage = {0xFF, 0xFF, 0x01, 0x00, 0x00, 0x00, 0x00, 0x01};
                asio::write(client, asio::buffer(garbage));
                client.close();
                break;
            }
        }
    }

    ASSERT_TRUE(wait_sessions_gone(server));
    server.stop();
}
