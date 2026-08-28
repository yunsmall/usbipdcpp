#pragma once

#include <bit>
#include <cstdint>
#include <cstring>
#include <exception>
#include <type_traits>

#include "usbipdcpp/type.h"

namespace usbipdcpp {

/**
 * @brief 环形序号比较（RFC 1982，TCP 序号同款）：a 是否比 b 新
 *
 * uint32_t 的 seqnum 在长时间运行下会回绕（如内核 vhci 的全局递增
 * seqnum），回绕后简单的 < 比较会取错顺序；本函数在比较距离 < 2^31
 * 内判定可靠（参与比较的挂起请求数不可能达到这个量级）
 */
inline bool seqnum_newer(std::uint32_t a, std::uint32_t b) {
    return static_cast<std::int32_t>(a - b) > 0;
}

inline void if_has_value_than_rethrow(std::exception_ptr e) {
    if (e)
        std::rethrow_exception(e);
}

/**
 * @brief 判断当前平台是否为小端（编译期已知时 consteval，否则运行时探测）
 */
constexpr bool is_little_endian() {
    if consteval {
        return std::endian::native == std::endian::little;
    }
    else {
        std::uint16_t tmp = 0x1234u;
        return *reinterpret_cast<std::uint8_t *>(&tmp) != 0x12u;
    }
}

/**
 * @brief host -> USB 线格式（小端）。
 * USB 描述符与音频/视频载荷中的多字节字段均为小端，与网络字节序相反。
 * std::endian::native 为编译期常量，if constexpr 直接消掉无关分支。
 * 小端平台上原样返回，大端平台上字节交换，保证序列化结果与平台无关。
 */
template<std::unsigned_integral T>
constexpr T htole(T num) {
    if constexpr (std::endian::native == std::endian::little) {
        return num;
    }
    else {
        return std::byteswap(num);
    }
}

/**
 * @brief 按 USB 线格式（小端）把字段依次追加到 data_type。
 * 只能处理 unsigned_integral 类型和 supported_data_type 类型：
 * 整数类型调用 htole 后按 sizeof 追加，range 类型直接内存复制。
 * 先预计算总大小并一次 resize，再按 offset 写入，避免反复扩容。
 * 用于描述符构建，与 network.h 的 vector_append_to_net（大端）相对。
 * @tparam Args 传入数据的类型
 * @param vec 目标 vector
 * @param args 要追加的字段
 */
template<typename... Args>
    requires((std::unsigned_integral<std::remove_cvref_t<Args>> || supported_data_type<std::remove_cvref_t<Args>>) &&
             ...)
void vector_append_to_le(data_type &vec, const Args &...args) {
    // 预计算总大小
    std::size_t total_size = 0;
    auto calc_size = [&](const auto &arg) {
        using RawType = std::remove_cvref_t<decltype(arg)>;
        if constexpr (supported_data_type<RawType>) {
            total_size += std::size(arg);
        }
        else {
            total_size += sizeof(RawType);
        }
    };
    (calc_size(args), ...);

    // 一次扩容到位
    std::size_t offset = vec.size();
    vec.resize(vec.size() + total_size);

    // 按 offset 依次写入
    auto process = [&](const auto &arg) {
        using RawType = std::remove_cvref_t<decltype(arg)>;
        if constexpr (supported_data_type<RawType>) {
            std::memcpy(vec.data() + offset, arg.data(), std::size(arg));
            offset += std::size(arg);
        }
        else {
            const RawType le_value = htole(arg);
            std::memcpy(vec.data() + offset, &le_value, sizeof(RawType));
            offset += sizeof(RawType);
        }
    };
    (process(args), ...);
}
}
