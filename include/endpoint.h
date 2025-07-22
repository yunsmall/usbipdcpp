#pragma once

#include "constant.h"

#include <cstdint>

namespace usbipdcpp {
    struct UsbEndpoint {
        enum Direction {
            In,
            Out
        };

        std::uint8_t address = 0;
        std::uint8_t attributes = 0;
        std::uint16_t max_packet_size = 0;
        std::uint8_t interval = 0;


        [[nodiscard]] Direction direction() const {
            //高位为1则是输入
            if ((address & 0b10000000) != 0) {
                return Direction::In;
            }
            else {
                return Direction::Out;
            }
        }

        [[nodiscard]] bool is_in() const {
            return direction() == Direction::In;
        }

        [[nodiscard]] bool is_ep0() const {
            return (address & 0x7F) == 0;
        }

        static UsbEndpoint get_ep0_in(std::uint16_t max_packet_size) {
            return {
                    .address = 0x00,
                    .attributes = static_cast<std::uint8_t>(EndpointAttributes::Control),
                    .max_packet_size = max_packet_size,
                    .interval = 0
            };
        }

        static UsbEndpoint get_default_ep0_in() {
            return get_ep0_in(EP0_MAX_PACKET_SIZE);
        }

        static UsbEndpoint get_ep0_out(std::uint16_t max_packet_size) {
            return {
                    .address = 0x80,
                    .attributes = static_cast<std::uint8_t>(EndpointAttributes::Control),
                    .max_packet_size = max_packet_size,
                    .interval = 0
            };
        }

        static UsbEndpoint get_default_ep0_out() {
            return get_ep0_out(EP0_MAX_PACKET_SIZE);
        }
    };
}
