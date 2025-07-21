#pragma once

#include <cstdint>
#include <vector>
#include <ranges>
#include <string>
#include <sstream>
#include <format>

namespace usbipcpp {
    using error_code = std::error_code;
    using data_type = std::vector<std::uint8_t>;

    template<typename T>
    concept supported_data_type = requires(T &t)
    {
        requires std::ranges::range<T>;
        requires std::same_as<std::decay_t<decltype(*begin(t))>, std::uint8_t>;
        requires std::same_as<std::decay_t<decltype(*end(t))>, std::uint8_t>;
    };

    template<supported_data_type T>
    std::string get_every_byte(const T &t) {
        std::stringstream ss;
        for (auto b: t) {
            ss << std::format("{:02x} ", b);
        }
        return ss.str();
    }
}
