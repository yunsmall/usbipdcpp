#pragma once

#include <asio.hpp>

#include "protocol.h"
#include "type.h"

namespace usbipdcpp {
    namespace test {

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
                usbipdcpp::vector_append_to_net(buffer, (std::uint16_t) cmd);
                auto data = origin.to_bytes();
                sock.send(asio::buffer(data));
            });

            T received{};
            asio::ip::tcp::socket server_socket(io_context);
            asio::error_code ec;
            server_socket.connect(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), server_port), ec);

            asio::co_spawn(io_context, [&]()-> asio::awaitable<void> {
                auto version = co_await usbipdcpp::read_u16(server_socket);
                auto op_command = co_await usbipdcpp::read_u16(server_socket);
                co_await received.from_socket(server_socket);
                // SPDLOG_INFO("Received header from server");
                if constexpr (with_header<T>) {
                    received.header.command = op_command;
                }
                else {
                    received.command = op_command;
                }
                server_socket.close();
                io_context.stop();
            }, asio::detached);

            io_context.run();
            sender.join();

            return received;
        }

    }
}
