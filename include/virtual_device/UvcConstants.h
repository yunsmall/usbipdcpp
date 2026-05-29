#pragma once

#include <cstdint>

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
constexpr std::uint8_t VS_DESC_COLORFORMAT = 0x0D;

// Terminal Types
constexpr std::uint16_t TT_VENDOR_SPECIFIC = 0x0100;
constexpr std::uint16_t TT_STREAMING = 0x0101;
constexpr std::uint16_t ITT_VENDOR_SPECIFIC = 0x0200;
constexpr std::uint16_t ITT_CAMERA = 0x0201;
constexpr std::uint16_t ITT_MEDIA_TRANSPORT_INPUT = 0x0202;

// VideoControl Requests
constexpr std::uint8_t RC_UNDEFINED = 0x00;
constexpr std::uint8_t SET_CUR = 0x01;
constexpr std::uint8_t SET_CUR_ALL = 0x11;
constexpr std::uint8_t GET_CUR = 0x81;
constexpr std::uint8_t GET_MIN = 0x82;
constexpr std::uint8_t GET_MAX = 0x83;
constexpr std::uint8_t GET_RES = 0x84;
constexpr std::uint8_t GET_LEN = 0x85;
constexpr std::uint8_t GET_INFO = 0x86;
constexpr std::uint8_t GET_DEF = 0x87;

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

// USB class-specific descriptor types
constexpr std::uint8_t CS_INTERFACE = 0x24;
constexpr std::uint8_t CS_ENDPOINT = 0x25;

// UVC version
constexpr std::uint16_t UVC_BCD_1_50 = 0x0150;

// Color Matching Descriptor
constexpr std::uint8_t VIDEO_COLOR_PRIMARIES_BT709 = 0x01;
constexpr std::uint8_t VIDEO_COLOR_XFER_CH_BT709 = 0x01;
constexpr std::uint8_t VIDEO_COLOR_COEF_SMPTE170M = 0x04;

// Descriptor size constants
constexpr std::uint8_t VC_HEADER_1ITF_LEN = 13; // 12 + bInCollection(1)
constexpr std::uint8_t CAMERA_TERM_LEN = 18; // 8 + focal(2+2+2) + bControlSize(1) + bmControls(3)
constexpr std::uint8_t OUTPUT_TERM_LEN = 9;
constexpr std::uint8_t PU_LEN = 11;
constexpr std::uint8_t VS_INPUT_HEADER_LEN = 14; // 13 + bControlSize(1)
constexpr std::uint8_t VS_FMT_UNCOMPR_LEN = 27;
constexpr std::uint8_t VS_FRM_UNCOMPR_CONT_LEN = 38; // 26 + 3*4 (continuous: min/max/step)
constexpr std::uint8_t VS_COLOR_MATCHING_LEN = 6;

// UVC Payload Header
constexpr std::uint8_t UVC_PAYLOAD_HEADER_SIZE = 2;
constexpr std::uint8_t UVC_PAYLOAD_HEADER_FID = 0x01; // Frame ID bit

// FOURCC pixel format codes
namespace UvcFourCC {
    constexpr std::uint32_t MJPEG = 0x47504A4D; // 'MJPG' LE
    constexpr std::uint32_t YUY2 = 0x32595559; // 'YUY2' LE
    constexpr std::uint32_t NV12 = 0x3231564E; // 'NV12' LE
    constexpr std::uint32_t H264 = 0x34363248; // 'H264' LE
    constexpr std::uint32_t I420 = 0x30323449; // 'I420' LE
} // namespace UvcFourCC

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
