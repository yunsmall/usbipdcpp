#pragma once

#include <cstdint>

#include "usbipdcpp/type.h"
#include "usbipdcpp/utils/utils.h"
#include "usbipdcpp/virtual_device/CdcAcmConstants.h"
#include "usbipdcpp/virtual_device/UsbClassConstants.h"

namespace usbipdcpp {
// ECM（CDC Ethernet Networking Control Model）类特定常量与描述符结构体。
// 参考 ECM 1.2 规范（ECM120）与内核 f_ecm.c；Header/Union 功能描述符是
// CDC 通用定义，直接复用 CdcAcmConstants.h 里的结构体

// ECM 类特定请求码（ECM120 §6.2 表 6）
enum class EcmRequest {
    SetEthernetMulticastFilters = 0x40,
    SetEthernetPowerManagementPatternFilter = 0x41,
    GetEthernetPowerManagementPatternFilter = 0x42,
    SetEthernetPacketFilter = 0x43,
    GetEthernetStatistic = 0x44,
};

// ECM 类特定通知码（ECM120 §6.3 表 11）
enum class EcmNotification {
    NetworkConnection = 0x00,
    ResponseAvailable = 0x01,
    ConnectionSpeedChange = 0x2A,
};

// ==================== 固定长度类特定描述符结构体 ====================
// 长度由规范硬性规定，用 packed 结构体表示，static_assert 校验 sizeof。
// append_to 按字段序列化（多字节字段自动转小端），与平台字节序无关。

#pragma pack(push, 1)
/// Ethernet Networking 功能描述符（ECM120 §5.4 表 3，固定 13 字节）
struct CdcEthernetFunctionalDesc {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint8_t bDescriptorSubtype; // 0x0F EthernetNetworking
    std::uint8_t iMACAddress; // MAC 地址字符串索引（不能为 0，cdc_ether 主机驱动读不到会 bind 失败）
    std::uint32_t bmEthernetStatistics; // 全 0 = 不支持 GetEthernetStatistic
    std::uint16_t wMaxSegmentSize; // 最大以太网帧长，典型 1514
    std::uint16_t wNumberMCFilters; // 多播过滤数，D15=0 完美过滤
    std::uint8_t bNumberPowerFilters; // 电源管理模式过滤数（0 = 不支持）

    void append_to(data_type &d) const {
        vector_append_to_le(d, bLength, bDescriptorType, bDescriptorSubtype, iMACAddress, bmEthernetStatistics,
                            wMaxSegmentSize, wNumberMCFilters, bNumberPowerFilters);
    }
};
static_assert(sizeof(CdcEthernetFunctionalDesc) == 13, "Ethernet Networking 功能描述符必须为 13 字节");

/// CDC 通知头（USB CDC 1.2 §6.3，固定 8 字节）
/// 状态型通知把状态放 wValue（如 NETWORK_CONNECTION 的 0/1），数据字段可有可无
struct CdcNotificationHeader {
    std::uint8_t bmRequestType; // 0xA1：类|接口|IN
    std::uint8_t bNotificationCode;
    std::uint16_t wValue;
    std::uint16_t wIndex;
    std::uint16_t wLength;

    void append_to(data_type &d) const {
        vector_append_to_le(d, bmRequestType, bNotificationCode, wValue, wIndex, wLength);
    }
};
static_assert(sizeof(CdcNotificationHeader) == 8, "CDC 通知头固定 8 字节");
#pragma pack(pop)

/// 组装一条中断 IN 通知消息（8 字节头 + 可选数据），返回完整消息字节
inline data_type make_cdc_notification(EcmNotification code, std::uint16_t w_value, std::uint16_t w_index,
                                       const std::uint8_t *data, std::uint16_t length) {
    data_type bytes;
    CdcNotificationHeader{0xA1, static_cast<std::uint8_t>(code), w_value, w_index, length}.append_to(bytes);
    if (length > 0 && data != nullptr) {
        bytes.insert(bytes.end(), data, data + length);
    }
    return bytes;
}
} // namespace usbipdcpp
