// ECM 数据接口数据面测试：bulk OUT 收帧（backend 消费 / 挂起取帧）、
// bulk IN 发帧（消息模式一帧一请求）。控制面（描述符、CDC 请求）不在本文件

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>

#include "test_utils.h"

#include "usbipdcpp/Interface.h"
#include "usbipdcpp/utils/StringPool.h"
#include "usbipdcpp/virtual_device/EcmVirtualInterfaceHandler.h"
#include "usbipdcpp/virtual_device/network_backends/NetworkBackend.h"

using namespace usbipdcpp;
using namespace usbipdcpp::test;

namespace {

// 记录收到的帧的测试后端
struct StubBackend : NetworkBackend {
    void send_frame(const std::uint8_t *data, std::size_t size) override {
        frames.emplace_back(data, data + size);
    }
    std::vector<data_type> frames;
};

// 不消费主机帧的子类：OUT 挂起走 take_frame（无 backend 且 on_frame_received
// 返回 false 时通道挂起）
class HangingEcmHandler : public EcmDataInterfaceHandler {
public:
    using EcmDataInterfaceHandler::EcmDataInterfaceHandler;
    bool on_frame_received(data_type &&frame) override {
        return false;
    }
};

// 最小可用的 ECM 数据接口（CDC Data 类）+ handler
struct EcmTestEnv {
    StringPool pool;
    // 传输分配器：必须声明在 stub 之前（成员逆序析构，op 后死）——
    // submits 里的 RetSubmit 持 TransferHandle，析构时要用 op 释放
    GenericTransferOperator op;
    StubBackend backend;
    UsbInterface intf{.interface_class = 0x0A, .interface_subclass = 0x00, .interface_protocol = 0x00};
    EcmDataInterfaceHandler handler{intf, pool, &backend};
    CaptureResponder stub;
    usbipdcpp::error_code ec;

    EcmTestEnv() : handler(intf, pool, &backend) {
        handler.on_new_connection(stub, ec);
    }
};

constexpr std::uint8_t EP_IN = 0x81;
constexpr std::uint8_t EP_OUT = 0x02;

} // namespace

TEST(TestEcmDataHandler, OutFrameDeliveredToBackendAndAcked) {
    // 有 backend：主机 OUT 帧直接交给网络侧消费并立即应答
    EcmTestEnv env;

    const data_type frame = {0xAA, 0xBB, 0x00, 0x01, 0x02, 0x03, 0x00, 0x11, 0x22, 0x33,
                             0x44, 0x55, 0x08, 0x00, 0x01, 0x02, 0x03, 0x04};
    env.handler.handle_bulk_transfer(1, UsbEndpoint{.address = EP_OUT, .attributes = 0x02, .max_packet_size = 64},
                                     0, frame.size(), make_cmd_submit(env.op, 1, EP_OUT, UsbIpDirection::Out,
                                                                      frame.size(), {}, frame).transfer,
                                     env.ec);
    ASSERT_FALSE(env.ec);

    ASSERT_EQ(env.backend.frames.size(), 1u);
    EXPECT_EQ(env.backend.frames[0], frame);

    ASSERT_EQ(env.stub.submits.size(), 1u);
    EXPECT_EQ(env.stub.submits[0].header.seqnum, 1u);
    EXPECT_EQ(env.stub.submits[0].status, 0u);
    EXPECT_EQ(env.stub.submits[0].actual_length, frame.size());
}

TEST(TestEcmDataHandler, OutFrameHangsUntilTakeFrame) {
    // 无消费方（子类 on_frame_received 返回 false）：OUT 挂起（NAK 背压），
    // 业务线程 take_frame 取走时应答
    StringPool pool;
    GenericTransferOperator op; // 先于 stub 声明（TransferHandle 析构要用它）
    UsbInterface intf{.interface_class = 0x0A, .interface_subclass = 0x00, .interface_protocol = 0x00};
    HangingEcmHandler handler(intf, pool, nullptr);
    CaptureResponder stub;
    usbipdcpp::error_code ec;
    handler.on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    const data_type frame = {'h', 'i', 0, 1, 2, 3};

    std::optional<OutEndpointChannel::Pending> taken;
    std::thread reader([&]() { taken = handler.take_frame(2000); });
    handler.handle_bulk_transfer(1, UsbEndpoint{.address = EP_OUT, .attributes = 0x02, .max_packet_size = 64},
                                 0, frame.size(), make_cmd_submit(op, 1, EP_OUT, UsbIpDirection::Out,
                                                                  frame.size(), {}, frame).transfer,
                                 ec);
    ASSERT_FALSE(ec);
    reader.join();

    ASSERT_TRUE(taken.has_value());
    EXPECT_EQ(taken->ep, EP_OUT);
    EXPECT_EQ(taken->data, frame);

    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(stub.submits[0].header.seqnum, 1u);
    EXPECT_EQ(stub.submits[0].status, 0u);
    EXPECT_EQ(stub.submits[0].actual_length, frame.size());
}

TEST(TestEcmDataHandler, InRequestServesSentFrame) {
    // send_frame 推一帧 → 主机 bulk IN 请求取走（消息模式：一个 URB 恰好一帧）
    EcmTestEnv env;

    const data_type frame = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05};
    EXPECT_EQ(env.handler.send_frame(frame), frame.size());

    env.handler.handle_bulk_transfer(2, UsbEndpoint{.address = EP_IN, .attributes = 0x02, .max_packet_size = 64},
                                     0, frame.size(), make_cmd_submit(env.op, 2, EP_IN, UsbIpDirection::In,
                                                                      frame.size()).transfer,
                                     env.ec);
    ASSERT_FALSE(env.ec);

    ASSERT_EQ(env.stub.submits.size(), 1u);
    EXPECT_EQ(env.stub.submits[0].header.seqnum, 2u);
    EXPECT_EQ(env.stub.submits[0].status, 0u);
    EXPECT_EQ(env.stub.submits[0].actual_length, frame.size());
    EXPECT_EQ(ret_submit_data(env.stub.submits[0]), frame);
}

TEST(TestEcmDataHandler, InRequestHangsThenSendFrameServesIt) {
    // 主机 IN 请求先挂起（无帧），send_frame 推入时直接应答
    EcmTestEnv env;

    env.handler.handle_bulk_transfer(3, UsbEndpoint{.address = EP_IN, .attributes = 0x02, .max_packet_size = 64},
                                     0, 8, make_cmd_submit(env.op, 3, EP_IN, UsbIpDirection::In, 8).transfer,
                                     env.ec);
    ASSERT_FALSE(env.ec);
    EXPECT_TRUE(env.stub.submits.empty()); // 挂起未应答

    const data_type frame = {9, 8, 7, 6};
    env.handler.send_frame(frame);
    ASSERT_EQ(env.stub.submits.size(), 1u);
    EXPECT_EQ(env.stub.submits[0].header.seqnum, 3u);
    EXPECT_EQ(env.stub.submits[0].actual_length, frame.size()); // 帧比请求短：短包结束
    EXPECT_EQ(ret_submit_data(env.stub.submits[0]), frame);
}

TEST(TestEcmDataHandler, UnlinkPendingInRequest) {
    // 挂起的 IN 请求被 UNLINK 取消：回 RET_UNLINK(-ECONNRESET)，且不再发
    // RET_SUBMIT（请求已从队列移除）
    EcmTestEnv env;

    env.handler.handle_bulk_transfer(1, UsbEndpoint{.address = EP_IN, .attributes = 0x02, .max_packet_size = 64},
                                     0, 8, make_cmd_submit(env.op, 1, EP_IN, UsbIpDirection::In, 8).transfer,
                                     env.ec);
    ASSERT_FALSE(env.ec);
    EXPECT_TRUE(env.stub.submits.empty());

    env.handler.handle_unlink_seqnum(1, 2);
    ASSERT_EQ(env.stub.unlinks.size(), 1u);
    EXPECT_EQ(env.stub.unlinks[0].header.seqnum, 2u);
    EXPECT_EQ(env.stub.unlinks[0].status, static_cast<std::uint32_t>(UrbStatusType::StatusECONNRESET));
    EXPECT_TRUE(env.stub.submits.empty());
}

TEST(TestEcmDataHandler, UnlinkPendingOutRequest) {
    // 挂起的 OUT 请求（无消费方）被 UNLINK 取消：回 RET_UNLINK(-ECONNRESET)
    StringPool pool;
    GenericTransferOperator op; // 先于 stub 声明（TransferHandle 析构要用它）
    UsbInterface intf{.interface_class = 0x0A, .interface_subclass = 0x00, .interface_protocol = 0x00};
    HangingEcmHandler handler(intf, pool, nullptr);
    CaptureResponder stub;
    usbipdcpp::error_code ec;
    handler.on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    const data_type frame = {1, 2, 3};
    handler.handle_bulk_transfer(1, UsbEndpoint{.address = EP_OUT, .attributes = 0x02, .max_packet_size = 64},
                                 0, frame.size(), make_cmd_submit(op, 1, EP_OUT, UsbIpDirection::Out,
                                                                  frame.size(), {}, frame).transfer,
                                 ec);
    ASSERT_FALSE(ec);
    EXPECT_TRUE(stub.submits.empty()); // 挂起未应答

    handler.handle_unlink_seqnum(1, 2);
    ASSERT_EQ(stub.unlinks.size(), 1u);
    EXPECT_EQ(stub.unlinks[0].header.seqnum, 2u);
    EXPECT_EQ(stub.unlinks[0].status, static_cast<std::uint32_t>(UrbStatusType::StatusECONNRESET));
    EXPECT_TRUE(stub.submits.empty());
}

TEST(TestEcmDataHandler, TxBufferDropsOldestOverLimit) {
    // 发帧缓冲超上限丢最旧（网络数据可丢，TCP 重传兜底）：上限 2 时发 3 帧，
    // 第 1 帧被丢，主机 IN 请求只能取到后两帧
    EcmTestEnv env;
    env.handler.set_tx_max_pending(2);

    env.handler.send_frame(data_type{1, 1, 1});
    env.handler.send_frame(data_type{2, 2, 2});
    env.handler.send_frame(data_type{3, 3, 3}); // 挤掉第 1 帧

    env.handler.handle_bulk_transfer(1, UsbEndpoint{.address = EP_IN, .attributes = 0x02, .max_packet_size = 64},
                                     0, 8, make_cmd_submit(env.op, 1, EP_IN, UsbIpDirection::In, 8).transfer,
                                     env.ec);
    env.handler.handle_bulk_transfer(2, UsbEndpoint{.address = EP_IN, .attributes = 0x02, .max_packet_size = 64},
                                     0, 8, make_cmd_submit(env.op, 2, EP_IN, UsbIpDirection::In, 8).transfer,
                                     env.ec);
    ASSERT_FALSE(env.ec);

    ASSERT_EQ(env.stub.submits.size(), 2u);
    EXPECT_EQ(ret_submit_data(env.stub.submits[0]), (data_type{2, 2, 2}));
    EXPECT_EQ(ret_submit_data(env.stub.submits[1]), (data_type{3, 3, 3}));

    // 第三个 IN 请求挂起（缓冲已空）
    env.handler.handle_bulk_transfer(3, UsbEndpoint{.address = EP_IN, .attributes = 0x02, .max_packet_size = 64},
                                     0, 8, make_cmd_submit(env.op, 3, EP_IN, UsbIpDirection::In, 8).transfer,
                                     env.ec);
    EXPECT_EQ(env.stub.submits.size(), 2u);
}

TEST(TestEcmDataHandler, FrameLongerThanInRequestTruncated) {
    // 帧比主机 IN 请求长：消息模式按请求长度截断应答（整条消息被消费，
    // 剩余部分丢弃——主机 usbnet 请求一般 ≥ MTU，短请求属异常主机）
    EcmTestEnv env;

    env.handler.send_frame(data_type{1, 2, 3, 4, 5, 6});
    env.handler.handle_bulk_transfer(1, UsbEndpoint{.address = EP_IN, .attributes = 0x02, .max_packet_size = 64},
                                     0, 4, make_cmd_submit(env.op, 1, EP_IN, UsbIpDirection::In, 4).transfer,
                                     env.ec);
    ASSERT_FALSE(env.ec);

    ASSERT_EQ(env.stub.submits.size(), 1u);
    EXPECT_EQ(env.stub.submits[0].actual_length, 4u);
    EXPECT_EQ(ret_submit_data(env.stub.submits[0]), (data_type{1, 2, 3, 4}));
}

TEST(TestEcmDataHandler, SendFrameAfterDisconnectDropsData) {
    // 断连后 send_frame 仍返回请求大小（调用方无感知），但数据被通道丢弃
    //（断连拒绝入队）；重连后从干净缓冲开始，无残留
    EcmTestEnv env;
    std::error_code disc_ec;
    env.handler.on_disconnection(disc_ec);

    EXPECT_EQ(env.handler.send_frame(data_type{1, 2, 3}), 3u);
    EXPECT_TRUE(env.stub.submits.empty());

    // 重连后 IN 请求挂起（缓冲为空，无残留数据）
    std::error_code conn_ec;
    env.handler.on_new_connection(env.stub, conn_ec);
    env.handler.handle_bulk_transfer(1, UsbEndpoint{.address = EP_IN, .attributes = 0x02, .max_packet_size = 64},
                                     0, 8, make_cmd_submit(env.op, 1, EP_IN, UsbIpDirection::In, 8).transfer,
                                     env.ec);
    ASSERT_FALSE(env.ec);
    EXPECT_TRUE(env.stub.submits.empty()); // 挂起未应答
}
