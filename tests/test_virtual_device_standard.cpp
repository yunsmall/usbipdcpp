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
    using VirtualDeviceHandler::get_string_descriptor;
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

// ==================== 控制请求路由的规范语义 ====================

namespace {

/// 发一个控制 URB 到设备 handler（ep0）
/// @note op 必须由调用方持有并活得比 CaptureResponder 久（应答里的
///       TransferHandle 析构要用它）
void send_control(VirtualDeviceHandler &device, GenericTransferOperator &op, std::uint32_t seqnum,
                  UsbIpDirection direction, std::uint32_t length, const SetupPacket &setup) {
    auto cmd = make_cmd_submit(op, seqnum, 0, static_cast<std::uint32_t>(direction), length, setup);
    UsbEndpoint ep0{.address = 0x80,
                    .attributes = static_cast<std::uint8_t>(EndpointAttributes::Control),
                    .max_packet_size = 64,
                    .interval = 0};
    error_code ec;
    device.receive_urb(std::move(cmd), ep0, std::nullopt, ec);
}

} // namespace

TEST(VirtualDeviceStandard, InterfaceFeatureRequestsStall) {
    StringPool pool;
    UsbInterface intf{};
    ProbeInterface handler(intf, pool);

    // USB 2.0 Table 9-6：接口级没有合法的标准 feature selector，§9.4 要求 STALL
    std::uint32_t status = 0;
    handler.request_set_feature(0, &status);
    EXPECT_EQ(status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));

    status = 0;
    handler.request_clear_feature(0, &status);
    EXPECT_EQ(status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
}

TEST(VirtualDeviceStandard, ResetEndpointHaltStateClearsAllAlternates) {
    StringPool pool;
    UsbInterface intf{};
    intf.endpoints = {{{.address = 0x81,
                        .attributes = static_cast<std::uint8_t>(EndpointAttributes::Bulk),
                        .max_packet_size = 64,
                        .interval = 0}},
                      {{.address = 0x82,
                        .attributes = static_cast<std::uint8_t>(EndpointAttributes::Bulk),
                        .max_packet_size = 64,
                        .interval = 0}}};
    ProbeInterface handler(intf, pool);

    std::uint32_t status = 0;
    handler.request_endpoint_set_feature(SetupPacket::USB_ENDPOINT_HALT, 0x81, &status);
    handler.request_endpoint_set_feature(SetupPacket::USB_ENDPOINT_HALT, 0x82, &status);
    ASSERT_TRUE(handler.is_endpoint_halted(0x81));
    ASSERT_TRUE(handler.is_endpoint_halted(0x82));

    // USB 2.0 §9.4.5：SetConfiguration/SetInterface 后所有 alt 的端点 Halt 清零
    handler.reset_endpoint_halt_state();
    EXPECT_FALSE(handler.is_endpoint_halted(0x81));
    EXPECT_FALSE(handler.is_endpoint_halted(0x82));
}

TEST(VirtualDeviceStandard, SetConfigurationResetsEndpointHalt) {
    StringPool pool;
    UsbInterface intf{};
    intf.endpoints = {{{.address = 0x81,
                        .attributes = static_cast<std::uint8_t>(EndpointAttributes::Bulk),
                        .max_packet_size = 64,
                        .interval = 0}}};
    auto device = UsbDevice::make("1-1", 0x1234, 0x5678, {intf}, 1, 1, 0, {}, UsbSpeed::High);
    auto interface_handler = device->interfaces[0].with_handler<ProbeInterface>(pool);
    auto device_handler = device->with_handler<SimpleVirtualDeviceHandler>(pool);
    device_handler->setup_interface_handlers();

    GenericTransferOperator op;
    CaptureResponder stub;
    error_code ec;
    device_handler->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    std::uint32_t status = 0;
    interface_handler->request_endpoint_set_feature(SetupPacket::USB_ENDPOINT_HALT, 0x81, &status);
    ASSERT_TRUE(interface_handler->is_endpoint_halted(0x81));

    // SET_CONFIGURATION(1)：USB 2.0 §9.4.5 要求复位端点 Halt
    SetupPacket setup{.request_type = 0x00, .request = 9, .value = 1, .index = 0, .length = 0};
    send_control(*device_handler, op, 1, UsbIpDirection::Out, 0, setup);

    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(stub.submits[0].status, 0u);
    EXPECT_FALSE(interface_handler->is_endpoint_halted(0x81));
}

TEST(VirtualDeviceStandard, SetInterfaceResetsEndpointHalt) {
    StringPool pool;
    UsbInterface intf{};
    intf.endpoints = {{{.address = 0x81,
                        .attributes = static_cast<std::uint8_t>(EndpointAttributes::Bulk),
                        .max_packet_size = 64,
                        .interval = 0}}};
    auto device = UsbDevice::make("1-1", 0x1234, 0x5678, {intf}, 1, 1, 0, {}, UsbSpeed::High);
    auto interface_handler = device->interfaces[0].with_handler<ProbeInterface>(pool);
    auto device_handler = device->with_handler<SimpleVirtualDeviceHandler>(pool);
    device_handler->setup_interface_handlers();

    GenericTransferOperator op;
    CaptureResponder stub;
    error_code ec;
    device_handler->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    std::uint32_t status = 0;
    interface_handler->request_endpoint_set_feature(SetupPacket::USB_ENDPOINT_HALT, 0x81, &status);
    ASSERT_TRUE(interface_handler->is_endpoint_halted(0x81));

    // SET_INTERFACE(alt=0)：USB 2.0 §9.4.5 要求复位该接口端点 Halt
    SetupPacket setup{.request_type = 0x01, .request = 11, .value = 0, .index = 0, .length = 0};
    send_control(*device_handler, op, 2, UsbIpDirection::Out, 0, setup);

    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(stub.submits[0].status, 0u);
    EXPECT_FALSE(interface_handler->is_endpoint_halted(0x81));
}

TEST(VirtualDeviceStandard, InRequestClampedToWLength) {
    DeviceEnv env(UsbSpeed::High);

    GenericTransferOperator op;
    CaptureResponder stub;
    error_code ec;
    env.handler->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    // GET_STATUS 正常返回 2 字节；wLength=1 时 USB 2.0 §9.3.5 要求只返回 1 字节
    SetupPacket setup{.request_type = 0x80, .request = 0, .value = 0, .index = 0, .length = 1};
    send_control(*env.handler, op, 3, UsbIpDirection::In, 1, setup);

    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(stub.submits[0].actual_length, 1u);
}

TEST(VirtualDeviceStandard, StringDescriptorUsesWIndexAsLanguageId) {
    DeviceEnv env(UsbSpeed::High);
    std::uint32_t status = 0;

    // 索引 0 返回支持的 LANGID 列表（USB 2.0 §9.6.7）
    auto lang = env.handler->get_string_descriptor(0, 0, 255, &status);
    ASSERT_EQ(lang.size(), 4u);
    EXPECT_EQ(lang[0], 4);
    EXPECT_EQ(lang[2], 0x09);
    EXPECT_EQ(lang[3], 0x04);

    // 正确 LANGID 返回字符串（索引 2 = manufacturer）
    status = 0;
    auto manufacturer = env.handler->get_string_descriptor(0x0409, 2, 255, &status);
    EXPECT_EQ(status, 0u);
    EXPECT_GT(manufacturer.size(), 2u);

    // 不支持的 LANGID 回 STALL
    status = 0;
    auto bad = env.handler->get_string_descriptor(0x0407, 2, 255, &status);
    EXPECT_EQ(status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
    EXPECT_TRUE(bad.empty());
}

TEST(VirtualDeviceStandard, ReservedRequestTypeStalls) {
    DeviceEnv env(UsbSpeed::High);

    GenericTransferOperator op;
    CaptureResponder stub;
    error_code ec;
    env.handler->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    // USB 2.0 Table 9-2：Type=3 是 Reserved，不能交给类/厂商 handler
    SetupPacket setup{.request_type = 0x60, .request = 0, .value = 0, .index = 0, .length = 0};
    send_control(*env.handler, op, 4, UsbIpDirection::Out, 0, setup);

    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(stub.submits[0].status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
}
