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
#include "usbipdcpp/virtual_device/UsbClassConstants.h"
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
    using VirtualDeviceHandler::get_other_speed_descriptor;
    using VirtualDeviceHandler::get_string_descriptor;
    using VirtualDeviceHandler::request_clear_feature;
    using VirtualDeviceHandler::request_get_configuration;
    using VirtualDeviceHandler::request_get_status;
    using VirtualDeviceHandler::request_set_descriptor;
    using VirtualDeviceHandler::request_set_feature;
    using VirtualDeviceHandler::request_set_isoch_delay;
    using VirtualDeviceHandler::request_set_sel;
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

    // 退出时延取最小非零值（01H / 0001H，小于 1μs）：0 在 Linux 里会关闭 LPM
    const data_type expect_ss = {0x0A, 0x10, 0x03, 0x00, 0x0E, 0x00, 0x03, 0x01, 0x01, 0x00};
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
    // Halt 只对中断/Bulk 端点有效（USB 2.0 §9.4.5），接口里必须声明端点
    intf.endpoints = {{{.address = 0x81,
                        .attributes = static_cast<std::uint8_t>(EndpointAttributes::Bulk),
                        .max_packet_size = 64,
                        .interval = 0}}};
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

TEST(VirtualDeviceStandard, IsochronousEndpointCannotBeHalted) {
    // USB 2.0 §9.4.5：Halt 特性只为中断和 Bulk 端点实现，等时端点
    // SET/CLEAR_FEATURE(ENDPOINT_HALT) 必须回 Request Error
    StringPool pool;
    UsbInterface intf{};
    intf.endpoints = {{{.address = 0x81,
                        .attributes = static_cast<std::uint8_t>(EndpointAttributes::Isochronous),
                        .max_packet_size = 1024,
                        .interval = 1}}};
    ProbeInterface handler(intf, pool);

    std::uint32_t status = 0;
    handler.request_endpoint_set_feature(SetupPacket::USB_ENDPOINT_HALT, 0x81, &status);
    EXPECT_EQ(status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
    EXPECT_FALSE(handler.is_endpoint_halted(0x81));

    status = 0;
    handler.request_endpoint_clear_feature(SetupPacket::USB_ENDPOINT_HALT, 0x81, &status);
    EXPECT_EQ(status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
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
    // 0x82 只存在于 alt 1，切到 alt 1 后才是当前端点
    intf.current_altsetting = 1;
    handler.request_endpoint_set_feature(SetupPacket::USB_ENDPOINT_HALT, 0x82, &status);
    intf.current_altsetting = 0;
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

// ==================== UNLINK 多接口汇总 ====================

namespace {

/// 记录设备级 UNLINK 是否查询到本接口的探测 handler
class UnlinkProbeHandler : public VirtualInterfaceHandler {
public:
    using VirtualInterfaceHandler::VirtualInterfaceHandler;

    bool cancel_pending_urb(std::uint32_t unlink_seqnum) override {
        (void) unlink_seqnum;
        ++cancel_queries;
        return cancel_result;
    }

    bool cancel_result = false;
    int cancel_queries = 0;
};

/// 双接口设备：一条 UNLINK 只能回一条 RET_UNLINK
struct UnlinkEnv {
    StringPool pool;
    std::shared_ptr<UsbDevice> device;
    std::unique_ptr<ProbeDevice> handler;
    CaptureResponder stub;
    error_code ec;
    UnlinkProbeHandler *first = nullptr;
    UnlinkProbeHandler *second = nullptr;

    UnlinkEnv() {
        device = std::make_shared<UsbDevice>(UsbDevice{
                .path = "/test/unlink",
                .busid = "1-1",
                .bus_num = 1,
                .dev_num = 1,
                .speed = static_cast<std::uint32_t>(UsbSpeed::Full),
                .vendor_id = 0x1234,
                .product_id = 0x5678,
                .device_bcd = 0x0100,
                .device_class = 0x00,
                .device_subclass = 0x00,
                .device_protocol = 0x00,
                .configuration_value = 1,
                .num_configurations = 1,
                .interfaces =
                        {
                                UsbInterface{.interface_class = 0xFF,
                                             .endpoints = {{UsbEndpoint{.address = 0x81,
                                                                        .attributes = 0x02,
                                                                        .max_packet_size = 64,
                                                                        .interval = 0}}}},
                                UsbInterface{.interface_class = 0xFF,
                                             .endpoints = {{UsbEndpoint{.address = 0x82,
                                                                        .attributes = 0x02,
                                                                        .max_packet_size = 64,
                                                                        .interval = 0}}}},
                        },
                .ep0_in = UsbEndpoint::get_ep0_in(UsbSpeed::Full),
                .ep0_out = UsbEndpoint::get_ep0_out(UsbSpeed::Full),
        });
        handler = std::make_unique<ProbeDevice>(*device, pool);
        first = device->interfaces[0].with_handler<UnlinkProbeHandler>(pool).get();
        second = device->interfaces[1].with_handler<UnlinkProbeHandler>(pool).get();
        handler->setup_interface_handlers();
        handler->on_new_connection(stub, ec);
    }
};

} // namespace

TEST(VirtualDeviceStandard, UnlinkOnMultiInterfaceDeviceRespondsOnce) {
    // 对齐内核 vudc v_recv_cmd_unlink：一条 UNLINK 只回一条 RET_UNLINK。
    // 多接口设备若每个接口各自应答，主机会收到重复应答
    UnlinkEnv env;
    ASSERT_FALSE(env.ec);
    env.first->cancel_result = true;

    env.handler->handle_unlink_seqnum(7, 9);

    ASSERT_EQ(env.stub.unlinks.size(), 1u);
    EXPECT_EQ(env.stub.unlinks[0].header.seqnum, 9u);
    EXPECT_EQ(env.stub.unlinks[0].status, static_cast<std::uint32_t>(UrbStatusType::StatusECONNRESET));
    // 第一个接口命中后不再查询后续接口（seqnum 全局唯一，不可能被两个接口挂起）
    EXPECT_EQ(env.first->cancel_queries, 1);
    EXPECT_EQ(env.second->cancel_queries, 0);
}

TEST(VirtualDeviceStandard, UnlinkQueriesAllInterfacesOnMiss) {
    // 目标不在任何接口：所有接口都查询一次，只回一条 RET_UNLINK(0)
    UnlinkEnv env;
    ASSERT_FALSE(env.ec);

    env.handler->handle_unlink_seqnum(7, 9);

    ASSERT_EQ(env.stub.unlinks.size(), 1u);
    EXPECT_EQ(env.stub.unlinks[0].status, 0u);
    EXPECT_EQ(env.first->cancel_queries, 1);
    EXPECT_EQ(env.second->cancel_queries, 1);
}

TEST(VirtualDeviceStandard, UnlinkFindsLaterInterface) {
    UnlinkEnv env;
    ASSERT_FALSE(env.ec);
    env.second->cancel_result = true;

    env.handler->handle_unlink_seqnum(7, 9);

    ASSERT_EQ(env.stub.unlinks.size(), 1u);
    EXPECT_EQ(env.stub.unlinks[0].status, static_cast<std::uint32_t>(UrbStatusType::StatusECONNRESET));
    EXPECT_EQ(env.first->cancel_queries, 1);
    EXPECT_EQ(env.second->cancel_queries, 1);
}

// ==================== SET_CONFIGURATION 状态 ====================

TEST(VirtualDeviceStandard, SetConfigurationZeroUnconfiguresDevice) {
    // USB 2.0 §9.4.7：SET_CONFIGURATION(0) 让设备回到 Address 状态；
    // §9.4.2：未配置时 GET_CONFIGURATION 必须返回 0
    StringPool pool;
    auto device = UsbDevice::make("1-1", 0x1234, 0x5678, {}, 1, 1, 0, {}, UsbSpeed::Full);
    auto device_handler = device->with_handler<ProbeDevice>(pool);
    GenericTransferOperator op;
    CaptureResponder stub;
    error_code ec;
    device_handler->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    // 新连接 = 总线复位，设备未配置
    std::uint32_t status = 1;
    EXPECT_EQ(device_handler->request_get_configuration(&status), 0);

    // SET_CONFIGURATION(1)
    SetupPacket set{.request_type = 0x00, .request = 9, .value = 1, .index = 0, .length = 0};
    send_control(*device_handler, op, 1, UsbIpDirection::Out, 0, set);
    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(stub.submits[0].status, 0u);

    // GET_CONFIGURATION 走控制传输：返回 1
    SetupPacket get{.request_type = 0x80, .request = 8, .value = 0, .index = 0, .length = 1};
    send_control(*device_handler, op, 2, UsbIpDirection::In, 1, get);
    ASSERT_EQ(stub.submits.size(), 2u);
    EXPECT_EQ(stub.submits[1].status, 0u);
    EXPECT_EQ(ret_submit_data(stub.submits[1]), data_type{1});

    // SET_CONFIGURATION(0) → 未配置，GET_CONFIGURATION 返回 0
    SetupPacket unset{.request_type = 0x00, .request = 9, .value = 0, .index = 0, .length = 0};
    send_control(*device_handler, op, 3, UsbIpDirection::Out, 0, unset);
    ASSERT_EQ(stub.submits.size(), 3u);
    EXPECT_EQ(stub.submits[2].status, 0u);

    send_control(*device_handler, op, 4, UsbIpDirection::In, 1, get);
    ASSERT_EQ(stub.submits.size(), 4u);
    EXPECT_EQ(stub.submits[3].status, 0u);
    EXPECT_EQ(ret_submit_data(stub.submits[3]), data_type{0});

    device_handler->on_disconnection(ec);
}

namespace {

/// 记录 SET_INTERFACE 收到的 alt
class AltProbeHandler : public VirtualInterfaceHandler {
public:
    using VirtualInterfaceHandler::VirtualInterfaceHandler;

    void request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) override {
        last_alt = alternate_setting;
        VirtualInterfaceHandler::request_set_interface(alternate_setting, p_status);
    }

    int last_alt = -1;
};

} // namespace

TEST(VirtualDeviceStandard, SetConfigurationResetsAlternateSettings) {
    // USB 2.0 §9.6.5：接口默认设置恒为 alt 0；§9.1.1.5：配置时受影响接口的
    // 端点状态/配置值复位到默认值
    StringPool pool;
    UsbInterface intf{};
    intf.endpoints = {
            {{.address = 0x81,
              .attributes = static_cast<std::uint8_t>(EndpointAttributes::Bulk),
              .max_packet_size = 64,
              .interval = 0}},
            {{.address = 0x82,
              .attributes = static_cast<std::uint8_t>(EndpointAttributes::Bulk),
              .max_packet_size = 64,
              .interval = 0}},
    };
    auto device = UsbDevice::make("1-1", 0x1234, 0x5678, {intf}, 1, 1, 0, {}, UsbSpeed::Full);
    auto alt_handler = device->interfaces[0].with_handler<AltProbeHandler>(pool);
    auto device_handler = device->with_handler<SimpleVirtualDeviceHandler>(pool);
    device_handler->setup_interface_handlers();

    GenericTransferOperator op;
    CaptureResponder stub;
    error_code ec;
    device_handler->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    // 先切到 alt 1
    SetupPacket set_intf{.request_type = 0x01, .request = 11, .value = 1, .index = 0, .length = 0};
    send_control(*device_handler, op, 1, UsbIpDirection::Out, 0, set_intf);
    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(stub.submits[0].status, 0u);
    EXPECT_EQ(device->interfaces[0].current_altsetting, 1);
    EXPECT_EQ(alt_handler->last_alt, 1);

    // SET_CONFIGURATION(1) 后接口回到 alt 0
    SetupPacket set_cfg{.request_type = 0x00, .request = 9, .value = 1, .index = 0, .length = 0};
    send_control(*device_handler, op, 2, UsbIpDirection::Out, 0, set_cfg);
    ASSERT_EQ(stub.submits.size(), 2u);
    EXPECT_EQ(stub.submits[1].status, 0u);
    EXPECT_EQ(device->interfaces[0].current_altsetting, 0);
    EXPECT_EQ(alt_handler->last_alt, 0);

    device_handler->on_disconnection(ec);
}

TEST(VirtualDeviceStandard, EmptyEndpointInterfaceAcceptsAltZero) {
    // USB 2.0 §9.6.5：bNumEndpoints=0 的接口只接受默认 alt 0
    StringPool pool;
    UsbInterface intf{};
    ProbeInterface handler(intf, pool);

    std::uint32_t status = 1;
    handler.request_set_interface(0, &status);
    EXPECT_EQ(status, 0u);

    status = 0;
    handler.request_set_interface(1, &status);
    EXPECT_EQ(status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
}

// ==================== 接口级请求按 interface_number 路由 ====================

namespace {

/// 记录接口级请求落在哪个 handler 上，用于验证路由目标
class RoutingProbeHandler : public VirtualInterfaceHandler {
public:
    using VirtualInterfaceHandler::VirtualInterfaceHandler;

    void request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) override {
        last_alt = alternate_setting;
        VirtualInterfaceHandler::request_set_interface(alternate_setting, p_status);
    }

    std::uint8_t request_get_interface(std::uint32_t *p_status) override {
        *p_status = 0;
        return interface_marker;
    }

    void handle_non_standard_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                      std::uint32_t transfer_flags,
                                                      std::uint32_t transfer_buffer_length,
                                                      const SetupPacket &setup, TransferHandle transfer,
                                                      std::error_code &ec) override {
        (void) seqnum;
        (void) ep;
        (void) transfer_flags;
        (void) transfer_buffer_length;
        (void) transfer;
        (void) ec;
        last_class_request = setup.value;
    }

    int last_alt = -1;
    std::uint16_t last_class_request = 0;
    std::uint8_t interface_marker = 0;
};

} // namespace

TEST(VirtualDeviceStandard, InterfaceRequestsRouteByInterfaceNumber) {
    // 接口级控制请求的 wIndex 是 bInterfaceNumber，不是 interfaces 数组下标。
    // 复合设备的接口排列顺序可以与编号不一致（见 UsbInterface::interface_number），
    // 按数组下标路由会把 SET_INTERFACE/GET_INTERFACE/类请求发给错误的 handler
    StringPool pool;
    UsbInterface number_five{};
    number_five.endpoints = {
            {{.address = 0x81,
              .attributes = static_cast<std::uint8_t>(EndpointAttributes::Bulk),
              .max_packet_size = 64,
              .interval = 0}},
            {{.address = 0x83,
              .attributes = static_cast<std::uint8_t>(EndpointAttributes::Bulk),
              .max_packet_size = 64,
              .interval = 0}},
    };
    UsbInterface number_two{};
    number_two.endpoints = {
            {{.address = 0x82,
              .attributes = static_cast<std::uint8_t>(EndpointAttributes::Bulk),
              .max_packet_size = 64,
              .interval = 0}},
            {{.address = 0x84,
              .attributes = static_cast<std::uint8_t>(EndpointAttributes::Bulk),
              .max_packet_size = 64,
              .interval = 0}},
    };
    auto device = UsbDevice::make("1-1", 0x1234, 0x5678, {number_five, number_two}, 1, 1, 0, {}, UsbSpeed::Full);
    device->interfaces[0].interface_number = 5;
    device->interfaces[1].interface_number = 2;
    auto *five = device->interfaces[0].with_handler<RoutingProbeHandler>(pool).get();
    five->interface_marker = 0xA5;
    auto *two = device->interfaces[1].with_handler<RoutingProbeHandler>(pool).get();
    two->interface_marker = 0x22;
    auto device_handler = device->with_handler<SimpleVirtualDeviceHandler>(pool);
    device_handler->setup_interface_handlers();

    GenericTransferOperator op;
    CaptureResponder stub;
    error_code ec;
    device_handler->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    // SET_INTERFACE 发往接口号 2（数组下标 1）
    SetupPacket set_intf{.request_type = 0x01, .request = 11, .value = 1, .index = 2, .length = 0};
    send_control(*device_handler, op, 1, UsbIpDirection::Out, 0, set_intf);
    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(stub.submits[0].status, 0u);
    EXPECT_EQ(two->last_alt, 1);
    EXPECT_EQ(five->last_alt, -1);
    EXPECT_EQ(device->interfaces[1].current_altsetting, 1);
    EXPECT_EQ(device->interfaces[0].current_altsetting, 0);

    // GET_INTERFACE 发往接口号 2：应答必须来自接口号 2 的 handler
    SetupPacket get_intf{.request_type = 0x81, .request = 10, .value = 0, .index = 2, .length = 1};
    send_control(*device_handler, op, 2, UsbIpDirection::In, 1, get_intf);
    ASSERT_EQ(stub.submits.size(), 2u);
    EXPECT_EQ(stub.submits[1].status, 0u);
    EXPECT_EQ(ret_submit_data(stub.submits[1]), data_type{0x22});

    // 类请求发往接口号 5（数组下标 0）
    SetupPacket class_out{.request_type = 0x21, .request = 0x09, .value = 0x1234, .index = 5, .length = 0};
    send_control(*device_handler, op, 3, UsbIpDirection::Out, 0, class_out);
    EXPECT_EQ(five->last_class_request, 0x1234u);
    EXPECT_EQ(two->last_class_request, 0u);

    // 不存在的接口号：USB 2.0 §9.4.10 规定 Request Error（STALL）
    SetupPacket missing{.request_type = 0x01, .request = 11, .value = 0, .index = 3, .length = 0};
    send_control(*device_handler, op, 4, UsbIpDirection::Out, 0, missing);
    ASSERT_EQ(stub.submits.size(), 3u);
    EXPECT_EQ(stub.submits[2].status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));

    device_handler->on_disconnection(ec);
}

// ==================== GET_DESCRIPTOR / SYNCH_FRAME ====================

// ==================== SET_DESCRIPTOR 短包 ====================

namespace {

/// 记录 SET_DESCRIPTOR 收到的数据，用于检查截取长度
class SetDescriptorProbe : public SimpleVirtualDeviceHandler {
public:
    using SimpleVirtualDeviceHandler::SimpleVirtualDeviceHandler;

    void request_set_descriptor(std::uint8_t desc_type, std::uint8_t desc_index, std::uint16_t language_id,
                                std::uint16_t descriptor_length, const data_type &descriptor,
                                std::uint32_t *p_status) override {
        (void) desc_type;
        (void) desc_index;
        (void) language_id;
        (void) descriptor_length;
        seen = descriptor;
        *p_status = 0;
    }

    data_type seen;
};

} // namespace

TEST(VirtualDeviceStandard, SetDescriptorTruncatesToReceivedBytes) {
    // 主机声明 wLength=16 但实际只收到 4 字节（控制 OUT 短包）：不能按声明
    // 长度越界构造数据，传给 request_set_descriptor 的只能是从实际数据里截的
    StringPool pool;
    auto device = UsbDevice::make("1-1", 0x1234, 0x5678, {}, 1, 1, 0, {}, UsbSpeed::Full);
    auto device_handler = device->with_handler<SetDescriptorProbe>(pool);
    GenericTransferOperator op;
    CaptureResponder stub;
    error_code ec;
    device_handler->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    SetupPacket set{.request_type = 0x00, .request = 7, .value = 0x0100, .index = 0, .length = 16};
    auto cmd = make_cmd_submit(op, 1, 0x00, UsbIpDirection::Out, 16, set, data_type(16, 0xAA));
    GenericTransfer::from_handle(cmd.transfer.get())->data.resize(4); // 模拟短包
    device_handler->receive_urb(std::move(cmd), UsbEndpoint::get_ep0_out(UsbSpeed::Full), std::nullopt, ec);
    ASSERT_FALSE(ec);

    EXPECT_EQ(device_handler->seen.size(), 4u);
    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(stub.submits[0].status, 0u);

    device_handler->on_disconnection(ec);
}

TEST(VirtualDeviceStandard, UnknownDescriptorTypeStalls) {
    // USB 2.0 §9.4.3：设备不支持请求的描述符时回 Request Error
    StringPool pool;
    auto device = UsbDevice::make("1-1", 0x1234, 0x5678, {}, 1, 1, 0, {}, UsbSpeed::Full);
    auto device_handler = device->with_handler<SimpleVirtualDeviceHandler>(pool);
    GenericTransferOperator op;
    CaptureResponder stub;
    error_code ec;
    device_handler->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    SetupPacket get{.request_type = 0x80, .request = 6, .value = 0x9900, .index = 0, .length = 8};
    send_control(*device_handler, op, 1, UsbIpDirection::In, 8, get);

    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(stub.submits[0].status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
    EXPECT_EQ(stub.submits[0].actual_length, 0u);
}

namespace {

/// 返回固定帧号的 SYNCH_FRAME 探测 handler
class SynchProbeHandler : public VirtualInterfaceHandler {
public:
    using VirtualInterfaceHandler::VirtualInterfaceHandler;

    std::uint16_t request_endpoint_sync_frame(std::uint8_t ep_address, std::uint32_t *p_status) override {
        (void) ep_address;
        *p_status = 0;
        return 0x1234;
    }
};

} // namespace

TEST(VirtualDeviceStandard, SynchFrameReturnsFrameNumber) {
    // USB 2.0 §9.4.11：数据阶段返回 2 字节帧号（小端）
    StringPool pool;
    UsbInterface intf{};
    intf.endpoints = {{{.address = 0x81,
                        .attributes = static_cast<std::uint8_t>(EndpointAttributes::Isochronous),
                        .max_packet_size = 64,
                        .interval = 1}}};
    auto device = UsbDevice::make("1-1", 0x1234, 0x5678, {intf}, 1, 1, 0, {}, UsbSpeed::High);
    device->interfaces[0].with_handler<SynchProbeHandler>(pool);
    auto device_handler = device->with_handler<SimpleVirtualDeviceHandler>(pool);
    device_handler->setup_interface_handlers();

    GenericTransferOperator op;
    CaptureResponder stub;
    error_code ec;
    device_handler->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    SetupPacket sync{.request_type = 0x82, .request = 12, .value = 0, .index = 0x81, .length = 2};
    send_control(*device_handler, op, 1, UsbIpDirection::In, 2, sync);

    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(stub.submits[0].status, 0u);
    EXPECT_EQ(stub.submits[0].actual_length, 2u);
    EXPECT_EQ(ret_submit_data(stub.submits[0]), (data_type{0x34, 0x12}));
}

TEST(VirtualDeviceStandard, OtherSpeedConfigurationUsesFullSpeedPacketSizes) {
    // USB 2.0 §9.6.4 / Table 9-11：other_speed_configuration 描述设备在另一
    // 速度下的配置，端点包长必须是那个速度的值（高速设备的另一速度 = 全速：
    // bulk/interrupt 最大 64，ISO 最大 1023，且没有每微帧多事务编码）
    StringPool pool;
    UsbInterface intf{};
    intf.endpoints = {{
            {.address = 0x81,
             .attributes = static_cast<std::uint8_t>(EndpointAttributes::Bulk),
             .max_packet_size = 512,
             .interval = 0},
            {.address = 0x82,
             .attributes = static_cast<std::uint8_t>(EndpointAttributes::Interrupt),
             .max_packet_size = 512,
             .interval = 10},
            {.address = 0x83,
             .attributes = static_cast<std::uint8_t>(EndpointAttributes::Isochronous),
             .max_packet_size = 1024,
             .interval = 1},
    }};
    auto device = UsbDevice::make("1-1", 0x1234, 0x5678, {intf}, 1, 1, 0, {}, UsbSpeed::High);
    auto handler = device->with_handler<ProbeDevice>(pool);

    std::uint32_t status = 0;
    auto desc = handler->get_other_speed_descriptor(0, 1024, &status);
    ASSERT_EQ(status, 0u);
    ASSERT_GE(desc.size(), 2u);
    EXPECT_EQ(desc[1], static_cast<std::uint8_t>(DescriptorType::OtherSpeedConfiguration));

    std::vector<std::uint16_t> sizes;
    for (std::size_t pos = 0; pos + 2 <= desc.size();) {
        auto length = desc[pos];
        ASSERT_GE(length, 2);
        ASSERT_LE(pos + length, desc.size());
        if (desc[pos + 1] == static_cast<std::uint8_t>(DescriptorType::Endpoint)) {
            ASSERT_EQ(length, sizeof(EndpointDesc));
            auto *ep = reinterpret_cast<const EndpointDesc *>(&desc[pos]);
            sizes.push_back(htole(ep->wMaxPacketSize));
        }
        pos += length;
    }
    ASSERT_EQ(sizes.size(), 3u);
    EXPECT_EQ(sizes[0], 64u);   // Bulk 512 → 64
    EXPECT_EQ(sizes[1], 64u);   // Interrupt 512 → 64
    EXPECT_EQ(sizes[2], 1023u); // ISO 1024 → 1023
}

TEST(VirtualDeviceStandard, LongStringDescriptorTruncatesTo255Bytes) {
    // bLength 是单字节（USB 2.0 §9.5）：2 字节头 + 最多 126 个 UTF-16 字符 = 254
    DeviceEnv env(UsbSpeed::High);
    auto index = env.pool.new_string(std::wstring(200, L'A'));
    ASSERT_NE(index, 0);

    // 成功路径沿用调用方初始化的 0（与其它描述符函数一致，只在失败时改写）
    std::uint32_t status = 0;
    auto desc = env.handler->get_string_descriptor(0x0409, index, 255, &status);
    EXPECT_EQ(status, 0u);
    ASSERT_EQ(desc.size(), 254u);
    EXPECT_EQ(desc[0], 254);
    EXPECT_EQ(desc[1], static_cast<std::uint8_t>(DescriptorType::String));
}

// ==================== SuperSpeed（USB 3.0 §9.6） ====================

namespace {

/// 按 bLength 把描述符链切成 (bDescriptorType, 描述符字节)
std::vector<std::pair<std::uint8_t, data_type>> split_descriptors(const data_type &desc) {
    std::vector<std::pair<std::uint8_t, data_type>> parts;
    for (std::size_t pos = 0; pos + 2 <= desc.size();) {
        auto length = desc[pos];
        // 长度非法就停止，避免越界/死循环
        if (length < 2 || pos + length > desc.size()) {
            break;
        }
        parts.emplace_back(desc[pos + 1], data_type(desc.begin() + pos, desc.begin() + pos + length));
        pos += length;
    }
    return parts;
}

/// 构造一个单接口、端点由参数决定的设备并绑定 ProbeDevice
std::shared_ptr<UsbDevice> make_device_with_endpoints(StringPool &pool, UsbSpeed speed,
                                                      std::vector<UsbEndpoint> endpoints,
                                                      std::unique_ptr<ProbeDevice> &handler) {
    UsbInterface intf{};
    intf.endpoints = {std::move(endpoints)};
    auto device = UsbDevice::make("1-1", 0x1234, 0x5678, {intf}, 1, 1, 0, {}, speed);
    handler = std::make_unique<ProbeDevice>(*device, pool);
    return device;
}

} // namespace

TEST(VirtualDeviceStandard, SuperSpeedDeviceDescriptorReportsBcdUsb30) {
    // USB 3.0 §9.6.1：SuperSpeed 能力设备的设备描述符 bcdUSB 必须是 0300H；
    // 并且 §9.2.6.6 要求 >= 0210H 的设备支持 GET_DESCRIPTOR(BOS)
    DeviceEnv env(UsbSpeed::Super, {2, 0, 0});
    std::uint32_t status = 0;
    auto desc = env.handler->get_device_descriptor(0, 18, &status);
    ASSERT_EQ(desc.size(), 18u);
    EXPECT_EQ(desc[2] | (desc[3] << 8), 0x0300);

    auto bos = env.handler->get_bos_descriptor(0, 64, &status);
    EXPECT_EQ(status, 0u);
    EXPECT_FALSE(bos.empty());
}

TEST(VirtualDeviceStandard, BcdDeviceIsFullBcdValue) {
    // bcdDevice 是完整 BCD（USB 2.0 Table 9-8），不是只拼 major/minor 两字节：
    // 2.3.4 → 0x0234，小端线上字节 34 02
    StringPool pool;
    auto device = UsbDevice::make("1-1", 0x1234, 0x5678, {}, 1, 1, 0, {}, UsbSpeed::Full, Version{2, 3, 4});
    auto handler = std::make_unique<ProbeDevice>(*device, pool);
    std::uint32_t status = 0;
    auto desc = handler->get_device_descriptor(0, 18, &status);
    ASSERT_EQ(desc.size(), 18u);
    EXPECT_EQ(desc[12], 0x34);
    EXPECT_EQ(desc[13], 0x02);
}

TEST(VirtualDeviceStandard, SuperSpeedBosUsb20ExtensionSetsLpmBit) {
    // USB 3.0 §9.6.2.1：SuperSpeed 设备的 USB 2.0 Extension 描述符
    // bmAttributes bit1（LPM）必须置 1
    DeviceEnv env(UsbSpeed::Super, {3, 0, 0});
    std::uint32_t status = 0;
    auto desc = env.handler->get_bos_descriptor(0, 64, &status);
    ASSERT_EQ(desc.size(), 22u);
    // USB 2.0 Extension 描述符从偏移 5 开始，bmAttributes 在偏移 8..11（小端）
    EXPECT_EQ(desc[8], 0x02);
    EXPECT_EQ(desc[9], 0x00);
    EXPECT_EQ(desc[10], 0x00);
    EXPECT_EQ(desc[11], 0x00);
}

TEST(VirtualDeviceStandard, SuperSpeedConfigurationInsertsBulkCompanion) {
    // USB 3.0 §9.6.5/§9.6.7：每个非 EP0 端点描述符后紧跟 Endpoint Companion，
    // §9.6.6：SuperSpeed 下 Bulk 的 wMaxPacketSize 固定 1024；
    // §9.6.3：bMaxPower 以 8mA 为单位（500mA → 63）
    StringPool pool;
    std::unique_ptr<ProbeDevice> handler;
    auto device = make_device_with_endpoints(
            pool, UsbSpeed::Super,
            {
                    {.address = 0x81,
                     .attributes = static_cast<std::uint8_t>(EndpointAttributes::Bulk),
                     .max_packet_size = 512,
                     .interval = 0,
                     .max_burst = 3},
                    {.address = 0x02,
                     .attributes = static_cast<std::uint8_t>(EndpointAttributes::Bulk),
                     .max_packet_size = 512,
                     .interval = 0},
            },
            handler);
    (void) device;

    std::uint32_t status = 0xFFFFFFFF; // 成功的描述符函数不改写状态
    auto desc = handler->get_configuration_descriptor(0, 4096, &status);
    ASSERT_GE(desc.size(), 9u);
    EXPECT_EQ(desc[8], 63); // 500mA / 8mA，向上取整
    EXPECT_EQ(desc[2] | (desc[3] << 8), desc.size());
    EXPECT_EQ(desc.size(), 9u + 9u + 2u * (7u + 6u));

    auto parts = split_descriptors(desc);
    ASSERT_EQ(parts.size(), 6u);
    EXPECT_EQ(parts[0].first, static_cast<std::uint8_t>(DescriptorType::Configuration));
    EXPECT_EQ(parts[1].first, static_cast<std::uint8_t>(DescriptorType::Interface));

    EXPECT_EQ(parts[2].first, static_cast<std::uint8_t>(DescriptorType::Endpoint));
    ASSERT_EQ(parts[2].second.size(), 7u);
    EXPECT_EQ(parts[2].second[4] | (parts[2].second[5] << 8), 1024);
    EXPECT_EQ(parts[3].first, static_cast<std::uint8_t>(DescriptorType::SsEndpointCompanion));
    EXPECT_EQ(parts[3].second, (data_type{0x06, 0x30, 0x03, 0x00, 0x00, 0x00}));

    EXPECT_EQ(parts[4].first, static_cast<std::uint8_t>(DescriptorType::Endpoint));
    ASSERT_EQ(parts[4].second.size(), 7u);
    EXPECT_EQ(parts[4].second[4] | (parts[4].second[5] << 8), 1024);
}

TEST(VirtualDeviceStandard, SuperSpeedInterruptCompanionReservesServiceIntervalBytes) {
    // USB 3.0 §9.6.6：Interrupt 端点 bMaxBurst 大于 0 时 wMaxPacketSize 固定 1024；
    // §9.6.7 Table 9-20：wBytesPerInterval 是每个服务间隔的总字节数，
    // 即 (bMaxBurst+1) * wMaxPacketSize；bMaxBurst 为 0 时就是 wMaxPacketSize
    StringPool pool;
    std::unique_ptr<ProbeDevice> handler;
    auto device = make_device_with_endpoints(
            pool, UsbSpeed::Super,
            {
                    {.address = 0x81,
                     .attributes = static_cast<std::uint8_t>(EndpointAttributes::Interrupt),
                     .max_packet_size = 16,
                     .interval = 1,
                     .max_burst = 1},
                    {.address = 0x82,
                     .attributes = static_cast<std::uint8_t>(EndpointAttributes::Interrupt),
                     .max_packet_size = 16,
                     .interval = 1},
            },
            handler);
    (void) device;

    std::uint32_t status = 0;
    auto desc = handler->get_configuration_descriptor(0, 4096, &status);
    ASSERT_EQ(status, 0u);
    auto parts = split_descriptors(desc);
    ASSERT_EQ(parts.size(), 6u); // 配置 + 接口 + (端点+伴随) ×2
    EXPECT_EQ(parts[2].second[4] | (parts[2].second[5] << 8), 1024);
    EXPECT_EQ(parts[3].second, (data_type{0x06, 0x30, 0x01, 0x00, 0x00, 0x08}));

    // 第二个端点：bMaxBurst=0 → 沿用声明的 16 字节包，wBytesPerInterval 16
    EXPECT_EQ(parts[4].second[4] | (parts[4].second[5] << 8), 16);
    EXPECT_EQ(parts[5].second, (data_type{0x06, 0x30, 0x00, 0x00, 0x10, 0x00}));
}

TEST(VirtualDeviceStandard, HighSpeedConfigurationHasNoCompanion) {
    // SuperSpeed Endpoint Companion 只出现在 SuperSpeed 配置里
    StringPool pool;
    std::unique_ptr<ProbeDevice> handler;
    auto device = make_device_with_endpoints(
            pool, UsbSpeed::High,
            {{.address = 0x81,
              .attributes = static_cast<std::uint8_t>(EndpointAttributes::Bulk),
              .max_packet_size = 512,
              .interval = 0}},
            handler);
    (void) device;

    std::uint32_t status = 0;
    auto desc = handler->get_configuration_descriptor(0, 4096, &status);
    EXPECT_EQ(desc[8], 0xFA); // 500mA / 2mA，与 USB 2.0 的 2mA 单位一致
    auto parts = split_descriptors(desc);
    ASSERT_EQ(parts.size(), 3u); // 配置 + 接口 + 端点，没有伴随描述符
    EXPECT_EQ(parts[2].first, static_cast<std::uint8_t>(DescriptorType::Endpoint));
    EXPECT_EQ(parts[2].second[4] | (parts[2].second[5] << 8), 512);
}

TEST(VirtualDeviceStandard, SuperSpeedHasNoDeviceQualifierOrOtherSpeed) {
    // USB 3.0 §9.4.3：SuperSpeed 模式下的设备通过 BOS 报告其他速度，
    // 不支持 device_qualifier 和 other_speed_configuration
    DeviceEnv env(UsbSpeed::Super, {3, 0, 0});
    std::uint32_t status = 0;
    auto qualifier = env.handler->get_device_qualifier_descriptor(0, 64, &status);
    EXPECT_EQ(status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
    EXPECT_TRUE(qualifier.empty());

    status = 0;
    auto other = env.handler->get_other_speed_descriptor(0, 64, &status);
    EXPECT_EQ(status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
    EXPECT_TRUE(other.empty());
}

TEST(VirtualDeviceStandard, SetSelStoresExitLatencies) {
    // USB 3.0 §9.4.12：wValue/wIndex 为 0、wLength 为 6；
    // 数据阶段 U1SEL/U1PEL 各 1 字节、U2SEL/U2PEL 各 2 字节（小端）
    StringPool pool;
    auto device = UsbDevice::make("1-1", 0x1234, 0x5678, {}, 1, 1, 0, {}, UsbSpeed::Super, Version{3, 0, 0});
    auto handler = device->with_handler<ProbeDevice>(pool);
    GenericTransferOperator op;
    CaptureResponder stub;
    error_code ec;
    handler->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    SetupPacket sel{
            .request_type = 0x00, .request = 0x30, .value = 0, .index = 0, .length = 6};
    auto cmd = make_cmd_submit(op, 1, 0x00, UsbIpDirection::Out, 6, sel,
                               data_type{0x0A, 0x14, 0x34, 0x12, 0x78, 0x56});
    handler->receive_urb(std::move(cmd), UsbEndpoint::get_ep0_out(UsbSpeed::Super), std::nullopt, ec);
    ASSERT_FALSE(ec);

    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(stub.submits[0].status, 0u);
    EXPECT_EQ(handler->get_u1_system_exit_latency(), 0x0A);
    EXPECT_EQ(handler->get_u1_device_to_host_latency(), 0x14);
    EXPECT_EQ(handler->get_u2_system_exit_latency(), 0x1234);
    EXPECT_EQ(handler->get_u2_device_to_host_latency(), 0x5678);

    handler->on_disconnection(ec);
}

TEST(VirtualDeviceStandard, SetSelAndIsochDelayRejectMalformedSetup) {
    // USB 3.0 §9.4.11/§9.4.12 规定各 Setup 字段的合法值；非法请求按
    // Request Error 处理，不能当作成功
    StringPool pool;
    auto device = UsbDevice::make("1-1", 0x1234, 0x5678, {}, 1, 1, 0, {}, UsbSpeed::Super, Version{3, 0, 0});
    auto handler = device->with_handler<ProbeDevice>(pool);
    GenericTransferOperator op;
    CaptureResponder stub;
    error_code ec;
    handler->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    const auto ep0 = UsbEndpoint::get_ep0_out(UsbSpeed::Super);
    const auto set_sel = static_cast<std::uint8_t>(StandardRequest::SetSel);
    const auto set_isoch = static_cast<std::uint8_t>(StandardRequest::SetIsochDelay);

    // SET_SEL 的 wLength 必须是 6
    SetupPacket sel_len{
            .request_type = 0x00, .request = set_sel, .value = 0, .index = 0, .length = 8};
    auto sel_len_cmd = make_cmd_submit(op, 1, 0, UsbIpDirection::Out, 8, sel_len, data_type(8, 0));
    handler->receive_urb(std::move(sel_len_cmd), ep0, std::nullopt, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(stub.submits[0].status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));

    // SET_SEL 的 wValue 必须是 0
    SetupPacket sel_value{
            .request_type = 0x00, .request = set_sel, .value = 1, .index = 0, .length = 6};
    auto sel_value_cmd = make_cmd_submit(op, 2, 0, UsbIpDirection::Out, 6, sel_value, data_type(6, 0));
    handler->receive_urb(std::move(sel_value_cmd), ep0, std::nullopt, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 2u);
    EXPECT_EQ(stub.submits[1].status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));

    // SET_ISOCH_DELAY 的 wIndex 必须是 0
    SetupPacket isoch_index{
            .request_type = 0x00, .request = set_isoch, .value = 100, .index = 1, .length = 0};
    auto isoch_index_cmd = make_cmd_submit(op, 3, 0, UsbIpDirection::Out, 0, isoch_index);
    handler->receive_urb(std::move(isoch_index_cmd), ep0, std::nullopt, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 3u);
    EXPECT_EQ(stub.submits[2].status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));

    // SET_ISOCH_DELAY 的 wLength 必须是 0
    SetupPacket isoch_len{
            .request_type = 0x00, .request = set_isoch, .value = 100, .index = 0, .length = 2};
    auto isoch_len_cmd = make_cmd_submit(op, 4, 0, UsbIpDirection::Out, 2, isoch_len, data_type{0x00, 0x00});
    handler->receive_urb(std::move(isoch_len_cmd), ep0, std::nullopt, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 4u);
    EXPECT_EQ(stub.submits[3].status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));

    handler->on_disconnection(ec);
}

TEST(VirtualDeviceStandard, SetSelRejectsInvalidParameters) {    // USB 3.0 §9.4.12：wValue/wIndex 必须为 0、wLength 必须为 6；
    // 该请求只对 SuperSpeed 设备有效
    DeviceEnv ss(UsbSpeed::Super, {3, 0, 0});
    std::uint32_t status = 0;
    ss.handler->request_set_sel(data_type(6, 0), 1, 0, &status);
    EXPECT_EQ(status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));

    status = 0;
    ss.handler->request_set_sel(data_type(6, 0), 0, 2, &status);
    EXPECT_EQ(status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));

    status = 0;
    ss.handler->request_set_sel(data_type(5, 0), 0, 0, &status);
    EXPECT_EQ(status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));

    DeviceEnv hs(UsbSpeed::High, {2, 0, 0});
    status = 0;
    hs.handler->request_set_sel(data_type(6, 0), 0, 0, &status);
    EXPECT_EQ(status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
}

TEST(VirtualDeviceStandard, SetIsochDelayStoresValue) {
    // USB 3.0 §9.4.11：wValue 是 0..65535 ns 的单向时延
    DeviceEnv ss(UsbSpeed::Super, {3, 0, 0});
    std::uint32_t status = 0;
    ss.handler->request_set_isoch_delay(1200, &status);
    EXPECT_EQ(status, 0u);
    EXPECT_EQ(ss.handler->get_isoch_delay_ns(), 1200);

    DeviceEnv hs(UsbSpeed::High, {2, 0, 0});
    status = 0;
    hs.handler->request_set_isoch_delay(1200, &status);
    EXPECT_EQ(status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
}

TEST(VirtualDeviceStandard, U1U2EnableOnlyInConfiguredSuperSpeedState) {
    // USB 3.0 §9.4.9：U1_Enable/U2_Enable 只在 Configured 状态处理；
    // GET_STATUS(Device) 的 bit2/bit3 反映启用状态（§9.4.5 Figure 9-5）
    DeviceEnv ss(UsbSpeed::Super, {3, 0, 0});
    GenericTransferOperator op;
    CaptureResponder stub;
    error_code ec;
    ss.handler->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    // Address 状态（未配置）收到 U1_Enable 必须回 Request Error
    std::uint32_t status = 0;
    ss.handler->request_set_feature(SetupPacket::USB_DEVICE_U1_ENABLE, &status);
    EXPECT_EQ(status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));

    // 进入 Configured 状态
    SetupPacket set_cfg{.request_type = 0x00, .request = 9, .value = 1, .index = 0, .length = 0};
    send_control(*ss.handler, op, 1, UsbIpDirection::Out, 0, set_cfg);
    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(stub.submits[0].status, 0u);

    status = 0;
    ss.handler->request_set_feature(SetupPacket::USB_DEVICE_U1_ENABLE, &status);
    EXPECT_EQ(status, 0u);
    status = 0;
    ss.handler->request_set_feature(SetupPacket::USB_DEVICE_U2_ENABLE, &status);
    EXPECT_EQ(status, 0u);
    status = 0;
    EXPECT_EQ(ss.handler->request_get_status(&status), 0x000C);
    EXPECT_EQ(status, 0u);

    status = 0;
    ss.handler->request_clear_feature(SetupPacket::USB_DEVICE_U1_ENABLE, &status);
    EXPECT_EQ(status, 0u);
    status = 0;
    EXPECT_EQ(ss.handler->request_get_status(&status), 0x0008);

    // LTM 没有在 BOS 里声明能力，SET_FEATURE(LTM_ENABLE) 必须 STALL
    status = 0;
    ss.handler->request_set_feature(SetupPacket::USB_DEVICE_LTM_ENABLE, &status);
    EXPECT_EQ(status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));

    // USB 2.0 设备不认这三个特性选择子
    DeviceEnv hs(UsbSpeed::High, {2, 0, 0});
    status = 0;
    hs.handler->request_set_feature(SetupPacket::USB_DEVICE_U1_ENABLE, &status);
    EXPECT_EQ(status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));

    ss.handler->on_disconnection(ec);
}

TEST(VirtualDeviceStandard, SetConfigurationZeroClearsU1U2Enable) {
    // USB 3.0 §9.4.9：U1/U2 Enable 只在 Configured 状态有效；SET_CONFIGURATION(0)
    // 回到 Address 状态后必须清零，否则主机在 Address 状态无法用 CLEAR_FEATURE
    // 清除（会回 Request Error），状态一直卡住
    DeviceEnv ss(UsbSpeed::Super, {3, 0, 0});
    GenericTransferOperator op;
    CaptureResponder stub;
    error_code ec;
    ss.handler->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    SetupPacket set_cfg{.request_type = 0x00, .request = 9, .value = 1, .index = 0, .length = 0};
    send_control(*ss.handler, op, 1, UsbIpDirection::Out, 0, set_cfg);
    ASSERT_EQ(stub.submits.size(), 1u);
    ASSERT_EQ(stub.submits[0].status, 0u);

    std::uint32_t status = 0;
    ss.handler->request_set_feature(SetupPacket::USB_DEVICE_U1_ENABLE, &status);
    ASSERT_EQ(status, 0u);
    ss.handler->request_set_feature(SetupPacket::USB_DEVICE_U2_ENABLE, &status);
    ASSERT_EQ(status, 0u);
    status = 0;
    EXPECT_EQ(ss.handler->request_get_status(&status), 0x000C);

    SetupPacket unset_cfg{.request_type = 0x00, .request = 9, .value = 0, .index = 0, .length = 0};
    send_control(*ss.handler, op, 2, UsbIpDirection::Out, 0, unset_cfg);
    ASSERT_EQ(stub.submits.size(), 2u);
    ASSERT_EQ(stub.submits[1].status, 0u);

    status = 0;
    EXPECT_EQ(ss.handler->request_get_status(&status), 0u);

    ss.handler->on_disconnection(ec);
}

TEST(VirtualDeviceStandard, WirelessEp0UsesHighSpeedPacketSize) {
    // Wireless USB 1.0 与高速同为 480Mbps，EP0 包大小一致；
    // change_device_ep0_max_size_by_speed 也把 Wireless 按 64 处理
    EXPECT_EQ(ep0_max_packet_size(UsbSpeed::Wireless), 64);
}

TEST(VirtualDeviceStandard, ReservedSetupFieldsFollowKernelBehavior) {
    // 与内核 gadget（gadget/composite.c）保持一致：
    // SET_CONFIGURATION/SET_INTERFACE 用完整 wValue 匹配配置/alt，高字节非 0
    // 天然会失败（STALL）；GET_STATUS 的 wValue 和端点 wIndex 高字节被忽略
    StringPool pool;
    UsbInterface intf{};
    intf.endpoints = {{{.address = 0x81,
                        .attributes = static_cast<std::uint8_t>(EndpointAttributes::Bulk),
                        .max_packet_size = 64,
                        .interval = 0}}};
    auto device = UsbDevice::make("1-1", 0x1234, 0x5678, {intf}, 1, 1, 0, {}, UsbSpeed::Full);
    device->interfaces[0].with_handler<ProbeInterface>(pool);
    auto handler = device->with_handler<ProbeDevice>(pool);
    handler->setup_interface_handlers();

    GenericTransferOperator op;
    CaptureResponder stub;
    error_code ec;
    handler->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    // SET_CONFIGURATION 的 wValue 高字节非 0：匹配不到配置，回 EPIPE
    SetupPacket set_cfg{.request_type = 0x00, .request = 9, .value = 0x0101, .index = 0, .length = 0};
    send_control(*handler, op, 1, UsbIpDirection::Out, 0, set_cfg);
    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(stub.submits[0].status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));

    // SET_INTERFACE 的 wValue 高字节非 0：alt 越界，回 EPIPE
    SetupPacket set_intf{.request_type = 0x01, .request = 11, .value = 0x0101, .index = 0, .length = 0};
    send_control(*handler, op, 2, UsbIpDirection::Out, 0, set_intf);
    ASSERT_EQ(stub.submits.size(), 2u);
    EXPECT_EQ(stub.submits[1].status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));

    // GET_STATUS 的 wValue 非 0：与内核一样忽略，正常返回 2 字节状态
    SetupPacket get_status{.request_type = 0x80, .request = 0, .value = 1, .index = 0, .length = 2};
    send_control(*handler, op, 3, UsbIpDirection::In, 2, get_status);
    ASSERT_EQ(stub.submits.size(), 3u);
    EXPECT_EQ(stub.submits[2].status, 0u);
    EXPECT_EQ(stub.submits[2].actual_length, 2u);

    // 端点请求的 wIndex 高字节非 0：与内核一样忽略高字节，按 0x81 处理
    SetupPacket ep_status{.request_type = 0x82, .request = 0, .value = 0, .index = 0x0181, .length = 2};
    send_control(*handler, op, 4, UsbIpDirection::In, 2, ep_status);
    ASSERT_EQ(stub.submits.size(), 4u);
    EXPECT_EQ(stub.submits[3].status, 0u);
    EXPECT_EQ(stub.submits[3].actual_length, 2u);

    // 非法配置值没有生效
    std::uint32_t status = 0;
    EXPECT_EQ(handler->request_get_configuration(&status), 0u);
    EXPECT_EQ(status, 0u);

    handler->on_disconnection(ec);
}

namespace {

/// GET_INTERFACE 返回失败的接口 handler（验证错误状态不携带数据）
class FailingGetInterfaceHandler : public VirtualInterfaceHandler {
public:
    using VirtualInterfaceHandler::VirtualInterfaceHandler;

    std::uint8_t request_get_interface(std::uint32_t *p_status) override {
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
        return 0xFF;
    }
};

/// 模拟违约的派生类：返回非空描述符的同时报 EPIPE
class FailingDescriptorDevice : public SimpleVirtualDeviceHandler {
public:
    using SimpleVirtualDeviceHandler::SimpleVirtualDeviceHandler;

protected:
    data_type get_custom_descriptor(std::uint8_t, std::uint8_t, std::uint16_t,
                                    std::uint32_t *p_status) override {
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
        return {0x01, 0x02, 0x03};
    }
};

/// 同上，接口级
class FailingDescriptorInterface : public VirtualInterfaceHandler {
public:
    using VirtualInterfaceHandler::VirtualInterfaceHandler;

    data_type request_get_descriptor(std::uint8_t, std::uint8_t, std::uint16_t,
                                     std::uint32_t *p_status) override {
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
        return {0x01, 0x02, 0x03};
    }
};

} // namespace

TEST(VirtualDeviceStandard, DeviceGetDescriptorErrorCarriesNoData) {
    // 子类返回非空描述符并置 EPIPE 时，RET_SUBMIT 不能带数据阶段
    // （USB 2.0 §9.4.3：不支持的描述符回 Request Error，无数据）
    StringPool pool;
    auto device = UsbDevice::make("1-1", 0x1234, 0x5678, {}, 1, 1, 0, {}, UsbSpeed::Full);
    auto handler = device->with_handler<FailingDescriptorDevice>(pool);
    GenericTransferOperator op;
    CaptureResponder stub;
    error_code ec;
    handler->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    SetupPacket get_desc{.request_type = 0x80, .request = 6, .value = 0x0F00, .index = 0, .length = 8};
    send_control(*handler, op, 1, UsbIpDirection::In, 8, get_desc);

    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(stub.submits[0].status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
    EXPECT_EQ(stub.submits[0].actual_length, 0u);
    EXPECT_TRUE(ret_submit_data(stub.submits[0]).empty());

    handler->on_disconnection(ec);
}

TEST(VirtualDeviceStandard, InterfaceGetDescriptorErrorCarriesNoData) {
    // 接口级描述符请求同样不能在 EPIPE 时带数据
    StringPool pool;
    UsbInterface intf{};
    auto device = UsbDevice::make("1-1", 0x1234, 0x5678, {intf}, 1, 1, 0, {}, UsbSpeed::Full);
    device->interfaces[0].with_handler<FailingDescriptorInterface>(pool);
    auto handler = device->with_handler<SimpleVirtualDeviceHandler>(pool);
    handler->setup_interface_handlers();

    GenericTransferOperator op;
    CaptureResponder stub;
    error_code ec;
    handler->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    SetupPacket get_desc{.request_type = 0x81, .request = 6, .value = 0x2200, .index = 0, .length = 8};
    send_control(*handler, op, 1, UsbIpDirection::In, 8, get_desc);

    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(stub.submits[0].status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
    EXPECT_EQ(stub.submits[0].actual_length, 0u);
    EXPECT_TRUE(ret_submit_data(stub.submits[0]).empty());

    handler->on_disconnection(ec);
}

TEST(VirtualDeviceStandard, GetInterfaceErrorCarriesNoData) {
    // 子类 handler 用非零 status 拒绝 GET_INTERFACE 时，RET_SUBMIT 不能带
    // 数据阶段（状态与 actual_length 必须一致）
    StringPool pool;
    UsbInterface intf{};
    auto device = UsbDevice::make("1-1", 0x1234, 0x5678, {intf}, 1, 1, 0, {}, UsbSpeed::Full);
    device->interfaces[0].with_handler<FailingGetInterfaceHandler>(pool);
    auto handler = device->with_handler<SimpleVirtualDeviceHandler>(pool);
    handler->setup_interface_handlers();

    GenericTransferOperator op;
    CaptureResponder stub;
    error_code ec;
    handler->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    SetupPacket get_intf{.request_type = 0x81, .request = 10, .value = 0, .index = 0, .length = 1};
    send_control(*handler, op, 1, UsbIpDirection::In, 1, get_intf);

    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(stub.submits[0].status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
    EXPECT_EQ(stub.submits[0].actual_length, 0u);
    EXPECT_TRUE(ret_submit_data(stub.submits[0]).empty());

    handler->on_disconnection(ec);
}

TEST(VirtualDeviceStandard, ControlRequestToMissingEndpointStalls) {
    // USB 2.0 §9.4.1/§9.4.5：引用不存在的端点必须回 Request Error。
    // 控制传输必须有 RET_SUBMIT，漏掉应答会让主机 URB 永久挂起
    StringPool pool;
    auto device = UsbDevice::make("1-1", 0x1234, 0x5678, {}, 1, 1, 0, {}, UsbSpeed::Full);
    auto handler = device->with_handler<ProbeDevice>(pool);
    GenericTransferOperator op;
    CaptureResponder stub;
    error_code ec;
    handler->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    SetupPacket get_status{.request_type = 0x82, .request = 0, .value = 0, .index = 0x85, .length = 2};
    send_control(*handler, op, 1, UsbIpDirection::In, 2, get_status);

    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(stub.submits[0].status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));

    handler->on_disconnection(ec);
}

TEST(VirtualDeviceStandard, NewConnectionResetsDeviceRuntimeState) {
    // 新连接等价于总线复位（USB 2.0 §9.2.1）；§9.1.1.6 要求复位后远程唤醒
    // 禁用，U1/U2 Enable、SET_SEL、SET_ISOCH_DELAY 保存的值也不能残留
    DeviceEnv ss(UsbSpeed::Super, {3, 0, 0});
    GenericTransferOperator op;
    CaptureResponder stub;
    error_code ec;
    ss.handler->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    SetupPacket set_cfg{.request_type = 0x00, .request = 9, .value = 1, .index = 0, .length = 0};
    send_control(*ss.handler, op, 1, UsbIpDirection::Out, 0, set_cfg);
    ASSERT_EQ(stub.submits.size(), 1u);
    ASSERT_EQ(stub.submits[0].status, 0u);

    std::uint32_t status = 0;
    ss.handler->request_set_feature(SetupPacket::USB_DEVICE_U1_ENABLE, &status);
    ASSERT_EQ(status, 0u);
    ss.handler->request_set_feature(SetupPacket::USB_DEVICE_U2_ENABLE, &status);
    ASSERT_EQ(status, 0u);
    ss.handler->request_set_sel(data_type{1, 2, 3, 0, 4, 0}, 0, 0, &status);
    ASSERT_EQ(status, 0u);
    ss.handler->request_set_isoch_delay(321, &status);
    ASSERT_EQ(status, 0u);
    status = 0;
    EXPECT_EQ(ss.handler->request_get_status(&status), 0x000C);

    // 断开后重新连接：所有运行时状态清零
    ss.handler->on_disconnection(ec);
    ss.handler->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    status = 0;
    EXPECT_EQ(ss.handler->request_get_status(&status), 0u);
    EXPECT_EQ(status, 0u);
    EXPECT_EQ(ss.handler->get_u1_system_exit_latency(), 0);
    EXPECT_EQ(ss.handler->get_u1_device_to_host_latency(), 0);
    EXPECT_EQ(ss.handler->get_u2_system_exit_latency(), 0);
    EXPECT_EQ(ss.handler->get_u2_device_to_host_latency(), 0);
    EXPECT_EQ(ss.handler->get_isoch_delay_ns(), 0);

    ss.handler->on_disconnection(ec);
}
