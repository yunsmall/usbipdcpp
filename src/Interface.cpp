#include "Interface.h"

std::vector<std::uint8_t> usbipdcpp::UsbInterface::to_bytes() const {
    std::vector<std::uint8_t> result(4, 0);
    result[0] = interface_class;
    result[1] = interface_subclass;
    result[2] = interface_protocol;
    return result;
}

void usbipdcpp::UsbInterface::from_socket(asio::ip::tcp::socket &sock) {
    interface_class = read_u8(sock);
    interface_subclass = read_u8(sock);
    interface_protocol = read_u8(sock);
    [[maybe_unused]] auto padding = read_u8(sock);
}
