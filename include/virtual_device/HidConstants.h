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

/// USB HID 标准按键码（Usage Page 0x07 Keyboard/Keypad）
namespace HIDKey {
    constexpr std::uint8_t NoEvent = 0x00;
    constexpr std::uint8_t ErrorRollOver = 0x01;

    // 字母 A-Z: 0x04–0x1D
    constexpr std::uint8_t A = 0x04, B = 0x05, C = 0x06, D = 0x07;
    constexpr std::uint8_t E = 0x08, F = 0x09, G = 0x0A, H = 0x0B;
    constexpr std::uint8_t I = 0x0C, J = 0x0D, K = 0x0E, L = 0x0F;
    constexpr std::uint8_t M = 0x10, N = 0x11, O = 0x12, P = 0x13;
    constexpr std::uint8_t Q = 0x14, R = 0x15, S = 0x16, T = 0x17;
    constexpr std::uint8_t U = 0x18, V = 0x19, W = 0x1A, X = 0x1B;
    constexpr std::uint8_t Y = 0x1C, Z = 0x1D;

    // 主键盘数字: 0x1E–0x27
    constexpr std::uint8_t Num1 = 0x1E, Num2 = 0x1F, Num3 = 0x20, Num4 = 0x21;
    constexpr std::uint8_t Num5 = 0x22, Num6 = 0x23, Num7 = 0x24, Num8 = 0x25;
    constexpr std::uint8_t Num9 = 0x26, Num0 = 0x27;

    constexpr std::uint8_t Enter = 0x28;
    constexpr std::uint8_t Escape = 0x29;
    constexpr std::uint8_t Backspace = 0x2A;
    constexpr std::uint8_t Tab = 0x2B;
    constexpr std::uint8_t Space = 0x2C;
    constexpr std::uint8_t Minus = 0x2D;
    constexpr std::uint8_t Equals = 0x2E;
    constexpr std::uint8_t LeftBracket = 0x2F;
    constexpr std::uint8_t RightBracket = 0x30;
    constexpr std::uint8_t Backslash = 0x31;
    constexpr std::uint8_t Semicolon = 0x33;
    constexpr std::uint8_t Apostrophe = 0x34;
    constexpr std::uint8_t Grave = 0x35;
    constexpr std::uint8_t Comma = 0x36;
    constexpr std::uint8_t Period = 0x37;
    constexpr std::uint8_t Slash = 0x38;
    constexpr std::uint8_t CapsLock = 0x39;

    // F1–F12: 0x3A–0x45
    constexpr std::uint8_t F1 = 0x3A, F2 = 0x3B, F3 = 0x3C, F4 = 0x3D;
    constexpr std::uint8_t F5 = 0x3E, F6 = 0x3F, F7 = 0x40, F8 = 0x41;
    constexpr std::uint8_t F9 = 0x42, F10 = 0x43, F11 = 0x44, F12 = 0x45;

    constexpr std::uint8_t PrintScreen = 0x46;
    constexpr std::uint8_t ScrollLock = 0x47;
    constexpr std::uint8_t Pause = 0x48;
    constexpr std::uint8_t Insert = 0x49;
    constexpr std::uint8_t Home = 0x4A;
    constexpr std::uint8_t PageUp = 0x4B;
    constexpr std::uint8_t Delete = 0x4C;
    constexpr std::uint8_t End = 0x4D;
    constexpr std::uint8_t PageDown = 0x4E;
    constexpr std::uint8_t RightArrow = 0x4F;
    constexpr std::uint8_t LeftArrow = 0x50;
    constexpr std::uint8_t DownArrow = 0x51;
    constexpr std::uint8_t UpArrow = 0x52;

    // 小键盘: 0x53–0x63
    constexpr std::uint8_t KeypadNumLock = 0x53;
    constexpr std::uint8_t KeypadSlash = 0x54;
    constexpr std::uint8_t KeypadAsterisk = 0x55;
    constexpr std::uint8_t KeypadMinus = 0x56;
    constexpr std::uint8_t KeypadPlus = 0x57;
    constexpr std::uint8_t KeypadEnter = 0x58;
    constexpr std::uint8_t Keypad1 = 0x59, Keypad2 = 0x5A, Keypad3 = 0x5B;
    constexpr std::uint8_t Keypad4 = 0x5C, Keypad5 = 0x5D, Keypad6 = 0x5E;
    constexpr std::uint8_t Keypad7 = 0x5F, Keypad8 = 0x60, Keypad9 = 0x61;
    constexpr std::uint8_t Keypad0 = 0x62;
    constexpr std::uint8_t KeypadPeriod = 0x63;

    constexpr std::uint8_t Application = 0x65;
    constexpr std::uint8_t Power = 0x66;
} // namespace HIDKey

/// USB HID Consumer Page (0x0C) Usage ID — 媒体键 / 系统控制
namespace HIDConsumer {
    constexpr std::uint16_t PlayPause = 0x00CD;
    constexpr std::uint16_t Stop = 0x00B7;
    constexpr std::uint16_t ScanNextTrack = 0x00B5;
    constexpr std::uint16_t ScanPrevTrack = 0x00B6;
    constexpr std::uint16_t FastForward = 0x00B3;
    constexpr std::uint16_t Rewind = 0x00B4;
    constexpr std::uint16_t Mute = 0x00E2;
    constexpr std::uint16_t VolumeInc = 0x00E9;
    constexpr std::uint16_t VolumeDec = 0x00EA;
    constexpr std::uint16_t BassBoost = 0x00E5;
    constexpr std::uint16_t Eject = 0x00B8;
    constexpr std::uint16_t ALCC = 0x00CF; // 自动电平控制（部分键盘用作计算器）
    constexpr std::uint16_t ALTaskManager = 0x0192;
    constexpr std::uint16_t ALFileExplorer = 0x0194;
    constexpr std::uint16_t ALWWW = 0x0223; // 浏览器
    constexpr std::uint16_t Power = 0x0030;
    constexpr std::uint16_t Sleep = 0x0032;
    constexpr std::uint16_t Wake = 0x0083;
} // namespace HIDConsumer
} // namespace usbipdcpp
