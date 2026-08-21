#pragma once

#include <bit>
#include <cstdint>

// 通用类请求码（SET_CUR/GET_CUR 等）和类特定描述符类型（CS_INTERFACE 等）
#include "usbipdcpp/type.h"
#include "usbipdcpp/utils/utils.h"
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

// ==================== UAC1 状态字（AC 接口中断端点）====================
// 对齐内核 include/uapi/linux/usb/audio.h 的 UAC1_STATUS_TYPE_*：状态字 2 字节
// （bStatusType + bOriginator），FU 控制变化时经 AC 中断端点推送（audio_notify）
constexpr std::uint8_t UAC1_STATUS_TYPE_ORIG_AUDIO_CONTROL_IF = 0x00;
constexpr std::uint8_t UAC1_STATUS_TYPE_IRQ_PENDING = 0x01;

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
// UAC 1.0 描述符长度由规范硬性规定，用 packed 结构体表示，
// static_assert 校验 sizeof 防止字段增删导致长度偏离规范。
// append_to 按字段序列化（多字节字段自动转小端），与平台字节序无关。

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

    void append_to(data_type &d) const {
        vector_append_to_le(d, bLength, bDescriptorType, bDescriptorSubtype, bcdADC, wTotalLength, bInCollection,
                            baInterfaceNr);
    }
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

    void append_to(data_type &d) const {
        vector_append_to_le(d, bLength, bDescriptorType, bDescriptorSubtype, bTerminalID, wTerminalType, bAssocTerminal,
                            bNrChannels, wChannelConfig, iChannelNames, iTerminal);
    }
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

    void append_to(data_type &d) const {
        vector_append_to_le(d, bLength, bDescriptorType, bDescriptorSubtype, bTerminalID, wTerminalType, bAssocTerminal,
                            bSourceID, iTerminal);
    }
};
static_assert(sizeof(AcOutputTerminalDesc) == AC_OUTPUT_TERMINAL_LEN, "Output Terminal 描述符必须为 9 字节");

/// Feature Unit 描述符固定头部（UAC 1.0 Table 4-7，bLength..bControlSize 共 6 字节）
struct AcFeatureUnitHead {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint8_t bDescriptorSubtype;
    std::uint8_t bUnitID;
    std::uint8_t bSourceID;
    std::uint8_t bControlSize;

    /// 追加完整 Feature Unit 描述符：固定头 + bmaControls[ch+1]（所有声道共享同一组控制）+ iFeature
    void append_to(data_type &d, std::uint8_t bma_controls, std::uint8_t channels) const {
        vector_append_to_le(d, bLength, bDescriptorType, bDescriptorSubtype, bUnitID, bSourceID, bControlSize);
        for (int i = 0; i < channels + 1; ++i)
            d.push_back(bma_controls);
        d.push_back(0x00); // iFeature
    }
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

    void append_to(data_type &d) const {
        vector_append_to_le(d, bLength, bDescriptorType, bDescriptorSubtype, bTerminalLink, bDelay, wFormatTag);
    }
};
static_assert(sizeof(AsGeneralDesc) == AS_GENERAL_LEN, "AS General 描述符必须为 7 字节");

/// Format Type I 描述符固定头部（bLength..bSamFreqType 共 8 字节）
struct AsFormatTypeIHead {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint8_t bDescriptorSubtype;
    std::uint8_t bFormatType;
    std::uint8_t bNrChannels;
    std::uint8_t bSubframeSize;
    std::uint8_t bBitResolution;
    std::uint8_t bSamFreqType;

    /// 追加完整 Format Type I 描述符：固定头 + tSamFreq[3×n]（每项为 24 位小端）
    void append_to(data_type &d, const std::vector<std::uint32_t> &rates) const {
        // 一次 reserve 全部可变部分，避免循环内反复扩容
        d.reserve(d.size() + sizeof(AsFormatTypeIHead) + rates.size() * AS_SAMFREQ_ENTRY_LEN);
        vector_append_to_le(d, bLength, bDescriptorType, bDescriptorSubtype, bFormatType, bNrChannels, bSubframeSize,
                            bBitResolution, bSamFreqType);
        for (auto rate: rates) {
            vector_append_to_le(d, static_cast<std::uint8_t>(rate & 0xFF), static_cast<std::uint8_t>((rate >> 8) & 0xFF),
                                static_cast<std::uint8_t>((rate >> 16) & 0xFF));
        }
    }
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

    void append_to(data_type &d) const {
        vector_append_to_le(d, bLength, bDescriptorType, bDescriptorSubtype, bmAttributes, bLockDelayUnits, wLockDelay);
    }
};
static_assert(sizeof(AsEpGeneralDesc) == AS_EP_DESC_GENERAL_LEN, "EP_GENERAL 端点描述符必须为 7 字节");
#pragma pack(pop)

// ==================== UAC 版本 ====================
constexpr std::uint16_t UAC_BCD_1_00 = 0x0100;

} // namespace usbipdcpp
