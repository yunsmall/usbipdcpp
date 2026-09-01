#pragma once

#include <array>
#include <cstdint>

#include "usbipdcpp/type.h"
#include "usbipdcpp/utils/utils.h"

namespace usbipdcpp {
// Microsoft OS 1.0 描述符：字符串索引 0xEE 签名串（"MSFT100"）+ Compatible ID
// 特征描述符。对齐内核 gadget 的 configfs os_desc 支持（composite.c 的
// usb_os_string / fill_ext_compat）；Windows 依赖它给部分设备类（如 RNDIS
// 网卡）加载内置驱动

/// MS OS 1.0 签名串的字符串索引（GET_DESCRIPTOR String 的 index）
inline constexpr std::uint8_t MS_OS_STRING_INDEX = 0xEE;
/// Compatible ID 特征描述符的 wIndex（Vendor 请求）
inline constexpr std::uint16_t MS_OS_COMPAT_ID_WINDEX = 0x0004;
/// bMS_VendorCode 默认值（内核 configfs os_desc 惯例，见 os_desc/b_vendor_code）
inline constexpr std::uint8_t MS_OS_DEFAULT_VENDOR_CODE = 0x01;
/// "MSFT100" 的 UTF-16LE 编码（14 字节，固定，微软 OS 描述符规范规定）
inline constexpr std::array<std::uint8_t, 14> MS_OS_SIGNATURE_UTF16LE = {
        0x4D, 0x00, 0x53, 0x00, 0x46, 0x00, 0x54, 0x00, 0x31, 0x00, 0x30, 0x00, 0x30, 0x00,
};

// ==================== 固定长度描述符结构体 ====================
// 长度由微软 OS 描述符规范硬性规定，用 packed 结构体表示，static_assert 校验。

#pragma pack(push, 1)
/// MS OS 1.0 签名串（字符串索引 0xEE，固定 18 字节）
struct MsOsStringDesc {
    std::uint8_t b_length; // 18
    std::uint8_t b_descriptor_type; // 0x03 STRING
    std::array<std::uint8_t, 14> qw_signature; // "MSFT100" UTF-16LE
    std::uint8_t b_ms_vendor_code; // Compatible ID 请求的请求号
    std::uint8_t b_pad; // 0

    void append_to(data_type &d) const {
        vector_append_to_le(d, b_length, b_descriptor_type, qw_signature, b_ms_vendor_code, b_pad);
    }
};
static_assert(sizeof(MsOsStringDesc) == 18, "MS OS 签名串固定 18 字节");

/// MS OS 1.0 Compatible ID 特征描述符头（固定 16 字节）
struct MsOsCompatIdHeader {
    std::uint32_t dw_length; // 16 + 24×功能节数
    std::uint16_t bcd_version; // 0x0100
    std::uint16_t w_index; // MS_OS_COMPAT_ID_WINDEX
    std::uint8_t b_count; // 功能节数量
    std::array<std::uint8_t, 7> reserved;

    void append_to(data_type &d) const {
        vector_append_to_le(d, dw_length, bcd_version, w_index, b_count, reserved);
    }
};
static_assert(sizeof(MsOsCompatIdHeader) == 16, "MS OS Compatible ID 头固定 16 字节");

/// MS OS 1.0 Compatible ID 功能节（每接口固定 24 字节）
struct MsOsCompatIdSection {
    std::uint8_t b_first_interface; // 接口号（RNDIS 控制接口 0）
    std::uint8_t reserved1; // 内核 fill_ext_compat 固定写 0x01
    std::array<std::uint8_t, 8> compatible_id; // "RNDIS" 等，短则补 0
    std::array<std::uint8_t, 8> sub_compatible_id; // 全 0
    std::array<std::uint8_t, 6> reserved2; // 全 0

    void append_to(data_type &d) const {
        vector_append_to_le(d, b_first_interface, reserved1, compatible_id, sub_compatible_id, reserved2);
    }
};
static_assert(sizeof(MsOsCompatIdSection) == 24, "MS OS Compatible ID 功能节固定 24 字节");
#pragma pack(pop)
} // namespace usbipdcpp
