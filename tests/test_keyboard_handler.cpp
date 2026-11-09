#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <initializer_list>

#include "usbipdcpp/Interface.h"
#include "usbipdcpp/virtual_device/HidConstants.h"
#include "usbipdcpp/virtual_device/devices/KeyboardHandler.h"

using namespace usbipdcpp;

namespace {

struct KeyboardTestEnv {
    UsbInterface intf = KeyboardHandler::make_interface(0x81);
    KeyboardHandler handler{intf};
};

bool contains_sequence(const data_type &data, std::initializer_list<std::uint8_t> seq) {
    return std::search(data.begin(), data.end(), seq.begin(), seq.end()) != data.end();
}

} // namespace

// 报告描述符必须声明 LED Output，否则内核不会创建 capslock 等 LED class 设备，
// 主机永远不会下发 SET_REPORT（LED API 形同虚设）
TEST(TestKeyboardHandler, ReportDescriptorDeclaresLedOutput) {
    KeyboardTestEnv env;
    auto desc = env.handler.get_report_descriptor();
    EXPECT_TRUE(contains_sequence(desc, {0x05, 0x08, // Usage Page (LEDs)
                                         0x19, 0x01, // Usage Minimum (Num Lock)
                                         0x29, 0x05, // Usage Maximum (Kana)
                                         0x15, 0x00, // Logical Minimum (0)
                                         0x25, 0x01, // Logical Maximum (1)
                                         0x95, 0x05, // Report Count (5)
                                         0x75, 0x01, // Report Size (1)
                                         0x91, 0x02})); // Output (Data,Var,Abs)
    EXPECT_EQ(desc.size(), env.handler.get_report_descriptor_size());
}

TEST(TestKeyboardHandler, SetReportOutputUpdatesLedStatus) {
    KeyboardTestEnv env;
    std::uint32_t status = 0xFFFFFFFF;
    env.handler.request_set_report(static_cast<std::uint8_t>(HIDReportType::Output), 0, 1, {0x02}, &status);
    EXPECT_EQ(status, 0);
    EXPECT_EQ(env.handler.get_led_status(), 0x02);
}

// 部分主机在数据前带 Report ID 前缀（长度 2）：取负载字节而不是前缀
TEST(TestKeyboardHandler, SetReportWithReportIdPrefixUsesPayload) {
    KeyboardTestEnv env;
    std::uint32_t status = 0;
    env.handler.request_set_report(static_cast<std::uint8_t>(HIDReportType::Output), 1, 2, {0x01, 0x03}, &status);
    EXPECT_EQ(status, 0);
    EXPECT_EQ(env.handler.get_led_status(), 0x03);
}

// 非 Output 类型的 SET_REPORT 不得改动 LED 状态
TEST(TestKeyboardHandler, SetReportNonOutputLeavesLedUntouched) {
    KeyboardTestEnv env;
    std::uint32_t status = 0;
    env.handler.request_set_report(static_cast<std::uint8_t>(HIDReportType::Output), 0, 1, {0x01}, &status);
    env.handler.request_set_report(static_cast<std::uint8_t>(HIDReportType::Input), 0, 1, {0x00}, &status);
    EXPECT_EQ(env.handler.get_led_status(), 0x01);
}

// HID 1.11 §7.2.1：GET_REPORT 强制支持，Output 报告（LED）必须返回当前值
TEST(TestKeyboardHandler, GetReportOutputReturnsLedState) {
    KeyboardTestEnv env;
    std::uint32_t status = 0xFFFFFFFF;
    env.handler.request_set_report(static_cast<std::uint8_t>(HIDReportType::Output), 1, 2, {0x01, 0x02}, &status);
    ASSERT_EQ(status, 0u);

    status = 0xFFFFFFFF;
    auto report = env.handler.request_get_report(static_cast<std::uint8_t>(HIDReportType::Output), 1, 1, &status);
    EXPECT_EQ(status, 0u);
    EXPECT_EQ(report, (data_type{0x02}));
}

// 没有定义 Output 报告的 Report ID 要 STALL，不能返回伪造数据
TEST(TestKeyboardHandler, GetReportOutputUnknownIdStalls) {
    KeyboardTestEnv env;
    std::uint32_t status = 0;
    auto report = env.handler.request_get_report(static_cast<std::uint8_t>(HIDReportType::Output), 2, 1, &status);
    EXPECT_NE(status, 0u);
    EXPECT_TRUE(report.empty());
}

// HID 1.11 §7.1.1：接口级 GET_DESCRIPTOR(HID, 0x21) 必须返回 9 字节
// HID 类描述符，其中 wDescriptorLength 是报告描述符长度
TEST(TestKeyboardHandler, GetDescriptorHidReturnsClassDescriptor) {
    KeyboardTestEnv env;
    std::uint32_t status = 0;
    auto desc = env.handler.request_get_descriptor(0x21, 0, 9, &status);
    EXPECT_EQ(status, 0u);
    ASSERT_EQ(desc.size(), 9u);
    EXPECT_EQ(desc[0], 0x09); // bLength
    EXPECT_EQ(desc[1], 0x21); // bDescriptorType HID
    EXPECT_EQ(desc[2], 0x11); // bcdHID 1.11
    EXPECT_EQ(desc[3], 0x01);
    EXPECT_EQ(desc[4], 0x00); // bCountryCode
    EXPECT_EQ(desc[5], 0x01); // bNumDescriptors
    EXPECT_EQ(desc[6], 0x22); // bDescriptorType[0] Report
    EXPECT_EQ(static_cast<std::uint16_t>(desc[7] | (desc[8] << 8)),
              env.handler.get_report_descriptor_size());
}

// HID 1.11 §7.2.6：上电默认 Report Protocol，GET_PROTOCOL 必须返回 1
TEST(TestKeyboardHandler, GetProtocolDefaultsToReport) {
    KeyboardTestEnv env;
    std::uint32_t status = 0xFFFFFFFF;
    EXPECT_EQ(env.handler.request_get_protocol(&status), 1);
    EXPECT_EQ(status, 0);
}

// HID 1.11 §7.2.6 + Appendix B.1：切到 Boot 协议后报告变成固定 8 字节，
// 无 Report ID：修饰键、保留字节、6 个按键码
TEST(TestKeyboardHandler, BootProtocolSwitchesToEightByteReport) {
    KeyboardTestEnv env;
    std::uint32_t status = 0xFFFFFFFF;
    env.handler.request_set_protocol(0, &status);
    EXPECT_EQ(status, 0);
    EXPECT_EQ(env.handler.request_get_protocol(&status), 0);

    env.handler.set_modifier(0x02); // Left Shift
    env.handler.press_key(HIDKey::A);

    auto report = env.handler.request_get_report(static_cast<std::uint8_t>(HIDReportType::Input), 1, 8, &status);
    EXPECT_EQ(status, 0);
    EXPECT_EQ(report.size(), 8u);
    EXPECT_EQ(report[0], 0x02); // 修饰键
    EXPECT_EQ(report[1], 0x00); // 保留
    EXPECT_EQ(report[2], HIDKey::A); // 第一个按键码
    for (std::size_t i = 3; i < 8; ++i) {
        EXPECT_EQ(report[i], 0x00);
    }
}

// 非法协议值必须 STALL，且不改变当前协议
TEST(TestKeyboardHandler, SetProtocolRejectsInvalidValue) {
    KeyboardTestEnv env;
    std::uint32_t status = 0;
    env.handler.request_set_protocol(2, &status);
    EXPECT_NE(status, 0u); // StatusEPIPE
    status = 0;
    EXPECT_EQ(env.handler.request_get_protocol(&status), 1); // 仍为 Report
}

// Report 协议保持 9 字节报告（首字节 Report ID=1）
TEST(TestKeyboardHandler, ReportProtocolKeepsNineByteReport) {
    KeyboardTestEnv env;
    std::uint32_t status = 0;
    env.handler.press_key(HIDKey::Enter);
    auto report = env.handler.request_get_report(static_cast<std::uint8_t>(HIDReportType::Input), 1, 9, &status);
    EXPECT_EQ(status, 0);
    EXPECT_EQ(report.size(), 9u);
    EXPECT_EQ(report[0], 0x01); // REPORT_ID_KEYBOARD
    EXPECT_EQ(report[3], HIDKey::Enter);
}
