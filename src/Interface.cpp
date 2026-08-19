#include "usbipdcpp/Interface.h"

std::vector<std::uint8_t> usbipdcpp::UsbInterface::to_bytes() const {
    // 协议中 OP_REP_DEVLIST 的接口结构就是 4 字节（类/子类/协议/padding），
    // 不含端点描述符：内核 usbip_common.h 的 struct usbip_usb_interface 只有
    // 这四个字段，usbipd 的 send_reply_devlist 也只发送该结构。端点在导入后
    // 由客户端通过控制传输（GET_DESCRIPTOR）自行获取，与设备列表无关
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
