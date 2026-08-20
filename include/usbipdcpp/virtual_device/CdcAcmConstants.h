#pragma once

#include <cstdint>

#include "usbipdcpp/type.h"
#include "usbipdcpp/utils/utils.h"
#include "usbipdcpp/virtual_device/UsbClassConstants.h"

namespace usbipdcpp {
// CDC ACM 类特定描述符子类型
enum class CdcAcmDescriptorSubtype {
    Header = 0x00,
    CallManagement = 0x01,
    ACM = 0x02,
    DirectLineManagement = 0x03,
    TelephoneRinger = 0x04,
    TelephoneCallStateReporting = 0x05,
    Union = 0x06,
    CountrySelection = 0x07,
    TelephoneOperatingModes = 0x08,
    USBTerminal = 0x09,
    NetworkChannelTerminal = 0x0A,
    ProtocolUnit = 0x0B,
    ExtensionUnit = 0x0C,
    MultiChannelManagement = 0x0D,
    CAPIControlManagement = 0x0E,
    EthernetNetworking = 0x0F,
    ATMNetworking = 0x10,
};

// CDC ACM 控制请求码
enum class CdcAcmRequest {
    SendEncapsulatedCommand = 0x00,
    GetEncapsulatedResponse = 0x01,
    SetCommFeature = 0x02,
    GetCommFeature = 0x03,
    ClearCommFeature = 0x04,
    SetLineCoding = 0x20,
    GetLineCoding = 0x21,
    SetControlLineState = 0x22,
    SendBreak = 0x23,
};

// CDC ACM 控制信号位
enum class CdcAcmControlSignal : std::uint16_t {
    DTR = 0x01, // Data Terminal Ready
    RTS = 0x02, // Request To Send
};

// CDC ACM 串口状态位
enum class CdcAcmSerialState : std::uint16_t {
    DCD = 0x01, // Data Carrier Detect
    DSR = 0x02, // Data Set Ready
    Break = 0x04, // Break signal
    Ring = 0x08, // Ring signal
    FramingError = 0x10, // Framing error
    ParityError = 0x20, // Parity error
    OverrunError = 0x40, // Overrun error
    CTS = 0x80, // Clear To Send
};

// ==================== 固定长度类特定描述符结构体 ====================
// CDC 1.1 功能描述符长度由规范硬性规定，用 packed 结构体表示，
// static_assert 校验 sizeof 防止字段增删导致长度偏离规范。
// append_to 按字段序列化（多字节字段自动转小端），与平台字节序无关。

#pragma pack(push, 1)
/// CDC Header 功能描述符（CDC 1.1，固定 5 字节）
struct CdcHeaderFunctionalDesc {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint8_t bDescriptorSubtype;
    std::uint16_t bcdCDC;

    void append_to(data_type &d) const {
        vector_append_to_le(d, bLength, bDescriptorType, bDescriptorSubtype, bcdCDC);
    }
};
static_assert(sizeof(CdcHeaderFunctionalDesc) == 5, "CDC Header 功能描述符必须为 5 字节");

/// Call Management 功能描述符（CDC 1.1，固定 5 字节）
struct CdcCallManagementDesc {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint8_t bDescriptorSubtype;
    std::uint8_t bmCapabilities;
    std::uint8_t bDataInterface;

    void append_to(data_type &d) const {
        vector_append_to_le(d, bLength, bDescriptorType, bDescriptorSubtype, bmCapabilities, bDataInterface);
    }
};
static_assert(sizeof(CdcCallManagementDesc) == 5, "Call Management 功能描述符必须为 5 字节");

/// ACM 功能描述符（CDC 1.1，固定 4 字节）
struct CdcAcmFunctionalDesc {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint8_t bDescriptorSubtype;
    std::uint8_t bmCapabilities;

    void append_to(data_type &d) const {
        vector_append_to_le(d, bLength, bDescriptorType, bDescriptorSubtype, bmCapabilities);
    }
};
static_assert(sizeof(CdcAcmFunctionalDesc) == 4, "ACM 功能描述符必须为 4 字节");

/// Union 功能描述符（CDC 1.1，固定 5 字节，单从属接口）
struct CdcUnionFunctionalDesc {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint8_t bDescriptorSubtype;
    std::uint8_t bMasterInterface;
    std::uint8_t bSlaveInterface0;

    void append_to(data_type &d) const {
        vector_append_to_le(d, bLength, bDescriptorType, bDescriptorSubtype, bMasterInterface, bSlaveInterface0);
    }
};
static_assert(sizeof(CdcUnionFunctionalDesc) == 5, "Union 功能描述符必须为 5 字节");
#pragma pack(pop)
} // namespace usbipdcpp
