#include "interface.h"

std::vector<std::uint8_t> usbipcpp::UsbInterface::to_bytes() const {
    std::vector<std::uint8_t> result(4, 0);
    result[0] = interface_class;
    result[1] = interface_subclass;
    result[2] = interface_protocol;
    return result;
}

asio::awaitable<void> usbipcpp::UsbInterface::from_socket(asio::ip::tcp::socket &sock) {
    interface_class = co_await read_u8(sock);
    interface_subclass = co_await read_u8(sock);
    interface_protocol = co_await read_u8(sock);
    auto padding = co_await read_u8(sock);
    co_return;
}
