#pragma once

#include <asio.hpp>
#include <gtest/gtest.h>

#include "crash_handler.h"

#include <chrono>
#include <thread>

#include "usbipdcpp/Server.h"
#include "usbipdcpp/protocol.h"
#include "usbipdcpp/type.h"
#include "usbipdcpp/utils/utils.h"

namespace usbipdcpp {
namespace test {

// 测试专用比较函数
inline void expect_header_equal(const UsbIpHeaderBasic &actual, const UsbIpHeaderBasic &expected) {
    EXPECT_EQ(actual.command, expected.command);
    EXPECT_EQ(actual.seqnum, expected.seqnum);
    EXPECT_EQ(actual.devid, expected.devid);
    EXPECT_EQ(actual.direction, expected.direction);
    EXPECT_EQ(actual.ep, expected.ep);
}

inline void expect_cmd_unlink_equal(const UsbIpCommand::UsbIpCmdUnlink &actual,
                                     const UsbIpCommand::UsbIpCmdUnlink &expected) {
    expect_header_equal(actual.header, expected.header);
    EXPECT_EQ(actual.unlink_seqnum, expected.unlink_seqnum);
}

    template<typename T>
    concept with_header = requires(T &&t)
    {
        std::forward<T>(t).header;
    };

// ---- 网络测试公共工具 ----

// 探测一个空闲端口。固定端口才能验证多次 start 的监听不冲突
inline std::uint16_t probe_free_port(asio::io_context &io) {
    asio::ip::tcp::acceptor probe(io);
    probe.open(asio::ip::tcp::v4());
    probe.bind(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    return probe.local_endpoint().port();
}

// 连接服务器，轮询等待监听就绪。失败时重建 socket 重试，返回是否连接成功
inline bool connect_with_retry(asio::ip::tcp::socket &client, const asio::ip::tcp::endpoint &ep) {
    for (int i = 0; i < 200; i++) {
        std::error_code ec;
        client.connect(ep, ec);
        if (!ec) {
            return true;
        }
        client.close();
        client = asio::ip::tcp::socket(client.get_executor());
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

// 以 RST 方式断开连接（linger=0 时不发送 FIN）：模拟客户端异常掉线，
// 服务器侧读到的是连接重置错误而不是 EOF，走不同的错误分支
inline void rst_disconnect(asio::ip::tcp::socket &client) {
    std::error_code ec;
    client.set_option(asio::socket_base::linger(true, 0), ec);
    client.close();
}

// 轮询等待服务器上所有 session 退出（客户端断连后 session 线程在退出前
// 自行从列表中移除自身），超时返回 false
inline bool wait_sessions_gone(Server &server, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (server.get_session_count() != 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return server.get_session_count() == 0;
}

    template<usbipdcpp::Serializable T>
    T reread_from_socket_with_command(const T &origin, std::uint16_t cmd) {
        asio::io_context io_context;
        asio::ip::tcp::acceptor acceptor(io_context);
        auto server_endpoint = asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0);
        acceptor.open(server_endpoint.protocol());

        acceptor.bind(server_endpoint);
        acceptor.listen();

        auto server_port = acceptor.local_endpoint().port();

        std::thread sender([&]() {
            auto sock = acceptor.accept();
            usbipdcpp::data_type buffer;
            // 发送版本号 + 命令码
            usbipdcpp::vector_append_to_net(buffer, static_cast<std::uint16_t>(USBIP_VERSION));
            usbipdcpp::vector_append_to_net(buffer, (std::uint16_t) cmd);
            auto data = origin.to_bytes();
            sock.send(asio::buffer(data));
        });

        T received{};
        asio::ip::tcp::socket server_socket(io_context);
        asio::error_code ec;
        server_socket.connect(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), server_port), ec);

        [[maybe_unused]] auto version = usbipdcpp::read_u16(server_socket);
        auto op_command = usbipdcpp::read_u16(server_socket);
        received.from_socket(server_socket);
        // SPDLOG_INFO("Received header from server");
        if constexpr (with_header<T>) {
            received.header.command = op_command;
        }
        else {
            received.command = op_command;
        }
        server_socket.close();

        sender.join();

        return received;
    }

}
}
