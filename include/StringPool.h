#pragma once

#include <shared_mutex>
#include <map>

#include <spdlog/spdlog.h>

namespace usbipdcpp {
    class StringPool {
    public:
        std::uint8_t new_string(const std::wstring &str) {
            std::lock_guard lock(string_pool_mutex);
            for (std::uint8_t index = 1; index < std::numeric_limits<std::uint8_t>::max(); index++) {
                if (!string_pool.contains(index)) {
                    string_pool[index] = str;
                    return index;
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

        void remove_string(std::uint8_t index) {
            std::lock_guard lock(string_pool_mutex);
            string_pool.erase(index);
        }

    private:
        std::map<std::uint8_t, std::wstring> string_pool;
        std::shared_mutex string_pool_mutex;
    };
}
