#pragma once

#include <cstdint>

#include "endpoint.h"
#include "network.h"

namespace usbipdcpp {
    class VirtualInterfaceHandler;

    struct UsbInterface {
        std::uint8_t interface_class;
        std::uint8_t interface_subclass;
        std::uint8_t interface_protocol;

        std::vector<UsbEndpoint> endpoints;

        std::shared_ptr<VirtualInterfaceHandler> handler;

        template<typename T, typename... Args>
        void with_handler(Args &&... args) {
            handler = std::dynamic_pointer_cast<VirtualInterfaceHandler>(
                    std::make_shared<T>(*this, std::forward<Args>(args)...));
        }

        [[nodiscard]] std::vector<std::uint8_t> to_bytes() const;
        asio::awaitable<void> from_socket(asio::ip::tcp::socket &sock);

        bool operator==(const UsbInterface &other) const {
            return interface_class == other.interface_class &&
                   interface_subclass == other.interface_subclass &&
                   interface_protocol == other.interface_protocol;
        };
    };

    static_assert(Serializable<UsbInterface>);
}
