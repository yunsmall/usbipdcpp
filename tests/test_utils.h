#pragma once

#include <asio.hpp>
#include <gtest/gtest.h>

#include "protocol.h"
#include "type.h"
#include "utils/utils.h"

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
