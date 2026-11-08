#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <initializer_list>

#include "usbipdcpp/Interface.h"
#include "usbipdcpp/utils/StringPool.h"
#include "usbipdcpp/virtual_device/HidConstants.h"
#include "usbipdcpp/virtual_device/devices/KeyboardHandler.h"

using namespace usbipdcpp;

namespace {

struct KeyboardTestEnv {
    StringPool pool;
    UsbInterface intf = KeyboardHandler::make_interface(0x81);
    KeyboardHandler handler{intf, pool};
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
