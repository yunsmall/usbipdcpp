#pragma once

#include <cstdint>

#include "usbipdcpp/Endpoint.h"
#include "usbipdcpp/Export.h"
#include "usbipdcpp/network.h"

namespace usbipdcpp {
class VirtualInterfaceHandler;

struct USBIPDCPP_API UsbInterface {
    std::uint8_t interface_class;
    std::uint8_t interface_subclass;
    std::uint8_t interface_protocol;

    /// USB 规范接口号（bInterfaceNumber），SET_INTERFACE 的 wIndex 用它匹配。
    /// 默认 0 与数组下标一致（虚拟设备接口从 0 连续编号）；
    /// libusb 后端绑定真实设备时按配置描述符填充（跳号设备下标≠接口号）
    std::uint8_t interface_number = 0;

    std::uint8_t current_altsetting = 0;

    /// 所有 alternate setting 的端点列表，[altsetting] = 该 alt 的端点
    std::vector<std::vector<UsbEndpoint>> endpoints;

    [[nodiscard]] const std::vector<UsbEndpoint> &current_endpoints() const {
        return endpoints[current_altsetting < endpoints.size() ? current_altsetting : 0];
    }
    std::vector<UsbEndpoint> &current_endpoints() {
        return endpoints[current_altsetting < endpoints.size() ? current_altsetting : 0];
    }

    std::shared_ptr<VirtualInterfaceHandler> handler;

    template<typename T, typename... Args>
    std::shared_ptr<T> with_handler(Args &&...args) {
        auto new_handler = std::make_shared<T>(*this, std::forward<Args>(args)...);
        handler = std::dynamic_pointer_cast<VirtualInterfaceHandler>(new_handler);
        return new_handler;
    }

    [[nodiscard]] std::vector<std::uint8_t> to_bytes() const;
    void from_socket(asio::ip::tcp::socket &sock);

    bool operator==(const UsbInterface &other) const {
        return interface_class == other.interface_class && interface_subclass == other.interface_subclass &&
               interface_protocol == other.interface_protocol;
    };
};

static_assert(Serializable<UsbInterface>);
} // namespace usbipdcpp
