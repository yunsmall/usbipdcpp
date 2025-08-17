#pragma once

#include <cstdint>
#include <vector>
#include <ranges>
#include <string>
#include <sstream>
#include <format>

namespace usbipdcpp {
    using error_code = std::error_code;
    using data_type = std::vector<std::uint8_t>;

    template<std::size_t N>
    using array_data_type = std::array<std::uint8_t, N>;

    template<typename T>
    struct is_array_data_type_t {
        static constexpr bool value = false;
    };

    template<std::size_t N>
    struct is_array_data_type_t<std::array<std::uint8_t, N>> {
        static constexpr bool value = true;
    };

    template<typename T>
    concept is_array_data_type = is_array_data_type_t<T>::value;

    template<typename T>
    concept supported_data_type = requires(T &t)
    {
        requires std::ranges::range<T>;
        requires std::same_as<std::remove_cvref_t<decltype(*begin(t))>, std::uint8_t>;
        requires std::same_as<std::remove_cvref_t<decltype(*end(t))>, std::uint8_t>;
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
