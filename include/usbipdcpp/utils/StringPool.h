#pragma once

#include <limits>
#include <optional>
#include <shared_mutex>
#include <system_error>
#include <unordered_map>

#include <spdlog/spdlog.h>

namespace usbipdcpp {
class StringPool {
public:
    std::uint8_t new_string(const std::wstring &str) {
        std::lock_guard lock(string_pool_mutex);
        // 1..255 全部可分配（0 是保留的 Language ID 索引）；
        // 循环变量用 uint16_t：uint8_t 在 255 处自增会回绕成 0，条件将永不满足
        for (std::uint16_t index = 1; index <= std::numeric_limits<std::uint8_t>::max(); ++index) {
            if (!string_pool.contains(static_cast<std::uint8_t>(index))) {
                string_pool[static_cast<std::uint8_t>(index)] = str;
                return static_cast<std::uint8_t>(index);
            }
        }
        SPDLOG_CRITICAL("字符串池用完了");
        throw std::system_error(std::make_error_code(std::errc::no_buffer_space));
    }

    std::optional<std::wstring> get_string(std::uint8_t index) {
        std::shared_lock lock(string_pool_mutex);
        if (!string_pool.contains(index)) {
            return std::nullopt;
        }
        return string_pool[index];
    }

    void change_string(std::uint8_t index, const std::wstring &new_str) {
        std::lock_guard lock(string_pool_mutex);
        if (!string_pool.contains(index)) {
            SPDLOG_CRITICAL("字符串索引 {} 无效", index);
            throw std::system_error(std::make_error_code(std::errc::invalid_argument));
        }
        string_pool[index] = new_str;
    }

    void remove_string(std::uint8_t index) {
        std::lock_guard lock(string_pool_mutex);
        string_pool.erase(index);
    }

private:
    std::unordered_map<std::uint8_t, std::wstring> string_pool;
    std::shared_mutex string_pool_mutex;
};
}
