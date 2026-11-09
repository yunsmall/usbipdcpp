#pragma once

#include <cstdint>

namespace usbipdcpp::e2e::proto {

/**
 * @brief e2e 设备的 VID
 */
inline constexpr std::uint16_t vid = 0x1d6b;

inline constexpr std::uint16_t pid_control = 0x0104;
inline constexpr std::uint16_t pid_bulk = 0x0105;
inline constexpr std::uint16_t pid_interrupt = 0x0106;
inline constexpr std::uint16_t pid_iso = 0x0107;

/**
 * @brief GET_INFO 返回的版本号
 */
inline constexpr std::uint32_t version = 0x00010000;

/**
 * @brief 设备自定义的 vendor 请求码
 *
 * 全部使用 device recipient（bmRequestType 0x40/0xC0），wIndex 用来
 * 选择端点地址；FunctionFS 需要打开 FUNCTIONFS_ALL_CTRL_RECIP 才会把
 * device recipient 的请求转给用户态。
 */
enum class Request : std::uint8_t {
    get_info = 0x10,        ///< IN：返回 4 字节版本号
    echo_in = 0x11,         ///< IN：返回 wLength 字节，wValue 低字节是种子
    echo_out = 0x12,        ///< OUT：接收 wLength 字节并记录长度/校验和
    get_last_out = 0x13,    ///< IN：返回 8 字节 {长度, 校验和}
    stall = 0x14,           ///< 任何方向：始终 stall
    short_in = 0x15,        ///< IN：只返回 min(wLength, wValue) 字节
    zero_in = 0x16,         ///< IN：wLength 非 0 但返回 0 字节
    delay_in = 0x17,        ///< IN：先睡 wValue 毫秒，再返回 1 字节
    delay_out = 0x18,       ///< OUT 无数据：先睡 wValue 毫秒再完成状态阶段
    stall_out_data = 0x19,  ///< OUT：带数据阶段时 stall
    set_in_enabled = 0x20,  ///< OUT 无数据：wValue=0/1，wIndex=端点地址
    set_reply_len = 0x21,   ///< OUT 无数据：wValue=长度，wIndex=端点地址
    get_out_stats = 0x22,   ///< IN：wIndex=端点地址，返回 OutStats
};

/**
 * @brief OUT 端点的统计信息，按小端序直接发送
 */
struct OutStats {
    std::uint32_t chunks;
    std::uint32_t bytes;
    std::uint32_t errors;
} __attribute__((packed));

/**
 * @brief iso 虚拟设备的 vendor 请求码（interface recipient）
 *
 * iso 设备是项目虚拟设备框架实现的（不是 FunctionFS），请求发给接口 0，
 * bmRequestType 为 0xC1（IN）/0x41（OUT）。
 */
enum class IsoRequest : std::uint8_t {
    get_info = 0x10,       ///< IN：返回 4 字节版本号
    get_iso_stats = 0x30,  ///< IN：返回 IsoStats
    reset_iso_stats = 0x31,///< OUT 无数据：清零统计
    set_in_delay = 0x32,   ///< OUT 无数据：wValue = IN 应答延迟（毫秒）
    set_fail_mask = 0x33,  ///< OUT 无数据：wValue 位图，第 i 位置 1 表示每个 URB 的第 i 包返回 EPIPE
};

/**
 * @brief iso 统计信息，按小端序直接发送
 */
struct IsoStats {
    std::uint32_t out_packets;
    std::uint32_t out_bytes;
    std::uint32_t out_errors;
    std::uint32_t in_packets;
} __attribute__((packed));

/**
 * @brief vendor 请求的 bmRequestType
 */
inline constexpr std::uint8_t request_type_in = 0xc0;
inline constexpr std::uint8_t request_type_out = 0x40;

/**
 * @brief iso 设备的 vendor 请求 bmRequestType（interface recipient）
 */
inline constexpr std::uint8_t iso_request_type_in = 0xc1;
inline constexpr std::uint8_t iso_request_type_out = 0x41;

}  // namespace usbipdcpp::e2e::proto
