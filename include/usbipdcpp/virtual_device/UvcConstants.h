#pragma once

#include <cstdint>

#include "usbipdcpp/type.h"
#include "usbipdcpp/utils/utils.h"
// 通用类请求码（SET_CUR/GET_CUR 等）和类特定描述符类型（CS_INTERFACE 等）
#include "usbipdcpp/virtual_device/UsbClassConstants.h"

namespace usbipdcpp {

// USB Video Class codes
constexpr std::uint8_t CC_VIDEO = 0x0E;
constexpr std::uint8_t SC_VIDEOCONTROL = 0x01;
constexpr std::uint8_t SC_VIDEOSTREAMING = 0x02;
constexpr std::uint8_t SC_VIDEO_INTERFACE_COLLECTION = 0x03;
constexpr std::uint8_t PC_PROTOCOL_UNDEFINED = 0x00;
constexpr std::uint8_t PC_PROTOCOL_15 = 0x01;

// VC Interface Descriptor Subtypes
constexpr std::uint8_t VC_DESC_UNDEFINED = 0x00;
constexpr std::uint8_t VC_DESC_HEADER = 0x01;
constexpr std::uint8_t VC_DESC_INPUT_TERMINAL = 0x02;
constexpr std::uint8_t VC_DESC_OUTPUT_TERMINAL = 0x03;
constexpr std::uint8_t VC_DESC_SELECTOR_UNIT = 0x04;
constexpr std::uint8_t VC_DESC_PROCESSING_UNIT = 0x05;
constexpr std::uint8_t VC_DESC_EXTENSION_UNIT = 0x06;

// VS Interface Descriptor Subtypes
constexpr std::uint8_t VS_DESC_UNDEFINED = 0x00;
constexpr std::uint8_t VS_DESC_INPUT_HEADER = 0x01;
constexpr std::uint8_t VS_DESC_OUTPUT_HEADER = 0x02;
constexpr std::uint8_t VS_DESC_FORMAT_MJPEG = 0x06;
constexpr std::uint8_t VS_DESC_FRAME_MJPEG = 0x07;
constexpr std::uint8_t VS_DESC_FORMAT_UNCOMPRESSED = 0x04;
constexpr std::uint8_t VS_DESC_FRAME_UNCOMPRESSED = 0x05;
constexpr std::uint8_t VS_DESC_FORMAT_FRAME_BASED = 0x10;
constexpr std::uint8_t VS_DESC_FRAME_FRAME_BASED = 0x11;
constexpr std::uint8_t VS_DESC_FORMAT_H264 = 0x13; // UVC 1.5 H.264 Payload spec Table 3-1
constexpr std::uint8_t VS_DESC_FRAME_H264 = 0x14;  // UVC 1.5 H.264 Payload spec Table 3-2
constexpr std::uint8_t VS_DESC_COLORFORMAT = 0x0D;

// Terminal Types
constexpr std::uint16_t TT_VENDOR_SPECIFIC = 0x0100;
constexpr std::uint16_t TT_STREAMING = 0x0101;
constexpr std::uint16_t ITT_VENDOR_SPECIFIC = 0x0200;
constexpr std::uint16_t ITT_CAMERA = 0x0201;
constexpr std::uint16_t ITT_MEDIA_TRANSPORT_INPUT = 0x0202;

// VC Control Selectors
constexpr std::uint8_t VC_CONTROL_UNDEFINED = 0x00;
constexpr std::uint8_t VC_VIDEO_POWER_MODE_CONTROL = 0x01;
constexpr std::uint8_t VC_REQUEST_ERROR_CODE_CONTROL = 0x02;

// Entity IDs
constexpr std::uint8_t ENTITY_VC_INTERFACE = 0x00;
constexpr std::uint8_t ENTITY_INPUT_TERMINAL = 0x01;
constexpr std::uint8_t ENTITY_PROCESSING_UNIT = 0x02;
constexpr std::uint8_t ENTITY_OUTPUT_TERMINAL = 0x03;

// Terminal Control Selectors
constexpr std::uint8_t CT_CONTROL_UNDEFINED = 0x00;
constexpr std::uint8_t CT_SCANNING_MODE = 0x01;
constexpr std::uint8_t CT_AE_MODE = 0x02;
constexpr std::uint8_t CT_AE_PRIORITY = 0x03;
constexpr std::uint8_t CT_EXPOSURE_TIME_ABSOLUTE = 0x04;
constexpr std::uint8_t CT_EXPOSURE_TIME_RELATIVE = 0x05;
constexpr std::uint8_t CT_FOCUS_ABSOLUTE = 0x06;
constexpr std::uint8_t CT_FOCUS_RELATIVE = 0x07;
constexpr std::uint8_t CT_FOCUS_AUTO = 0x08;
constexpr std::uint8_t CT_ZOOM_ABSOLUTE = 0x0B;
constexpr std::uint8_t CT_ZOOM_RELATIVE = 0x0C;
constexpr std::uint8_t CT_PANTILT_ABSOLUTE = 0x0D;
constexpr std::uint8_t CT_PANTILT_RELATIVE = 0x0E;
constexpr std::uint8_t CT_ROLL_ABSOLUTE = 0x0F;
constexpr std::uint8_t CT_ROLL_RELATIVE = 0x10;
constexpr std::uint8_t CT_PRIVACY = 0x11;

// Processing Unit Control Selectors
constexpr std::uint8_t PU_CONTROL_UNDEFINED = 0x00;
constexpr std::uint8_t PU_BACKLIGHT_COMPENSATION = 0x01;
constexpr std::uint8_t PU_BRIGHTNESS = 0x02;
constexpr std::uint8_t PU_CONTRAST = 0x03;
constexpr std::uint8_t PU_GAIN = 0x04;
constexpr std::uint8_t PU_POWER_LINE_FREQUENCY = 0x05;
constexpr std::uint8_t PU_HUE = 0x06;
constexpr std::uint8_t PU_SATURATION = 0x07;
constexpr std::uint8_t PU_SHARPNESS = 0x08;
constexpr std::uint8_t PU_GAMMA = 0x09;
constexpr std::uint8_t PU_WHITE_BALANCE_TEMPERATURE = 0x0A;
constexpr std::uint8_t PU_WHITE_BALANCE_TEMPERATURE_AUTO = 0x0B;
constexpr std::uint8_t PU_WHITE_BALANCE_COMPONENT = 0x0C;
constexpr std::uint8_t PU_WHITE_BALANCE_COMPONENT_AUTO = 0x0D;
constexpr std::uint8_t PU_DIGITAL_MULTIPLIER = 0x0E;
constexpr std::uint8_t PU_DIGITAL_MULTIPLIER_LIMIT = 0x0F;
constexpr std::uint8_t PU_HUE_AUTO = 0x10;
constexpr std::uint8_t PU_ANALOG_VIDEO_STANDARD = 0x11;
constexpr std::uint8_t PU_ANALOG_LOCK_STATUS = 0x12;

// VS Probe/Commit Control
constexpr std::uint8_t VS_CONTROL_UNDEFINED = 0x00;
constexpr std::uint8_t VS_PROBE_CONTROL = 0x01;
constexpr std::uint8_t VS_COMMIT_CONTROL = 0x02;
constexpr std::uint8_t VS_STILL_PROBE_CONTROL = 0x03;
constexpr std::uint8_t VS_STILL_COMMIT_CONTROL = 0x04;
constexpr std::uint8_t VS_STREAM_ERROR_CODE_CONTROL = 0x06;

// Video Class-Specific Endpoint Descriptor Subtypes (Table A-7)
constexpr std::uint8_t EP_UNDEFINED = 0x00;
constexpr std::uint8_t EP_GENERAL = 0x01;
constexpr std::uint8_t EP_ENDPOINT = 0x02;
constexpr std::uint8_t EP_INTERRUPT = 0x03;

// UVC version
constexpr std::uint16_t UVC_BCD_1_00 = 0x0100;
constexpr std::uint16_t UVC_BCD_1_10 = 0x0110;
constexpr std::uint16_t UVC_BCD_1_50 = 0x0150;

// Color Matching Descriptor
constexpr std::uint8_t VIDEO_COLOR_PRIMARIES_BT709 = 0x01;
constexpr std::uint8_t VIDEO_COLOR_XFER_CH_BT709 = 0x01;
constexpr std::uint8_t VIDEO_COLOR_COEF_SMPTE170M = 0x04;

// Descriptor size constants
constexpr std::uint8_t VC_HEADER_1ITF_LEN = 13; // 12 + bInCollection(1)
constexpr std::uint8_t CAMERA_TERM_LEN = 18; // 8 + focal(2+2+2) + bControlSize(1) + bmControls(3)
constexpr std::uint8_t OUTPUT_TERM_LEN = 9;
constexpr std::uint8_t PU_LEN = 13;
constexpr std::uint8_t VS_INPUT_HEADER_LEN = 14; // 13 + bControlSize(1)
constexpr std::uint8_t VS_FMT_UNCOMPR_LEN = 27;
constexpr std::uint8_t VS_FRM_UNCOMPR_CONT_LEN = 38; // 26 + 3*4 (continuous: min/max/step)
constexpr std::uint8_t VS_FMT_MJPEG_LEN = 11;
constexpr std::uint8_t VS_FMT_FRAME_BASED_LEN = 28;    // generic frame-based (VP8 etc.)
constexpr std::uint8_t VS_FMT_H264_LEN = 52;           // H.264 Payload spec Table 3-1
constexpr std::uint8_t VS_FRM_H264_BASE_LEN = 44;      // H.264 frame before intervals, Table 3-2
constexpr std::uint8_t VS_FRM_H264_CONT_LEN = 56;      // 44 + 3*4
constexpr std::uint8_t VS_COLOR_MATCHING_LEN = 6;

// UVC Payload Header（UVC 1.5 Table 2-5 bmHeaderInfo：D0 FID、D1 EOH、D7 EOF）。
// Windows usbvideo.sys 按规范位解释；Linux uvcvideo 驱动用自定义位
// （UVC_STREAM_FID=0x80/UVC_STREAM_EOF=0x40，与规范相反），设备侧必须按规范发
constexpr std::uint8_t UVC_PAYLOAD_HEADER_SIZE = 2;
constexpr std::uint8_t UVC_PAYLOAD_HEADER_FID = 0x01; // D0: Frame ID bit
constexpr std::uint8_t UVC_PAYLOAD_HEADER_EOF = 0x80; // D7: End of Frame

// ==================== 固定长度描述符结构体 ====================
// UVC 1.5 描述符长度由规范硬性规定，用 packed 结构体表示，
// static_assert 校验 sizeof 防止字段增删导致长度偏离规范。
// append_to 按字段序列化（多字节字段自动转小端），与平台字节序无关。

#pragma pack(push, 1)
/// VC Header 描述符（UVC 1.5 Table 3-3，单 VS 接口固定 13 字节）
struct VcHeaderDesc {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint8_t bDescriptorSubtype;
    std::uint16_t bcdUVC;
    std::uint16_t wTotalLength;
    std::uint32_t dwClockFrequency;
    std::uint8_t bInCollection;
    std::uint8_t baInterfaceNr;

    void append_to(data_type &d) const {
        vector_append_to_le(d, bLength, bDescriptorType, bDescriptorSubtype, bcdUVC, wTotalLength, dwClockFrequency,
                            bInCollection, baInterfaceNr);
    }
};
static_assert(sizeof(VcHeaderDesc) == VC_HEADER_1ITF_LEN, "VC Header 描述符必须为 13 字节");

/// Camera Terminal 描述符（UVC 1.5 Table 3-6，固定 18 字节，bControlSize=3）
struct VcCameraTerminalDesc {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint8_t bDescriptorSubtype;
    std::uint8_t bTerminalID;
    std::uint16_t wTerminalType;
    std::uint8_t bAssocTerminal;
    std::uint8_t iTerminal;
    std::uint16_t wObjectiveFocalLengthMin;
    std::uint16_t wObjectiveFocalLengthMax;
    std::uint16_t wOcularFocalLength;
    std::uint8_t bControlSize;
    array_data_type<3> bmControls;

    void append_to(data_type &d) const {
        vector_append_to_le(d, bLength, bDescriptorType, bDescriptorSubtype, bTerminalID, wTerminalType, bAssocTerminal,
                            iTerminal, wObjectiveFocalLengthMin, wObjectiveFocalLengthMax, wOcularFocalLength,
                            bControlSize, bmControls);
    }
};
static_assert(sizeof(VcCameraTerminalDesc) == CAMERA_TERM_LEN, "Camera Terminal 描述符必须为 18 字节");

/// Processing Unit 描述符（UVC 1.5 Table 3-8，固定 13 字节，bControlSize=3，含 bmVideoStandards）
struct VcProcessingUnitDesc {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint8_t bDescriptorSubtype;
    std::uint8_t bUnitID;
    std::uint8_t bSourceID;
    std::uint16_t wMaxMultiplier;
    std::uint8_t bControlSize;
    array_data_type<3> bmControls;
    std::uint8_t iProcessing;
    std::uint8_t bmVideoStandards;

    void append_to(data_type &d) const {
        vector_append_to_le(d, bLength, bDescriptorType, bDescriptorSubtype, bUnitID, bSourceID, wMaxMultiplier,
                            bControlSize, bmControls, iProcessing, bmVideoStandards);
    }
};
static_assert(sizeof(VcProcessingUnitDesc) == PU_LEN, "Processing Unit 描述符必须为 13 字节");

/// VC Output Terminal 描述符（UVC 1.5 Table 3-5，固定 9 字节）
struct VcOutputTerminalDesc {
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
static_assert(sizeof(VcOutputTerminalDesc) == OUTPUT_TERM_LEN, "VC Output Terminal 描述符必须为 9 字节");

/// VS Input Header 描述符（UVC 1.5 Table 3-14，固定 13+(p×n)，p=n=1 时 14 字节）
struct VsInputHeaderDesc {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint8_t bDescriptorSubtype;
    std::uint8_t bNumFormats;
    std::uint16_t wTotalLength;
    std::uint8_t bEndpointAddress;
    std::uint8_t bmInfo;
    std::uint8_t bTerminalLink;
    std::uint8_t bStillCaptureMethod;
    std::uint8_t bTriggerSupport;
    std::uint8_t bTriggerUsage;
    std::uint8_t bControlSize;
    std::uint8_t bmaControls; // bControlSize=1

    void append_to(data_type &d) const {
        vector_append_to_le(d, bLength, bDescriptorType, bDescriptorSubtype, bNumFormats, wTotalLength, bEndpointAddress,
                            bmInfo, bTerminalLink, bStillCaptureMethod, bTriggerSupport, bTriggerUsage, bControlSize,
                            bmaControls);
    }
};
static_assert(sizeof(VsInputHeaderDesc) == VS_INPUT_HEADER_LEN, "VS Input Header 描述符必须为 14 字节");

/// Color Matching 描述符（UVC 1.5 各 payload spec，固定 6 字节）
struct VsColorMatchingDesc {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint8_t bDescriptorSubtype;
    std::uint8_t bColorPrimaries;
    std::uint8_t bTransferCharacteristics;
    std::uint8_t bMatrixCoefficients;

    void append_to(data_type &d) const {
        vector_append_to_le(d, bLength, bDescriptorType, bDescriptorSubtype, bColorPrimaries, bTransferCharacteristics,
                            bMatrixCoefficients);
    }
};
static_assert(sizeof(VsColorMatchingDesc) == VS_COLOR_MATCHING_LEN, "Color Matching 描述符必须为 6 字节");

/// VC 中断端点类特定描述符（UVC 1.5 Table 3-12，固定 5 字节）
struct VcInterruptEndpointDesc {
    std::uint8_t bLength;
    std::uint8_t bDescriptorType;
    std::uint8_t bDescriptorSubtype;
    std::uint16_t wMaxTransferSize;

    void append_to(data_type &d) const {
        vector_append_to_le(d, bLength, bDescriptorType, bDescriptorSubtype, wMaxTransferSize);
    }
};
static_assert(sizeof(VcInterruptEndpointDesc) == 5, "VC 中断端点类描述符必须为 5 字节");
#pragma pack(pop)

// FOURCC pixel format codes
namespace UvcFourCC {
    constexpr std::uint32_t MJPEG = 0x47504A4D; // 'MJPG' LE
    constexpr std::uint32_t YUY2 = 0x32595559; // 'YUY2' LE
    constexpr std::uint32_t NV12 = 0x3231564E; // 'NV12' LE
    constexpr std::uint32_t H264 = 0x34363248; // 'H264' LE
    constexpr std::uint32_t I420 = 0x30323449; // 'I420' LE
} // namespace UvcFourCC

/// UVC 格式类别 — 决定 VS 描述符子类型和 PROBE/COMMIT 字段行为
enum class UvcFormatCategory : std::uint8_t {
    Uncompressed, // YUY2 / NV12 / I420 — VS_DESC_FORMAT_UNCOMPRESSED
    Mjpeg,       // MJPEG — VS_DESC_FORMAT_MJPEG
    FrameBased,  // 通用 Frame-Based (VP8 等) — VS_DESC_FORMAT_FRAME_BASED
    H264,        // H.264 — VS_DESC_FORMAT_H264（UVC 1.5 H.264 Payload spec）
};

inline constexpr UvcFormatCategory uvc_format_category(std::uint32_t fourcc) {
    switch (fourcc) {
    case UvcFourCC::MJPEG: return UvcFormatCategory::Mjpeg;
    case UvcFourCC::H264:  return UvcFormatCategory::H264;
    // YUY2 / NV12 / I420 及其他未知均视为不压缩
    default:               return UvcFormatCategory::Uncompressed;
    }
}

/// UVC 使用的 16 字节 GUID（USB 描述符中的小端序）
struct UvcGuid {
    std::uint8_t data[16];

    static constexpr UvcGuid from_fourcc(std::uint32_t fourcc) {
        // FOURCC-based GUID template: {XXXXXXXX-0000-0010-8000-00AA00389B71}
        UvcGuid g{};
        g.data[0] = static_cast<std::uint8_t>(fourcc & 0xFF);
        g.data[1] = static_cast<std::uint8_t>((fourcc >> 8) & 0xFF);
        g.data[2] = static_cast<std::uint8_t>((fourcc >> 16) & 0xFF);
        g.data[3] = static_cast<std::uint8_t>((fourcc >> 24) & 0xFF);
        g.data[4] = 0x00;
        g.data[5] = 0x00;
        g.data[6] = 0x10;
        g.data[7] = 0x00;
        g.data[8] = 0x80;
        g.data[9] = 0x00;
        g.data[10] = 0x00;
        g.data[11] = 0xAA;
        g.data[12] = 0x00;
        g.data[13] = 0x38;
        g.data[14] = 0x9B;
        g.data[15] = 0x71;
        return g;
    }

    bool operator==(const UvcGuid &other) const {
        for (int i = 0; i < 16; ++i)
            if (data[i] != other.data[i])
                return false;
        return true;
    }
};

} // namespace usbipdcpp
