#pragma once

#include <cstdint>

#include "usbipdcpp/type.h"

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

// ==================== 标准 USB 描述符结构体 ====================
// USB 2.0 规范固定长度描述符。字段布局由规范硬性规定，
// 用 packed 结构体表示并用 static_assert 校验 sizeof 防止字段增删偏离规范。
// append_to 按字段序列化（多字节字段自动转小端），与平台字节序无关。

#pragma pack(push, 1)
/// 设备描述符（USB 2.0 Table 9-8，固定 18 字节）
struct DeviceDesc {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint16_t bcdUSB;
    std::uint8_t bDeviceClass;
    std::uint8_t bDeviceSubClass;
    std::uint8_t bDeviceProtocol;
    std::uint8_t bMaxPacketSize0;
    std::uint16_t idVendor;
    std::uint16_t idProduct;
    std::uint16_t bcdDevice;
    std::uint8_t iManufacturer;
    std::uint8_t iProduct;
    std::uint8_t iSerialNumber;
    std::uint8_t bNumConfigurations;

    void append_to(data_type &d) const {
        vector_append_to_le(d, bLength, bDescriptorType, bcdUSB, bDeviceClass, bDeviceSubClass, bDeviceProtocol,
                            bMaxPacketSize0, idVendor, idProduct, bcdDevice, iManufacturer, iProduct, iSerialNumber,
                            bNumConfigurations);
    }
};
static_assert(sizeof(DeviceDesc) == 18, "设备描述符固定 18 字节");

/// 配置描述符（USB 2.0 Table 9-10，固定 9 字节）
struct ConfigHeaderDesc {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint16_t wTotalLength;
    std::uint8_t bNumInterfaces;
    std::uint8_t bConfigurationValue;
    std::uint8_t iConfiguration;
    std::uint8_t bmAttributes;
    std::uint8_t bMaxPower;

    void append_to(data_type &d) const {
        vector_append_to_le(d, bLength, bDescriptorType, wTotalLength, bNumInterfaces, bConfigurationValue,
                            iConfiguration, bmAttributes, bMaxPower);
    }
};
static_assert(sizeof(ConfigHeaderDesc) == 9, "配置描述符固定 9 字节");

/// 接口关联描述符 IAD（USB 2.0 ECN，固定 8 字节）
struct IadDesc {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint8_t bFirstInterface;
    std::uint8_t bInterfaceCount;
    std::uint8_t bFunctionClass;
    std::uint8_t bFunctionSubClass;
    std::uint8_t bFunctionProtocol;
    std::uint8_t iFunction;

    void append_to(data_type &d) const {
        vector_append_to_le(d, bLength, bDescriptorType, bFirstInterface, bInterfaceCount, bFunctionClass,
                            bFunctionSubClass, bFunctionProtocol, iFunction);
    }
};
static_assert(sizeof(IadDesc) == 8, "IAD 描述符固定 8 字节");

/// 接口描述符（USB 2.0 Table 9-12，固定 9 字节）
struct InterfaceDesc {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint8_t bInterfaceNumber;
    std::uint8_t bAlternateSetting;
    std::uint8_t bNumEndpoints;
    std::uint8_t bInterfaceClass;
    std::uint8_t bInterfaceSubClass;
    std::uint8_t bInterfaceProtocol;
    std::uint8_t iInterface;

    void append_to(data_type &d) const {
        vector_append_to_le(d, bLength, bDescriptorType, bInterfaceNumber, bAlternateSetting, bNumEndpoints,
                            bInterfaceClass, bInterfaceSubClass, bInterfaceProtocol, iInterface);
    }
};
static_assert(sizeof(InterfaceDesc) == 9, "接口描述符固定 9 字节");

/// 端点描述符（USB 2.0 Table 9-13，固定 7 字节）
struct EndpointDesc {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint8_t bEndpointAddress;
    std::uint8_t bmAttributes;
    std::uint16_t wMaxPacketSize;
    std::uint8_t bInterval;

    void append_to(data_type &d) const {
        vector_append_to_le(d, bLength, bDescriptorType, bEndpointAddress, bmAttributes, wMaxPacketSize, bInterval);
    }
};
static_assert(sizeof(EndpointDesc) == 7, "端点描述符固定 7 字节");

/// BOS 描述符头（USB 3.0 Table 9-14 兼容 USB 2.0 设备，固定 5 字节）
struct BosHeaderDesc {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint16_t wTotalLength;
    std::uint8_t bNumCapabilities;

    void append_to(data_type &d) const {
        vector_append_to_le(d, bLength, bDescriptorType, wTotalLength, bNumCapabilities);
    }
};
static_assert(sizeof(BosHeaderDesc) == 5, "BOS 头固定 5 字节");

/// USB 2.0 Extension 设备能力描述符（固定 7 字节）
struct BosUsb20ExtCapDesc {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint8_t bDevCapabilityType;
    std::uint32_t bmAttributes;

    void append_to(data_type &d) const {
        vector_append_to_le(d, bLength, bDescriptorType, bDevCapabilityType, bmAttributes);
    }
};
static_assert(sizeof(BosUsb20ExtCapDesc) == 7, "USB 2.0 Extension 能力描述符固定 7 字节");
#pragma pack(pop)

} // namespace usbipdcpp
