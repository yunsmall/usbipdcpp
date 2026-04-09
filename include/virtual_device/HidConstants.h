#pragma once

#include <cstdint>

namespace usbipdcpp {

enum class HIDRequest {
    GetReport = 0x01,
    GetIdle = 0x02,
    GetProtocol = 0x03,
    SetReport = 0x09,
    SetIdle = 0x0A,
    SetProtocol = 0x0B,
};

enum class HIDReportType {
    Output = 0x01,
    Input = 0x02,
    Feature = 0x03,
};

enum class HIDProtocolType {
    Boot = 0x00,
    Report = 0x01,
};

enum HidDescriptorType {
    Hid = 0x21,
    Report = 0x22,
    Physical = 0x23,
};

}