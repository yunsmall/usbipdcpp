#pragma once

#include <bit>
#include <cstdint>

// 通用类请求码（SET_CUR/GET_CUR 等）和类特定描述符类型（CS_INTERFACE 等）
#include "usbipdcpp/type.h"
#include "usbipdcpp/virtual_device/UsbClassConstants.h"

namespace usbipdcpp {

// ==================== USB Audio Class codes ====================
constexpr std::uint8_t CC_AUDIO = 0x01;
constexpr std::uint8_t SC_AUDIOCONTROL = 0x01;
constexpr std::uint8_t SC_AUDIOSTREAMING = 0x02;

// ==================== AC Interface Descriptor Subtypes ====================
constexpr std::uint8_t AC_DESC_UNDEFINED = 0x00;
constexpr std::uint8_t AC_DESC_HEADER = 0x01;
constexpr std::uint8_t AC_DESC_INPUT_TERMINAL = 0x02;
constexpr std::uint8_t AC_DESC_OUTPUT_TERMINAL = 0x03;
constexpr std::uint8_t AC_DESC_MIXER_UNIT = 0x04;
constexpr std::uint8_t AC_DESC_SELECTOR_UNIT = 0x05;
constexpr std::uint8_t AC_DESC_FEATURE_UNIT = 0x06;

// ==================== AS Interface Descriptor Subtypes ====================
constexpr std::uint8_t AS_DESC_UNDEFINED = 0x00;
constexpr std::uint8_t AS_DESC_GENERAL = 0x01;
constexpr std::uint8_t AS_DESC_FORMAT_TYPE = 0x02;

// ==================== AS 类特定端点描述符 ====================
// Linux snd-usb-audio 依赖 bmAttributes 的 SamplingFreqControl 位判断是否
// 支持采样率协商；缺失时驱动跳过 SET_CUR 直接按请求速率开流
constexpr std::uint8_t AS_EP_DESC_GENERAL = 0x01; // bDescriptorSubtype: EP_GENERAL
constexpr std::uint8_t AS_EP_ATTR_SAMPLING_FREQ = 0x01; // bmAttributes: SamplingFreqControl
constexpr std::uint8_t AS_EP_DESC_GENERAL_LEN = 7; // 类特定 ISO 音频数据端点描述符

// ==================== Terminal Types ====================
constexpr std::uint16_t TT_USB_STREAMING = 0x0101;
constexpr std::uint16_t ITT_MICROPHONE = 0x0201;
constexpr std::uint16_t ITT_SPEAKER = 0x0301;

// ==================== Channel Config ====================
constexpr std::uint16_t CHANNEL_CONFIG_MONO = 0x0004; // LEFT_FRONT
constexpr std::uint16_t CHANNEL_CONFIG_STEREO = 0x0003; // LEFT_FRONT | RIGHT_FRONT

// ==================== Entity IDs ====================
// 前缀 UAC_ 避免与 UvcConstants.h 中的 ENTITY_* 常量重名
constexpr std::uint8_t UAC_ENTITY_INPUT_TERMINAL = 0x01;
constexpr std::uint8_t UAC_ENTITY_FEATURE_UNIT = 0x02;
constexpr std::uint8_t UAC_ENTITY_OUTPUT_TERMINAL = 0x03;

// ==================== Feature Unit Control Selectors ====================
constexpr std::uint8_t FU_CONTROL_UNDEFINED = 0x00;
constexpr std::uint8_t FU_MUTE_CONTROL = 0x01;
constexpr std::uint8_t FU_VOLUME_CONTROL = 0x02;

// ==================== AS Interface Control Selectors ====================
constexpr std::uint8_t AS_CONTROL_UNDEFINED = 0x00;
constexpr std::uint8_t AS_SAMPLING_FREQ_CONTROL = 0x01;

// ==================== 音频格式常量 ====================
constexpr std::uint16_t AUDIO_FORMAT_PCM = 0x0001; // wFormatTag: PCM

// ==================== 描述符尺寸常量 ====================
constexpr std::uint8_t AC_HEADER_LEN = 9;
constexpr std::uint8_t AC_INPUT_TERMINAL_LEN = 12;
constexpr std::uint8_t AC_OUTPUT_TERMINAL_LEN = 9;
// Feature Unit 固定字节数：bLength..bControlSize 共 6 字节 + 末尾 iFeature 1 字节。
// 总长 = 固定 7 + bmaControls 字节数，其中 bmaControls 字节数 = 声道数 + 1（master + 每个逻辑通道）
constexpr std::uint8_t AC_FEATURE_UNIT_FIXED_LEN = 7;
constexpr std::uint8_t AS_GENERAL_LEN = 7;
constexpr std::uint8_t AS_FORMAT_TYPE_I_BASE_LEN = 8; // bLength..bSamFreqType 共 8 字节
constexpr std::uint8_t AS_SAMFREQ_ENTRY_LEN = 3; // 每个采样率占 3 字节（tSamFreq）

// ==================== 固定长度描述符结构体 ====================
// UAC 1.0 描述符长度由规范硬性规定，用 packed 结构体直接映射线格式，
// static_assert 校验 sizeof 防止字段增删导致长度偏离规范。
// 仅小端平台可用（USB 线格式为小端，结构体直接按字节拷贝）。

static_assert(std::endian::native == std::endian::little, "描述符结构体按小端字节序直接拷贝，仅支持小端平台");

#pragma pack(push, 1)
/// AC Header 描述符（UAC 1.0 Table 4-2，单 AS 接口固定 9 字节）
struct AcHeaderDesc {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint8_t bDescriptorSubtype;
    std::uint16_t bcdADC;
    std::uint16_t wTotalLength;
    std::uint8_t bInCollection;
    std::uint8_t baInterfaceNr;
};
static_assert(sizeof(AcHeaderDesc) == AC_HEADER_LEN, "AC Header 描述符必须为 9 字节");

/// Input Terminal 描述符（UAC 1.0 Table 4-3，固定 12 字节）
struct AcInputTerminalDesc {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint8_t bDescriptorSubtype;
    std::uint8_t bTerminalID;
    std::uint16_t wTerminalType;
    std::uint8_t bAssocTerminal;
    std::uint8_t bNrChannels;
    std::uint16_t wChannelConfig;
    std::uint8_t iChannelNames;
    std::uint8_t iTerminal;
};
static_assert(sizeof(AcInputTerminalDesc) == AC_INPUT_TERMINAL_LEN, "Input Terminal 描述符必须为 12 字节");

/// Output Terminal 描述符（UAC 1.0 Table 4-4，固定 9 字节）
struct AcOutputTerminalDesc {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint8_t bDescriptorSubtype;
    std::uint8_t bTerminalID;
    std::uint16_t wTerminalType;
    std::uint8_t bAssocTerminal;
    std::uint8_t bSourceID;
    std::uint8_t iTerminal;
};
static_assert(sizeof(AcOutputTerminalDesc) == AC_OUTPUT_TERMINAL_LEN, "Output Terminal 描述符必须为 9 字节");

/// Feature Unit 描述符固定头部（UAC 1.0 Table 4-7，bLength..bControlSize 共 6 字节）
/// 可变部分 bmaControls[ch+1] + iFeature 由构建代码拼接
struct AcFeatureUnitHead {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint8_t bDescriptorSubtype;
    std::uint8_t bUnitID;
    std::uint8_t bSourceID;
    std::uint8_t bControlSize;
};
static_assert(sizeof(AcFeatureUnitHead) == 6, "Feature Unit 固定头部必须为 6 字节");

/// AS General 描述符（UAC 1.0 Table 4-19，固定 7 字节）
struct AsGeneralDesc {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint8_t bDescriptorSubtype;
    std::uint8_t bTerminalLink;
    std::uint8_t bDelay;
    std::uint16_t wFormatTag;
};
static_assert(sizeof(AsGeneralDesc) == AS_GENERAL_LEN, "AS General 描述符必须为 7 字节");

/// Format Type I 描述符固定头部（bLength..bSamFreqType 共 8 字节）
/// 可变部分 tSamFreq[3×n] 由构建代码拼接
struct AsFormatTypeIHead {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint8_t bDescriptorSubtype;
    std::uint8_t bFormatType;
    std::uint8_t bNrChannels;
    std::uint8_t bSubframeSize;
    std::uint8_t bBitResolution;
    std::uint8_t bSamFreqType;
};
static_assert(sizeof(AsFormatTypeIHead) == AS_FORMAT_TYPE_I_BASE_LEN, "Format Type I 固定头部必须为 8 字节");

/// EP_GENERAL 类特定端点描述符（UAC 1.0 Table 4-21，固定 7 字节）
struct AsEpGeneralDesc {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint8_t bDescriptorSubtype;
    std::uint8_t bmAttributes;
    std::uint8_t bLockDelayUnits;
    std::uint16_t wLockDelay;
};
static_assert(sizeof(AsEpGeneralDesc) == AS_EP_DESC_GENERAL_LEN, "EP_GENERAL 端点描述符必须为 7 字节");
#pragma pack(pop)

/// 将 packed 描述符结构体按字节追加到 data_type（pack(1) 无填充，小端序与 USB 线格式一致）
template <typename T>
inline void append_descriptor(data_type &d, const T &desc) {
    const auto *p = reinterpret_cast<const std::uint8_t *>(&desc);
    d.insert(d.end(), p, p + sizeof(T));
}

// ==================== UAC 版本 ====================
constexpr std::uint16_t UAC_BCD_1_00 = 0x0100;

} // namespace usbipdcpp
