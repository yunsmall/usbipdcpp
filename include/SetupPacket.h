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

        //转成小端
        [[nodiscard]] data_type to_bytes() const {
            data_type result(8, 0);

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

        [[nodiscard]] asio::awaitable<void> from_socket(asio::ip::tcp::socket &sock) {
            std::array<std::uint8_t, 8> setup{};
            co_await asio::async_read(sock, asio::buffer(setup), asio::use_awaitable);
            *this = parse(setup);
        }

        bool operator==(const SetupPacket &other) const = default;

        /**
         * @brief 从字节数组中解析setup包，请万分注意，setup包使用小端序传输
         * @param setup 字节数组
         * @return 解析后的setup包
         */
        static SetupPacket parse(const std::array<std::uint8_t, 8> &setup) {
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
            uint8_t recip = calc_recipient();
            return request == static_cast<std::uint8_t>(StandardRequest::ClearFeature) &&
                   recip == static_cast<std::uint8_t>(RequestRecipient::Endpoint) &&
                   value == USB_ENDPOINT_HALT;
        }

        [[nodiscard]] bool is_set_interface_cmd() const {
            uint8_t recip = calc_recipient();
            return request == static_cast<std::uint8_t>(StandardRequest::SetInterface) &&
                   recip == static_cast<std::uint8_t>(RequestRecipient::Interface);
        }

        [[nodiscard]] bool is_set_configuration_cmd() const {
            uint8_t recip = calc_recipient();
            return request == static_cast<std::uint8_t>(StandardRequest::SetConfiguration) &&
                   recip == static_cast<std::uint8_t>(RequestRecipient::Device);
        }

        bool is_out() const {
            return !(request_type & 0x80) || !length;
        }


        [[nodiscard]] bool is_reset_device_cmd() const {
            uint8_t request_type = calc_request_type();
            uint8_t recip = calc_recipient();


            if ((request == static_cast<std::uint8_t>(StandardRequest::SetFeature)) &&
                (request_type == static_cast<std::uint8_t>(RequestType::Class)) &&
                (recip == static_cast<std::uint8_t>(RequestRecipient::Other)) &&
                (value == static_cast<std::uint8_t>(PortFeat::Reset))) {
                // usbip_dbg_stub_rx("reset_device_cmd, port %u", index);
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
