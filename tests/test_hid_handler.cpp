#include <gtest/gtest.h>

#include <array>
#include <cstdint>

#include "test_utils.h"

#include "usbipdcpp/Interface.h"
#include "usbipdcpp/utils/StringPool.h"
#include "usbipdcpp/virtual_device/HidVirtualInterfaceHandler.h"

using namespace usbipdcpp;
using namespace usbipdcpp::test;

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

    // 记录 SET_REPORT 收到的输出报告（键盘 LED 等场景）
    void request_set_report(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                            const data_type &data, std::uint32_t *p_status) override {
        last_output_report = data;
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusOK);
    }

    data_type last_output_report;

    // 测试用公开包装，转发到 protected 的 has_pending_input_reports()
    bool has_pending_reports_for_test() const {
        return has_pending_input_reports();
    }

    // 测试用：模拟已建立连接（通道激活，disconnected 复位）。真实场景设备
    // 连接后才产生报告；未连接时通道断连检查会拒绝入队（push 返回 false）
    void connect_for_test() {
        input_channel.on_new_connection();
    }
};

// 构造一个最小可用的 UsbInterface 与 StringPool
struct HidTestEnv {
    StringPool pool;
    // 传输分配器：必须声明在 stub 之前（成员逆序析构，op 后死）——
    // submits 里的 RetSubmit 持 TransferHandle，析构时要用 op 释放，
    // op 活得比 stub 久才不会有 use-after-scope
    GenericTransferOperator op;
    CaptureResponder stub;
    UsbInterface intf{.interface_class = 3, .interface_subclass = 0, .interface_protocol = 0};
    TestHidHandler handler{intf, pool};
    usbipdcpp::error_code ec;

    // 连上 stub：通道应答/数据面测试走这里（描述符类测试不受影响）
    HidTestEnv() : handler(intf, pool) {
        handler.on_new_connection(stub, ec);
    }
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

// ========== HID 类请求默认实现（形式响应对齐内核 f_hid.c，不 stall） ==========

TEST(VirtualDeviceHandlers, HidIdleFormalResponse) {
    HidTestEnv env;

    // GET_IDLE 返回初始 idle 值 0；SET_IDLE 无条件接受
    std::uint32_t status = 0;
    auto ret = env.handler.request_get_idle(0, 0, 1, &status);
    EXPECT_EQ(status, static_cast<std::uint32_t>(UrbStatusType::StatusOK));
    EXPECT_EQ(ret, data_type({0x00}));

    status = 0;
    env.handler.request_set_idle(0, &status);
    EXPECT_EQ(status, static_cast<std::uint32_t>(UrbStatusType::StatusOK));
}

TEST(VirtualDeviceHandlers, HidReportFormalResponse) {
    HidTestEnv env;

    // GET_REPORT 返回全 0 空报告（长度按主机 wLength）；SET_REPORT 接受并丢弃
    std::uint32_t status = 0;
    auto ret = env.handler.request_get_report(1, 0, 8, &status);
    EXPECT_EQ(status, static_cast<std::uint32_t>(UrbStatusType::StatusOK));
    EXPECT_EQ(ret, data_type(8, 0));

    status = 0;
    env.handler.request_set_report(1, 0, 8, {}, &status);
    EXPECT_EQ(status, static_cast<std::uint32_t>(UrbStatusType::StatusOK));
}

TEST(VirtualDeviceHandlers, HidProtocolFormalResponse) {
    HidTestEnv env;

    // GET_PROTOCOL 返回初始 protocol 值 0
    std::uint32_t status = 0;
    auto ret = env.handler.request_get_protocol(&status);
    EXPECT_EQ(status, static_cast<std::uint32_t>(UrbStatusType::StatusOK));
    EXPECT_EQ(ret, 0u);

    // 测试环境接口 subclass=0（非 Boot），SET_PROTOCOL 应 stall（对齐内核）
    status = 0;
    env.handler.request_set_protocol(1, &status);
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
    env.handler.connect_for_test(); // 模拟已连接（未连接时 push 被断连检查拒绝）

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

// ========== HID 数据面（中断 IN 挂起-应答、SET_REPORT 控制路径） ==========

TEST(VirtualDeviceHandlers, HidInterruptInServedBySendInputReport) {
    // 中断 IN 请求先挂起（无报告），send_input_report 推入时直接应答：
    // 报告数据原样返回、status 0（HidTestEnv 已连上 stub responder）
    HidTestEnv env;

    const std::array<std::uint8_t, 4> report = {0x11, 0x22, 0x33, 0x44};
    env.handler.handle_interrupt_transfer(
            1, UsbEndpoint{.address = 0x81, .attributes = 0x03, .max_packet_size = 8, .interval = 10},
            0, report.size(), make_cmd_submit(env.op, 1, 0x01, UsbIpDirection::In, report.size()).transfer, env.ec);
    ASSERT_FALSE(env.ec);
    EXPECT_TRUE(env.stub.submits.empty()); // 无报告：请求挂起未应答

    env.handler.send_input_report(asio::buffer(report));
    ASSERT_EQ(env.stub.submits.size(), 1u);
    EXPECT_EQ(env.stub.submits[0].header.seqnum, 1u);
    EXPECT_EQ(env.stub.submits[0].status, 0u);
    EXPECT_EQ(env.stub.submits[0].actual_length, report.size());
    EXPECT_EQ(ret_submit_data(env.stub.submits[0]), (data_type(report.begin(), report.end())));
}

TEST(VirtualDeviceHandlers, HidSetReportDeliveredToOverride) {
    // SET_REPORT（Class|Interface|OUT 控制请求）数据经 request_set_report
    // 交给子类（键盘 LED 场景），并回 OK
    HidTestEnv env;

    const SetupPacket setup{
            .request_type = 0x21, // Class | Interface | OUT
            .request = 0x09,      // SET_REPORT
            .value = 0x0200,      // 报告类型 2（Output），报告 ID 0
            .index = 0,
            .length = 3,
    };
    const data_type payload = {'L', 'E', 'D'};
    env.handler.handle_non_standard_request_type_control_urb(
            2, UsbEndpoint::get_ep0_in(UsbSpeed::Full), 0, payload.size(), setup,
            make_cmd_submit(env.op, 2, 0x00, UsbIpDirection::Out, payload.size(), setup, payload).transfer, env.ec);
    ASSERT_FALSE(env.ec);

    EXPECT_EQ(env.handler.last_output_report, payload);
    ASSERT_EQ(env.stub.submits.size(), 1u);
    EXPECT_EQ(env.stub.submits[0].header.seqnum, 2u);
    EXPECT_EQ(env.stub.submits[0].status, 0u);
}

TEST(VirtualDeviceHandlers, HidSendInputReportDisconnectedRejected) {
    // 未建立连接时 send_input_report 被通道断连检查拒绝（数据不会白攒）
    StringPool pool;
    UsbInterface intf{.interface_class = 3, .interface_subclass = 0, .interface_protocol = 0};
    TestHidHandler handler(intf, pool); // 未调 on_new_connection

    const std::array<std::uint8_t, 2> report = {1, 2};
    handler.send_input_report(asio::buffer(report));
    EXPECT_FALSE(handler.has_pending_reports_for_test());
}

TEST(VirtualDeviceHandlers, HidInputReportQueueDropsOldestOverLimit) {
    // 报告缓冲超上限（1024）丢最旧：发 1025 个后第 1 个被丢，
    // 主机 IN 请求取到的是第 2 个报告
    HidTestEnv env;

    for (int i = 0; i < 1025; i++) {
        const std::array<std::uint8_t, 2> report = {static_cast<std::uint8_t>(i & 0xFF),
                                                    static_cast<std::uint8_t>((i >> 8) & 0xFF)};
        env.handler.send_input_report(asio::buffer(report));
    }

    env.handler.handle_interrupt_transfer(
            1, UsbEndpoint{.address = 0x81, .attributes = 0x03, .max_packet_size = 8, .interval = 10},
            0, 2, make_cmd_submit(env.op, 1, 0x01, UsbIpDirection::In, 2).transfer, env.ec);
    ASSERT_FALSE(env.ec);

    ASSERT_EQ(env.stub.submits.size(), 1u);
    EXPECT_EQ(ret_submit_data(env.stub.submits[0]), (data_type{1, 0})); // 第 2 个报告
}
