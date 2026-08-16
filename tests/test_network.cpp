#include <gtest/gtest.h>

#include <chrono>
#include <thread>

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

        // start 的 bind 在网络线程中异步执行，轮询连接直到监听就绪。
        // 超时给 4 秒：覆盖率插桩等慢速构建下服务器启动可能超过 1 秒
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
