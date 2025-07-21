#pragma once

#include <cstdint>

namespace usbipcpp {
    struct Version {
        Version(std::uint8_t major, std::uint8_t minor, std::uint8_t patch):
            major(major), minor(minor), patch(patch) {
        }

        Version(std::uint16_t bcd):
            major(bcd >> 8), minor((bcd >> 4) & 0xF), patch(bcd & 0xF) {
        }

        operator std::uint16_t() const {
            std::uint16_t bcd = 0;
            bcd |= static_cast<std::uint16_t>(major) << 8;
            bcd |= static_cast<std::uint16_t>(minor) << 4;
            bcd |= static_cast<std::uint16_t>(patch) << 0;
            return bcd;
        }

        std::uint8_t major;
        std::uint8_t minor;
        std::uint8_t patch;
    };
}
