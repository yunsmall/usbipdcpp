#pragma once

#include <cstdint>
#include <array>
#include <sstream>

#include "network.h"
#include "constant.h"

namespace usbipdcpp {
struct SetupPacket {

    std::uint8_t request_type;
    std::uint8_t request;
    std::uint16_t value;
    std::uint16_t index;
    std::uint16_t length;

    using array_storage = array_data_type<8>;

    //转成小端
    [[nodiscard]] array_storage to_bytes() const {
        array_storage result;

        result[0] = request_type;
        result[1] = request;

        //小端，低位在低地址
        result[2] = value & 0xFF;
        result[3] = value >> 8;

        result[4] = index & 0xFF;
        result[5] = index >> 8;

        result[6] = length & 0xFF;
        result[7] = length >> 8;

        return result;
    }

    [[nodiscard]] asio::awaitable<void> from_socket_co(asio::ip::tcp::socket &sock) {
        array_storage setup{};
        co_await data_read_from_socket_co(sock, setup);
        *this = parse(setup);
    }

    void from_socket(asio::ip::tcp::socket &sock) {
        array_storage setup{};
        data_read_from_socket(sock, setup);
        *this = parse(setup);
    }

    bool operator==(const SetupPacket &other) const = default;

    /**
     * @brief 从字节数组中解析setup包，请万分注意，setup包使用小端序传输
     * @param setup 字节数组
     * @return 解析后的setup包
     */
    static SetupPacket parse(const array_storage &setup) {
        return {
                .request_type = setup[0],
                .request = setup[1],
                .value = static_cast<std::uint16_t>(
                    static_cast<std::uint16_t>(setup[3]) << 8 | static_cast<std::uint16_t>(setup[2])),
                .index = static_cast<std::uint16_t>(
                    static_cast<std::uint16_t>(setup[5]) << 8 | static_cast<std::uint16_t>(setup[4])),
                .length = static_cast<std::uint16_t>(
                    static_cast<std::uint16_t>(setup[7]) << 8 | static_cast<std::uint16_t>(setup[6])),
        };
    }

    [[nodiscard]] std::string to_string() const;

    static constexpr std::uint8_t USB_ENDPOINT_HALT = 0u;

    static constexpr std::uint8_t USB_TYPE_MASK = 0b01100000u;
    static constexpr std::uint8_t USB_RECIP_MASK = 0b00011111u;
    static constexpr std::uint8_t USB_DIR_MASK = 0b10000000u;

    [[nodiscard]] static uint8_t calc_request_type(uint8_t request_type) {
        return (request_type & USB_TYPE_MASK);
    }

    [[nodiscard]] uint8_t calc_request_type() const {
        return calc_request_type(request_type);
    }

    [[nodiscard]] static uint8_t calc_standard_request(uint8_t request) {
        return request;
    }

    [[nodiscard]] uint8_t calc_standard_request() const {
        return calc_standard_request(request);
    }

    [[nodiscard]] uint8_t calc_recipient() const {
        return (request_type & USB_RECIP_MASK);
    }

    [[nodiscard]] bool is_clear_halt_cmd() const {
        // Linux kernel: bRequestType == USB_RECIP_ENDPOINT (0x02)
        // 即 Standard(0x00) + Endpoint(0x02) + OUT(0x00) = 0x02
        // 必须完整检查，不能只看Recip
        return request == static_cast<std::uint8_t>(StandardRequest::ClearFeature) &&
               request_type == static_cast<std::uint8_t>(RequestRecipient::Endpoint) &&
               value == USB_ENDPOINT_HALT;
    }

    [[nodiscard]] bool is_set_interface_cmd() const {
        // Linux kernel: bRequestType == USB_RECIP_INTERFACE (0x01)
        // 即 Standard(0x00) + Interface(0x01) + OUT(0x00) = 0x01
        return request == static_cast<std::uint8_t>(StandardRequest::SetInterface) &&
               request_type == static_cast<std::uint8_t>(RequestRecipient::Interface);
    }

    [[nodiscard]] bool is_set_configuration_cmd() const {
        // Linux kernel: bRequestType == USB_RECIP_DEVICE (0x00)
        // 即 Standard(0x00) + Device(0x00) + OUT(0x00) = 0x00
        return request == static_cast<std::uint8_t>(StandardRequest::SetConfiguration) &&
               request_type == static_cast<std::uint8_t>(RequestRecipient::Device);
    }

    bool is_out() const {
        return !(request_type & 0x80) || !length;
    }

    std::uint8_t calc_ep0_address() const {
        if (is_out()) {
            return 0x80;
        }
        else {
            return 0x00;
        }
    }


    [[nodiscard]] bool is_reset_device_cmd() const {
        // Linux kernel: bRequestType == USB_RT_PORT (0x23)
        // 即 Class(0x20) + Other(0x03) + OUT(0x00) = 0x23
        // 必须完整检查bRequestType，不能分开检查Type和Recip而忽略Dir
        if ((request == static_cast<std::uint8_t>(StandardRequest::SetFeature)) &&
            (request_type == (static_cast<std::uint8_t>(RequestType::Class) |
                              static_cast<std::uint8_t>(RequestRecipient::Other))) &&
            (value == static_cast<std::uint8_t>(PortFeat::Reset))) {
            return true;
        }
        return false;
    }
};


static std::ostream &operator<<(std::ostream &os, const SetupPacket &setup) {
    os << "request_type:" << std::hex << static_cast<std::uint32_t>(setup.request_type) << std::endl;
    os << "request:" << std::hex << static_cast<std::uint32_t>(setup.request) << std::endl;
    os << "value:" << std::hex << setup.value << std::endl;
    os << "index:" << std::hex << setup.index << std::endl;
    os << "length:" << std::hex << setup.length;
    return os;
}

inline std::string SetupPacket::to_string() const {
    std::stringstream ss;
    ss << *this;
    return ss.str();
}

static_assert(Serializable<SetupPacket>);

}
