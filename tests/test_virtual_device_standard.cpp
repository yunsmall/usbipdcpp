// 虚拟设备标准请求与描述符语义测试
// 对照 USB 2.0 §9.4/§9.6 与 USB 3.0 §9.6：端点 halt、设备状态、
// BOS/Device_Qualifier 的存在条件、SuperSpeed 的 EP0 编码。

#include <gtest/gtest.h>

#include <memory>

#include "test_utils.h"

#include "usbipdcpp/Device.h"
#include "usbipdcpp/Interface.h"
#include "usbipdcpp/SetupPacket.h"
#include "usbipdcpp/utils/StringPool.h"
#include "usbipdcpp/virtual_device/SimpleVirtualDeviceHandler.h"
#include "usbipdcpp/virtual_device/VirtualInterfaceHandler.h"

using namespace usbipdcpp;
using namespace usbipdcpp::test;

namespace {

/// 暴露设备级标准请求入口的测试 handler
class ProbeDevice : public SimpleVirtualDeviceHandler {
public:
    using SimpleVirtualDeviceHandler::SimpleVirtualDeviceHandler;
    using VirtualDeviceHandler::get_bos_descriptor;
    using VirtualDeviceHandler::get_configuration_descriptor;
    using VirtualDeviceHandler::get_device_descriptor;
    using VirtualDeviceHandler::get_device_qualifier_descriptor;
    using VirtualDeviceHandler::request_clear_feature;
    using VirtualDeviceHandler::request_get_status;
    using VirtualDeviceHandler::request_set_descriptor;
    using VirtualDeviceHandler::request_set_feature;
};

struct DeviceEnv {
    StringPool pool;
    std::shared_ptr<UsbDevice> device;
    std::unique_ptr<ProbeDevice> handler;

    explicit DeviceEnv(UsbSpeed speed, Version usb_version = {2, 0, 0}) {
        device = UsbDevice::make("1-1", 0x1234, 0x5678, {}, 1, 1, 0, {}, speed);
        handler = std::make_unique<ProbeDevice>(*device, pool, usb_version);
    }
};

/// 暴露端点级标准请求入口的接口 handler
class ProbeInterface : public VirtualInterfaceHandler {
public:
    using VirtualInterfaceHandler::VirtualInterfaceHandler;
    using VirtualInterfaceHandler::is_endpoint_halted;
    using VirtualInterfaceHandler::request_endpoint_clear_feature;
    using VirtualInterfaceHandler::request_endpoint_get_status;
    using VirtualInterfaceHandler::request_endpoint_set_feature;
};

} // namespace

// ==================== Device_Qualifier / BOS 存在条件 ====================

TEST(VirtualDeviceStandard, FullSpeedHasNoDeviceQualifier) {
    DeviceEnv env(UsbSpeed::Full);
    std::uint32_t status = 0;
    auto desc = env.handler->get_device_qualifier_descriptor(0, 64, &status);
    EXPECT_EQ(status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
    EXPECT_TRUE(desc.empty());
}

TEST(VirtualDeviceStandard, HighSpeedReturnsDeviceQualifier) {
    DeviceEnv env(UsbSpeed::High);
    std::uint32_t status = 0;
    auto desc = env.handler->get_device_qualifier_descriptor(0, 64, &status);
    EXPECT_EQ(status, 0u);
    ASSERT_EQ(desc.size(), 10u);
    EXPECT_EQ(desc[0], 0x0A);
    EXPECT_EQ(desc[1], static_cast<std::uint8_t>(DescriptorType::DeviceQualifier));
}

TEST(VirtualDeviceStandard, BcdUsb20HasNoBos) {
    DeviceEnv env(UsbSpeed::High, {2, 0, 0});
    std::uint32_t status = 0;
    auto desc = env.handler->get_bos_descriptor(0, 64, &status);
    EXPECT_EQ(status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
    EXPECT_TRUE(desc.empty());
}

TEST(VirtualDeviceStandard, BcdUsb210ReturnsBosWithoutSsCapability) {
    DeviceEnv env(UsbSpeed::High, {2, 1, 0});
    std::uint32_t status = 0;
    auto desc = env.handler->get_bos_descriptor(0, 64, &status);
    EXPECT_EQ(status, 0u);
    ASSERT_EQ(desc.size(), 12u);
    EXPECT_EQ(desc[0], 0x05);
    EXPECT_EQ(desc[1], static_cast<std::uint8_t>(DescriptorType::BOS));
    EXPECT_EQ(desc[2], 12);
    EXPECT_EQ(desc[3], 0);
    EXPECT_EQ(desc[4], 1); // bNumCapabilities
}

TEST(VirtualDeviceStandard, SuperSpeedBosContainsSsCapability) {
    DeviceEnv env(UsbSpeed::Super, {3, 0, 0});
    std::uint32_t status = 0;
    auto desc = env.handler->get_bos_descriptor(0, 64, &status);
    EXPECT_EQ(status, 0u);
    ASSERT_EQ(desc.size(), 22u);
    EXPECT_EQ(desc[2], 22);
    EXPECT_EQ(desc[3], 0);
    EXPECT_EQ(desc[4], 2); // USB 2.0 Extension + SuperSpeed

    const data_type expect_ss = {0x0A, 0x10, 0x03, 0x00, 0x0E, 0x00, 0x03, 0x00, 0x00, 0x00};
    const data_type actual_ss(desc.begin() + 12, desc.end());
    EXPECT_EQ(actual_ss, expect_ss);
}

// ==================== SuperSpeed 设备描述符 ====================

TEST(VirtualDeviceStandard, SuperSpeedDeviceDescriptorUsesExponentForEp0) {
    DeviceEnv env(UsbSpeed::Super, {3, 0, 0});
    std::uint32_t status = 0;
    auto desc = env.handler->get_device_descriptor(0, 18, &status);
    ASSERT_EQ(desc.size(), 18u);
    // USB 3.0 §9.6.1：bMaxPacketSize0 的 SuperSpeed 唯一合法值是 9（即 512 字节）
    EXPECT_EQ(desc[7], 9);
}

TEST(VirtualDeviceStandard, FullSpeedDeviceDescriptorUsesActualEp0Size) {
    DeviceEnv env(UsbSpeed::Full);
    std::uint32_t status = 0;
    auto desc = env.handler->get_device_descriptor(0, 18, &status);
    ASSERT_EQ(desc.size(), 18u);
    EXPECT_EQ(desc[7], 64);
}

// ==================== 端点 HALT 状态机 ====================

TEST(VirtualDeviceStandard, EndpointHaltSetGetClear) {
    StringPool pool;
    UsbInterface intf{};
    ProbeInterface handler(intf, pool);

    std::uint32_t status = 0;
    handler.request_endpoint_set_feature(SetupPacket::USB_ENDPOINT_HALT, 0x81, &status);
    EXPECT_EQ(status, 0u);
    EXPECT_TRUE(handler.is_endpoint_halted(0x81));

    status = 0;
    EXPECT_EQ(handler.request_endpoint_get_status(0x81, &status), 1u);
    EXPECT_EQ(status, 0u);

    status = 0;
    handler.request_endpoint_clear_feature(SetupPacket::USB_ENDPOINT_HALT, 0x81, &status);
    EXPECT_EQ(status, 0u);
    EXPECT_FALSE(handler.is_endpoint_halted(0x81));
    EXPECT_EQ(handler.request_endpoint_get_status(0x81, &status), 0u);
}

TEST(VirtualDeviceStandard, EndpointZeroCannotBeHalted) {
    StringPool pool;
    UsbInterface intf{};
    ProbeInterface handler(intf, pool);

    std::uint32_t status = 0;
    handler.request_endpoint_set_feature(SetupPacket::USB_ENDPOINT_HALT, 0x00, &status);
    EXPECT_EQ(status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
    EXPECT_FALSE(handler.is_endpoint_halted(0x00));
}

TEST(VirtualDeviceStandard, UnknownEndpointFeatureStalls) {
    StringPool pool;
    UsbInterface intf{};
    ProbeInterface handler(intf, pool);

    std::uint32_t status = 0;
    handler.request_endpoint_set_feature(0x0001, 0x81, &status);
    EXPECT_EQ(status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
}

TEST(VirtualDeviceStandard, HaltedEndpointUrbCompletesWithEpipe) {
    StringPool pool;

    UsbInterface intf{};
    intf.endpoints = {{UsbEndpoint{.address = 0x81,
                                   .attributes = static_cast<std::uint8_t>(EndpointAttributes::Bulk),
                                   .max_packet_size = 64,
                                   .interval = 0}}};
    auto device = UsbDevice::make("1-1", 0x1234, 0x5678, {intf}, 1, 1, 0, {}, UsbSpeed::High);
    auto interface_handler = device->interfaces[0].with_handler<ProbeInterface>(pool);
    auto device_handler = device->with_handler<SimpleVirtualDeviceHandler>(pool);
    device_handler->setup_interface_handlers();

    CaptureResponder stub;
    error_code ec;
    device_handler->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    std::uint32_t status = 0;
    interface_handler->request_endpoint_set_feature(SetupPacket::USB_ENDPOINT_HALT, 0x81, &status);
    ASSERT_EQ(status, 0u);

    // 释放顺序：op 必须活过 cmd（cmd 的 TransferHandle 析构要用它）
    GenericTransferOperator op;
    auto cmd = make_cmd_submit(op, 7, 1, static_cast<std::uint32_t>(UsbIpDirection::In), 8);
    auto ep = device->interfaces[0].endpoints[0][0];

    device_handler->receive_urb(std::move(cmd), ep, std::optional<UsbInterface>(device->interfaces[0]), ec);
    EXPECT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(stub.submits[0].status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
}

TEST(VirtualDeviceStandard, ClearHaltAllowsTransferAgain) {
    StringPool pool;

    UsbInterface intf{};
    intf.endpoints = {{UsbEndpoint{.address = 0x81,
                                   .attributes = static_cast<std::uint8_t>(EndpointAttributes::Bulk),
                                   .max_packet_size = 64,
                                   .interval = 0}}};
    auto device = UsbDevice::make("1-1", 0x1234, 0x5678, {intf}, 1, 1, 0, {}, UsbSpeed::High);
    auto interface_handler = device->interfaces[0].with_handler<ProbeInterface>(pool);
    auto device_handler = device->with_handler<SimpleVirtualDeviceHandler>(pool);
    device_handler->setup_interface_handlers();

    CaptureResponder stub;
    error_code ec;
    device_handler->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    std::uint32_t status = 0;
    interface_handler->request_endpoint_set_feature(SetupPacket::USB_ENDPOINT_HALT, 0x81, &status);
    interface_handler->request_endpoint_clear_feature(SetupPacket::USB_ENDPOINT_HALT, 0x81, &status);
    ASSERT_EQ(status, 0u);

    // 未 halt 时请求进入 handler；SimpleVirtualDeviceHandler 的 bulk 默认实现回 EPIPE，
    // 但应答来自数据面而不是 halt 分支，端点此后可正常处理
    GenericTransferOperator op;
    auto cmd = make_cmd_submit(op, 8, 1, static_cast<std::uint32_t>(UsbIpDirection::In), 8);
    auto ep = device->interfaces[0].endpoints[0][0];
    device_handler->receive_urb(std::move(cmd), ep, std::optional<UsbInterface>(device->interfaces[0]), ec);
    ASSERT_EQ(stub.submits.size(), 1u);
}

// ==================== 设备状态（remote wakeup） ====================

TEST(VirtualDeviceStandard, RemoteWakeupNotSupportedByDefault) {
    DeviceEnv env(UsbSpeed::High);
    std::uint32_t status = 0;
    env.handler->request_set_feature(SetupPacket::USB_DEVICE_REMOTE_WAKEUP, &status);
    EXPECT_EQ(status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));

    status = 0;
    EXPECT_EQ(env.handler->request_get_status(&status), 0u);
    EXPECT_EQ(status, 0u);
}

TEST(VirtualDeviceStandard, RemoteWakeupStateFollowsFeatures) {
    DeviceEnv env(UsbSpeed::High);
    env.handler->set_remote_wakeup_support(true);

    std::uint32_t status = 0;
    env.handler->request_set_feature(SetupPacket::USB_DEVICE_REMOTE_WAKEUP, &status);
    EXPECT_EQ(status, 0u);
    EXPECT_EQ(env.handler->request_get_status(&status), 0x0002u);

    status = 0;
    env.handler->request_clear_feature(SetupPacket::USB_DEVICE_REMOTE_WAKEUP, &status);
    EXPECT_EQ(status, 0u);
    EXPECT_EQ(env.handler->request_get_status(&status), 0u);
}

TEST(VirtualDeviceStandard, RemoteWakeupBitReflectedInConfigurationDescriptor) {
    DeviceEnv env(UsbSpeed::High);
    std::uint32_t status = 0;
    auto without = env.handler->get_configuration_descriptor(0, 9, &status);
    ASSERT_GE(without.size(), 8u);
    EXPECT_EQ(without[7], 0x80); // bus powered，未声明 remote wakeup

    env.handler->set_remote_wakeup_support(true);
    auto with = env.handler->get_configuration_descriptor(0, 9, &status);
    ASSERT_GE(with.size(), 8u);
    EXPECT_EQ(with[7], 0xA0); // bus powered + remote wakeup
}

TEST(VirtualDeviceStandard, UnsupportedDeviceFeatureStalls) {
    DeviceEnv env(UsbSpeed::High);
    std::uint32_t status = 0;
    env.handler->request_set_feature(SetupPacket::USB_DEVICE_TEST_MODE, &status);
    EXPECT_EQ(status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
}

TEST(VirtualDeviceStandard, SetDescriptorStallsOnSimpleDevice) {
    DeviceEnv env(UsbSpeed::High);
    std::uint32_t status = 0;
    env.handler->request_set_descriptor(static_cast<std::uint8_t>(DescriptorType::Device), 0, 0, 0, {}, &status);
    EXPECT_EQ(status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
}
