#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "usbipdcpp/Interface.h"
#include "usbipdcpp/utils/StringPool.h"
#include "usbipdcpp/virtual_device/HidVirtualInterfaceHandler.h"

using namespace usbipdcpp;

namespace {
// 测试用 HID handler：返回固定的报告描述符（5 字节示例）
class TestHidHandler : public HidVirtualInterfaceHandler {
public:
    explicit TestHidHandler(UsbInterface &intf, StringPool &pool) : HidVirtualInterfaceHandler(intf, pool) {
    }

    data_type get_report_descriptor() override {
        return {0x05, 0x01, 0x09, 0x06, 0xC0};
    }

    std::uint16_t get_report_descriptor_size() override {
        return 5;
    }

    // 测试用公开包装，转发到 protected 的 has_pending_input_reports()
    bool has_pending_reports_for_test() const {
        return has_pending_input_reports();
    }
};

// 构造一个最小可用的 UsbInterface 与 StringPool
struct HidTestEnv {
    StringPool pool;
    UsbInterface intf{.interface_class = 3, .interface_subclass = 0, .interface_protocol = 0};
    TestHidHandler handler{intf, pool};
};
} // namespace

// ========== HID 描述符 ==========

TEST(VirtualDeviceHandlers, HidGetReportDescriptor) {
    HidTestEnv env;

    std::uint32_t status = 0;
    auto desc = env.handler.request_get_descriptor(HidDescriptorType::Report, 0, 64, &status);
    ASSERT_EQ(status, 0u);
    ASSERT_EQ(desc.size(), 5u);
    EXPECT_EQ(desc[0], 0x05);
    EXPECT_EQ(desc[4], 0xC0);
}

TEST(VirtualDeviceHandlers, HidGetUnsupportedDescriptorType) {
    HidTestEnv env;

    std::uint32_t status = 0;
    auto desc = env.handler.request_get_descriptor(HidDescriptorType::Physical, 0, 64, &status);
    EXPECT_EQ(status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
    EXPECT_TRUE(desc.empty());
}

TEST(VirtualDeviceHandlers, HidClassSpecificDescriptor) {
    HidTestEnv env;

    auto desc = env.handler.get_class_specific_descriptor();
    // bLength=9, bDescriptorType=0x21(HID), bcdHID=1.11, bCountryCode=0,
    // bNumDescriptors=1, bDescriptorType[0]=0x22(Report), wDescriptorLength=5（小端）
    const data_type expect = {0x09, 0x21, 0x11, 0x01, 0x00, 0x01, 0x22, 0x05, 0x00};
    EXPECT_EQ(desc, expect);
}

// ========== HID 类请求默认实现（未重写时返回 EPIPE） ==========

TEST(VirtualDeviceHandlers, HidIdleDefaultsToStall) {
    HidTestEnv env;

    std::uint32_t status = 0;
    auto ret = env.handler.request_get_idle(0, 0, 1, &status);
    EXPECT_EQ(status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
    EXPECT_TRUE(ret.empty());

    status = 0;
    env.handler.request_set_idle(0, &status);
    EXPECT_EQ(status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
}

TEST(VirtualDeviceHandlers, HidReportDefaultsToStall) {
    HidTestEnv env;

    std::uint32_t status = 0;
    auto ret = env.handler.request_get_report(1, 0, 8, &status);
    EXPECT_EQ(status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
    EXPECT_TRUE(ret.empty());

    status = 0;
    env.handler.request_set_report(1, 0, 8, {}, &status);
    EXPECT_EQ(status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
}

// ========== HID 标准请求默认实现 ==========

TEST(VirtualDeviceHandlers, HidStandardRequests) {
    HidTestEnv env;

    std::uint32_t status = 1;
    env.handler.request_clear_feature(0, &status);
    EXPECT_EQ(status, 0u);

    status = 0;
    EXPECT_EQ(env.handler.request_get_interface(&status), 0u);
    EXPECT_EQ(status, 0u);

    status = 0;
    env.handler.request_set_feature(0, &status);
    EXPECT_EQ(status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
}

// ========== HID 输入报告队列 ==========

TEST(VirtualDeviceHandlers, HidSendInputReportQueuesWhenNoRequest) {
    HidTestEnv env;

    EXPECT_FALSE(env.handler.has_pending_reports_for_test());

    // 没有排队的端点请求时，报告进入 pending 队列
    const std::array<std::uint8_t, 3> report = {1, 2, 3};
    env.handler.send_input_report(asio::buffer(report));
    EXPECT_TRUE(env.handler.has_pending_reports_for_test());

    // 再次发送继续排队
    env.handler.send_input_report(asio::buffer(report));
    EXPECT_TRUE(env.handler.has_pending_reports_for_test());

    // 断连时清空队列
    std::error_code ec;
    env.handler.on_disconnection(ec);
    EXPECT_FALSE(env.handler.has_pending_reports_for_test());
}
