#pragma once

#include <cstdint>

namespace usbipdcpp {

// ==================== USB 类特定控制请求码 ====================
// 视频/音频等各设备类通用（UVC 1.5 §4.2 / UAC 1.0 §5.2.3）
constexpr std::uint8_t RC_UNDEFINED = 0x00;
constexpr std::uint8_t SET_CUR = 0x01;
constexpr std::uint8_t SET_CUR_ALL = 0x11;
constexpr std::uint8_t GET_CUR = 0x81;
constexpr std::uint8_t GET_MIN = 0x82;
constexpr std::uint8_t GET_MAX = 0x83;
constexpr std::uint8_t GET_RES = 0x84;
constexpr std::uint8_t GET_LEN = 0x85; // Video 类命名；Audio 规范中称 GET_MEM
constexpr std::uint8_t GET_INFO = 0x86;
constexpr std::uint8_t GET_DEF = 0x87;

// ==================== USB 类特定描述符类型 ====================
constexpr std::uint8_t CS_INTERFACE = 0x24;
constexpr std::uint8_t CS_ENDPOINT = 0x25;

} // namespace usbipdcpp
