#pragma once

#include <cstdint>
#include <bit>

#include <asio/read.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/ip/tcp.hpp>

#include "type.h"

namespace usbipdcpp {

    constexpr bool is_little_endian() {
        if (std::is_constant_evaluated()) {
            return std::endian::native == std::endian::little;
        }
        else {
            std::uint16_t tmp = 0x1234u;
            return *reinterpret_cast<std::uint8_t *>(&tmp) != 0x12u;
        }
    }

    template<std::unsigned_integral T>
    constexpr T ntoh(T num) {
        if (is_little_endian()) {
            return std::byteswap(num);
        }
        return num;
    }

    template<std::unsigned_integral T>
    constexpr T hton(T num) {
        return ntoh<T>(num);
    }

    /**
     * @brief 全部用 sizeof 计算长度
     * @tparam Args 类型
     * @return 总长度
     */
    template<std::unsigned_integral... Args>
    constexpr std::size_t calculate_unsigned_integral_total_size() {
        return (sizeof(std::remove_reference_t<Args>) + ...);
    }

    /**
     * @brief 只能处理 unsigned_integral 类型和 array_data_type 类型。
     * 整数类型用sizeof计算，array 类型会用std::size计算
     * @tparam Args 类型
     * @return 总长度
     */
    template<typename... Args>
        requires (
            (std::unsigned_integral<std::remove_cvref_t<Args>> ||
             is_array_data_type<std::remove_cvref_t<Args>>)
            && ...)
    constexpr std::size_t calculate_total_size_with_array() {
        auto calc_type_size = []<typename T>()-> std::size_t {
            using RawType = std::remove_cvref_t<T>;
            if constexpr (is_array_data_type<RawType>) {
                return std::size(RawType{});
            }
            else {
                return sizeof(RawType);
            }
        };
        return (calc_type_size.template operator()<Args>() + ...);
    }

    /**
     * @brief calculate_total_size_with_array 的直接传参的类型自动推导版本。
     * 只能处理 unsigned_integral 类型和 array_data_type 类型。
     * 整数类型用sizeof计算，array 类型会用std::size计算
     * @param args 参数
     * @return 长度
     */
    template<typename... Args>
        requires (
            (std::unsigned_integral<std::remove_cvref_t<Args>> ||
             is_array_data_type<std::remove_cvref_t<Args>>)
            && ...)
    constexpr std::size_t calculate_data_total_size_with_array(const Args &... args) {
        return calculate_data_total_size_with_array<Args...>();
    }

    /**
     * @brief 只能处理 unsigned_integral 类型和 supported_data_type 类型，整数类型用sizeof计算，supported_data_type 类型会用std::size计算
     * @param arg 参数
     * @return 长度
     */
    template<typename... Args>
        requires (
            (std::unsigned_integral<std::remove_cvref_t<Args>> ||
             supported_data_type<std::remove_cvref_t<Args>>)
            && ...)
    constexpr std::size_t calculate_data_total_size_with_range(const Args &... arg) {
        auto calc_type_size = []<typename T>(const T &t)-> std::size_t {
            if constexpr (supported_data_type<T>) {
                return std::size(t);
            }
            else {
                return sizeof(T);
            }
        };
        return (calc_type_size(arg) + ...);
    }

    /**
     * @brief 从socket中读入整数，会一次性全读入然后赋值。读入后会调用 ntoh
     * @param sock 目标socket
     * @param args 整数
     */
    template<std::unsigned_integral... Args>
    asio::awaitable<void> unsigned_integral_read_from_socket(asio::ip::tcp::socket &sock, Args &... args) {
        constexpr auto total_size = calculate_unsigned_integral_total_size<Args...>();

        std::array<std::uint8_t, total_size> buffer;
        co_await asio::async_read(sock, asio::buffer(buffer), asio::use_awaitable);
        std::size_t offset = 0;

        auto process = [&](auto &arg) {
            using RawType = std::remove_reference_t<decltype(arg)>;
            RawType tmp;
            std::memcpy(&tmp, &buffer[offset], sizeof(tmp));
            offset += sizeof(tmp);

            arg = ntoh(tmp);
        };

        (process(args), ...);
    }

    /**
     * @brief 只能处理 unsigned_integral 类型和 supported_data_type 类型。
     * 整数类型读取后调用 ntoh，supported_data_type 类型会直接读取。
     * 会挨个读取并写入
     * @tparam Args 所有类型
     * @param sock 目标socket
     * @param args 希望读入的数据
     */
    template<typename... Args>
        requires (
            (std::unsigned_integral<std::remove_cvref_t<Args>> ||
             supported_data_type<std::remove_cvref_t<Args>>)
            && ...)
    asio::awaitable<void> data_read_from_socket(asio::ip::tcp::socket &sock, Args &... args) {
        auto process = [&](auto &arg)-> asio::awaitable<void> {
            using RawType = std::remove_reference_t<decltype(arg)>;
            if constexpr (supported_data_type<RawType>) {
                co_await asio::async_read(sock, asio::buffer(arg), asio::use_awaitable);
            }
            else {
                RawType tmp;
                co_await asio::async_read(sock, asio::buffer(&tmp, sizeof(tmp)), asio::use_awaitable);
                arg = ntoh(tmp);
            }
        };
        (co_await process(args), ...);
    }

    inline asio::awaitable<std::uint64_t> read_u64(asio::ip::tcp::socket &sock) {
        std::uint64_t result;
        co_await asio::async_read(sock, asio::buffer(&result, sizeof(result)), asio::use_awaitable);
        co_return ntoh(result);
    }

    inline asio::awaitable<std::uint32_t> read_u32(asio::ip::tcp::socket &sock) {
        std::uint32_t result;
        co_await asio::async_read(sock, asio::buffer(&result, sizeof(result)), asio::use_awaitable);
        co_return ntoh(result);
    }

    inline asio::awaitable<std::uint16_t> read_u16(asio::ip::tcp::socket &sock) {
        std::uint16_t result;
        co_await asio::async_read(sock, asio::buffer(&result, sizeof(result)), asio::use_awaitable);
        co_return ntoh(result);
    }

    inline asio::awaitable<std::uint8_t> read_u8(asio::ip::tcp::socket &sock) {
        std::uint8_t result;
        co_await asio::async_read(sock, asio::buffer(&result, sizeof(result)), asio::use_awaitable);
        co_return ntoh(result);
    }

    /**
     * @brief 给数组添加padding
     * @tparam padding
     * @param array 源数组
     * @return padding后的数组
     */
    template<
        std::size_t padding,
        is_array_data_type Array,
        std::size_t total_size = calculate_total_size_with_array<Array>() + padding
    >
    constexpr array_data_type<total_size> array_add_padding(const Array &array) {
        array_data_type<total_size> result;
        std::memcpy(result.data(), array.data(), std::size(array));
        return result;
    }

    /**
     * @brief 只能处理 unsigned_integral 类型和 array_data_type 类型。
     * 整数类型会按网络字节序储存，array 类型会直接内存复制。整数会调用 hton
     * @tparam Args 传入数据的类型
     * @param args 要转换的数据
     * @return 创建的数组
     */
    template<typename... Args, std::size_t total_size = calculate_total_size_with_array<Args...>()>
        requires (
            (std::unsigned_integral<std::remove_cvref_t<Args>> ||
             is_array_data_type<std::remove_cvref_t<Args>>)
            && ...)
    array_data_type<total_size> to_network_array(const Args &... args) {
        // 创建缓冲区
        array_data_type<total_size> buffer;

        // 处理每个参数
        std::size_t offset = 0;
        auto process = [&](auto &&arg) {
            using RawType = std::remove_cvref_t<decltype(arg)>;

            if constexpr (is_array_data_type<RawType>) {
                std::memcpy(buffer.data() + offset, arg.data(), arg.size());
                offset += arg.size();
            }
            else {
                const RawType net_value = hton(arg);
                // 复制数据到缓冲区
                std::memcpy(buffer.data() + offset, &net_value, sizeof(RawType));
                offset += sizeof(RawType);
            }
        };
        // 展开所有参数
        (process(args), ...);

        return buffer;
    }

    /**
     * @brief 只能处理 unsigned_integral 类型和 supported_data_type 类型。
     * 整数类型会按网络字节序储存，range 类型会直接内存复制。整数会调用 hton
     * @tparam Args 传入数据的类型
     * @param args 要转换的数据
     * @return 创建的数组
     */
    template<typename... Args>
        requires (
            (std::unsigned_integral<std::remove_cvref_t<Args>> ||
             supported_data_type<std::remove_cvref_t<Args>>)
            && ...)
    std::vector<std::uint8_t> to_network_data(const Args &... args) {
        // 计算总缓冲区大小
        std::size_t total_size = calculate_data_total_size_with_range(args...);

        // 创建缓冲区
        data_type buffer;
        buffer.resize(total_size);

        // 处理每个参数
        std::size_t offset = 0;
        auto process = [&](auto &&arg) {
            using RawType = std::remove_cvref_t<decltype(arg)>;

            if constexpr (supported_data_type<RawType>) {
                memcpy(buffer.data() + offset, arg.data(), arg.size());
                offset += std::size(arg);
            }
            else {
                const RawType net_value = hton(arg);
                // 复制数据到缓冲区
                std::memcpy(buffer.data() + offset, &net_value, sizeof(RawType));
                offset += sizeof(RawType);
            }
        };

        // 展开所有参数
        (process(args), ...);

        return buffer;
    }

    /**
     * @brief 只能处理 unsigned_integral 类型和 supported_data_type 类型。
     * 整数类型会按本地字节序储存，range 类型会直接内存复制。
     * @tparam Args
     * @param vec 目标vector
     * @param args 整数引用
     */
    template<typename... Args>
        requires (
            (std::unsigned_integral<std::remove_cvref_t<Args>> ||
             supported_data_type<std::remove_cvref_t<Args>>)
            && ...)
    void vector_mem_order_append(data_type &vec, const Args &... args) {
        // 计算总缓冲区大小
        std::size_t total_size = calculate_data_total_size_with_range(args...);

        // 扩充缓冲区
        std::size_t offset = vec.size();
        vec.resize(vec.size() + total_size);

        // 处理每个参数
        auto process = [&](auto &&arg) {
            using RawType = std::remove_cvref_t<decltype(arg)>;
            if constexpr (supported_data_type<RawType>) {
                memcpy(vec.data() + offset, arg.data(), arg.size());
                offset += std::size(arg);
            }
            else {
                // 复制数据到缓冲区
                std::memcpy(vec.data() + offset, &arg, sizeof(RawType));
                offset += sizeof(RawType);
            }
        };

        // 展开所有参数
        (process(args), ...);
    }

    /**
     * @brief 只能处理 unsigned_integral 类型和 supported_data_type 类型。
     * 整数类型会按网络字节序储存，range 类型会直接内存复制。整数会调用 hton
     * @tparam Args
     * @param vec 目标vector
     * @param args 整数引用
     */
    template<typename... Args>
        requires (
            (std::unsigned_integral<std::remove_cvref_t<Args>> ||
             supported_data_type<std::remove_cvref_t<Args>>)
            && ...)
    void vector_append_to_net(data_type &vec, const Args &... args) {
        // 计算总缓冲区大小
        std::size_t total_size = calculate_data_total_size_with_range(args...);

        // 扩充缓冲区
        std::size_t offset = vec.size();
        vec.resize(vec.size() + total_size);

        // 处理每个参数
        auto process = [&](auto &&arg) {
            using RawType = std::remove_cvref_t<decltype(arg)>;
            if constexpr (supported_data_type<RawType>) {
                memcpy(vec.data() + offset, arg.data(), arg.size());
                offset += std::size(arg);
            }
            else {
                const RawType net_value = hton(arg);
                // 复制数据到缓冲区
                std::memcpy(vec.data() + offset, &net_value, sizeof(RawType));
                offset += sizeof(RawType);
            }
        };

        // 展开所有参数
        (process(args), ...);
    }


    template<typename T>
    concept Serializable = requires(T &&t1, T &&t2, asio::ip::tcp::socket &sock)
    {
        { static_cast<std::remove_cvref_t<T>>(t1) == static_cast<std::remove_cvref_t<T>>(t2) } -> std::same_as<bool>;
        { static_cast<const std::remove_cvref_t<T> &>(t1).to_bytes() } -> supported_data_type;
        { static_cast<std::remove_cvref_t<T>>(t1).from_socket(sock) } -> std::same_as<asio::awaitable<void>>;
    };

}
