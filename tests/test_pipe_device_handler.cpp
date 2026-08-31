#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

#include "test_utils.h"

#include "usbipdcpp/Device.h"
#include "usbipdcpp/DeviceHandler/TransferOperator.h"
#include "usbipdcpp/protocol.h"
#include "usbipdcpp/utils/StringPool.h"
#include "usbipdcpp/virtual_device/PipeDeviceHandler.h"

using namespace usbipdcpp;
using namespace usbipdcpp::test;

namespace {

// 管道设备 + 其 handler 指针（handler 由 device 持有，生命周期同 device）
struct PipeFixture {
    std::shared_ptr<UsbDevice> device;
    PipeDeviceHandler *pipe = nullptr;
};

// 构造管道设备（与 examples/mock_pipe 相同的构造方式）：
// vendor 类接口 + bulk IN(0x81) + bulk OUT(0x02)
PipeFixture make_pipe_device(StringPool &string_pool) {
    std::vector<UsbInterface> interfaces = {
            UsbInterface{
                    .interface_class = 0xFF, // vendor specific
                    .interface_subclass = 0x00,
                    .interface_protocol = 0x00,
                    .endpoints = {{
                            UsbEndpoint{
                                    .address = 0x81, // IN
                                    .attributes = 0x02, // Bulk
                                    .max_packet_size = 64,
                                    .interval = 0,
                            },
                            UsbEndpoint{
                                    .address = 0x02, // OUT
                                    .attributes = 0x02, // Bulk
                                    .max_packet_size = 64,
                                    .interval = 0,
                            },
                    }},
            },
    };
    auto device = std::make_shared<UsbDevice>(UsbDevice{
            .path = "/test/mock_pipe",
            .busid = "1-1",
            .bus_num = 1,
            .dev_num = 1,
            .speed = static_cast<std::uint32_t>(UsbSpeed::Full),
            .vendor_id = 0x1234,
            .product_id = 0x5690,
            .device_bcd = 0x0100,
            .device_class = 0x00,
            .device_subclass = 0x00,
            .device_protocol = 0x00,
            .configuration_value = 1,
            .num_configurations = 1,
            .interfaces = interfaces,
            .ep0_in = UsbEndpoint::get_ep0_in(UsbSpeed::Full),
            .ep0_out = UsbEndpoint::get_ep0_out(UsbSpeed::Full),
    });
    auto pipe = device->with_handler<PipeDeviceHandler>(string_pool);
    pipe->setup_interface_handlers();
    return {device, pipe.get()};
}

// 构造双 OUT 端点管道设备（bulk IN 0x81 + bulk OUT 0x02/0x04），
// 用于验证 read 的跨端点全局顺序
PipeFixture make_pipe_device_two_out(StringPool &string_pool) {
    std::vector<UsbInterface> interfaces = {
            UsbInterface{
                    .interface_class = 0xFF,
                    .interface_subclass = 0x00,
                    .interface_protocol = 0x00,
                    .endpoints = {{
                            UsbEndpoint{
                                    .address = 0x81, // IN
                                    .attributes = 0x02,
                                    .max_packet_size = 64,
                                    .interval = 0,
                            },
                            UsbEndpoint{
                                    .address = 0x02, // OUT
                                    .attributes = 0x02,
                                    .max_packet_size = 64,
                                    .interval = 0,
                            },
                            UsbEndpoint{
                                    .address = 0x04, // OUT
                                    .attributes = 0x02,
                                    .max_packet_size = 64,
                                    .interval = 0,
                            },
                    }},
            },
    };
    auto device = std::make_shared<UsbDevice>(UsbDevice{
            .path = "/test/mock_pipe_two_out",
            .busid = "1-1",
            .bus_num = 1,
            .dev_num = 1,
            .speed = static_cast<std::uint32_t>(UsbSpeed::Full),
            .vendor_id = 0x1234,
            .product_id = 0x5690,
            .device_bcd = 0x0100,
            .device_class = 0x00,
            .device_subclass = 0x00,
            .device_protocol = 0x00,
            .configuration_value = 1,
            .num_configurations = 1,
            .interfaces = interfaces,
            .ep0_in = UsbEndpoint::get_ep0_in(UsbSpeed::Full),
            .ep0_out = UsbEndpoint::get_ep0_out(UsbSpeed::Full),
    });
    auto pipe = device->with_handler<PipeDeviceHandler>(string_pool);
    pipe->setup_interface_handlers();
    return {device, pipe.get()};
}

} // namespace

TEST(TestPipeDeviceHandler, OutDataReachesRead) {
    // OUT 传输：数据经 read() 返回，ep 与数据一致，handler 回 OK RET_SUBMIT
    StringPool string_pool;
    auto fixture = make_pipe_device(string_pool);
    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.pipe->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    auto &intf = fixture.device->interfaces[0];
    const data_type payload = {'p', 'i', 'p', 'e', '!'};

    // 业务线程阻塞 read；主线程投递 OUT 传输
    PipeXfer xfer;
    std::thread reader([&]() { ASSERT_TRUE(fixture.pipe->read(xfer, 2000)); });

    GenericTransferOperator op;
    fixture.pipe->receive_urb(make_cmd_submit(op, 1, 0x02, UsbIpDirection::Out, payload.size(), {}, payload),
                              intf.endpoints[0][1], intf, ec);
    ASSERT_FALSE(ec);

    reader.join();
    EXPECT_EQ(xfer.ep, 0x02);
    EXPECT_FALSE(xfer.setup_req.has_value());
    EXPECT_EQ(xfer.data, payload);

    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(stub.submits[0].header.seqnum, 1u);
    EXPECT_EQ(stub.submits[0].status, 0u);
    EXPECT_EQ(stub.submits[0].actual_length, payload.size());

    fixture.pipe->on_disconnection(ec);
}

TEST(TestPipeDeviceHandler, ReadPreservesGlobalRequestOrderAcrossEndpoints) {
    // 跨端点按主机请求的全局到达顺序返回（seqnum 序），不是按端点号：
    // 发 ep2(seq 1) → ep4(seq 2) → ep2(seq 3)，read 必须按 1、2、3 取回
    StringPool string_pool;
    auto fixture = make_pipe_device_two_out(string_pool);
    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.pipe->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    auto &intf = fixture.device->interfaces[0];
    const UsbEndpoint ep_out2 = intf.endpoints[0][1];
    const UsbEndpoint ep_out4 = intf.endpoints[0][2];

    // 业务线程阻塞读 3 条
    std::vector<PipeXfer> got;
    std::thread reader([&]() {
        for (int i = 0; i < 3; i++) {
            PipeXfer xfer;
            if (fixture.pipe->read(xfer, 2000)) {
                got.push_back(std::move(xfer));
            }
        }
    });

    // 交错发：ep 0x02 → ep 0x04 → ep 0x02
    GenericTransferOperator op;
    fixture.pipe->receive_urb(make_cmd_submit(op, 1, 0x02, UsbIpDirection::Out, 1, {}, {0x01}), ep_out2, intf, ec);
    fixture.pipe->receive_urb(make_cmd_submit(op, 2, 0x04, UsbIpDirection::Out, 1, {}, {0x02}), ep_out4, intf, ec);
    fixture.pipe->receive_urb(make_cmd_submit(op, 3, 0x02, UsbIpDirection::Out, 1, {}, {0x03}), ep_out2, intf, ec);
    ASSERT_FALSE(ec);

    reader.join();
    ASSERT_EQ(got.size(), 3u);
    EXPECT_EQ(got[0].ep, 0x02);
    EXPECT_EQ(got[0].data, (data_type{0x01}));
    EXPECT_EQ(got[1].ep, 0x04);
    EXPECT_EQ(got[1].data, (data_type{0x02}));
    EXPECT_EQ(got[2].ep, 0x02);
    EXPECT_EQ(got[2].data, (data_type{0x03}));

    ASSERT_EQ(stub.submits.size(), 3u);
    for (std::size_t i = 0; i < 3; i++) {
        EXPECT_EQ(stub.submits[i].header.seqnum, static_cast<std::uint32_t>(i + 1));
        EXPECT_EQ(stub.submits[i].status, 0u);
        EXPECT_EQ(stub.submits[i].actual_length, 1u);
    }

    fixture.pipe->on_disconnection(ec);
}

TEST(TestPipeDeviceHandler, InWriteServesPendingRequests) {
    // IN 请求先挂起（FIFO 无数据），write 后按请求长度分片应答：
    // 两个 8 字节请求各取走 16 字节数据的一半
    StringPool string_pool;
    auto fixture = make_pipe_device(string_pool);
    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.pipe->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    auto &intf = fixture.device->interfaces[0];
    const UsbEndpoint ep_in = intf.endpoints[0][0];

    // 先挂两个 IN 请求（每个期望 8 字节）
    GenericTransferOperator op;
    fixture.pipe->receive_urb(make_cmd_submit(op, 1, 0x01, UsbIpDirection::In, 8), ep_in, intf, ec);
    fixture.pipe->receive_urb(make_cmd_submit(op, 2, 0x01, UsbIpDirection::In, 8), ep_in, intf, ec);
    ASSERT_FALSE(ec);

    // 16 字节数据一次写入，两个挂起请求各取 8 字节
    const data_type payload = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    EXPECT_EQ(fixture.pipe->write(PipeXfer{.ep = 0x81, .data = payload}, 2000), payload.size());

    ASSERT_EQ(stub.submits.size(), 2u);
    EXPECT_EQ(stub.submits[0].header.seqnum, 1u);
    EXPECT_EQ(stub.submits[0].status, 0u);
    EXPECT_EQ(stub.submits[0].actual_length, 8u);
    EXPECT_EQ(ret_submit_data(stub.submits[0]), data_type(payload.begin(), payload.begin() + 8));

    EXPECT_EQ(stub.submits[1].header.seqnum, 2u);
    EXPECT_EQ(stub.submits[1].status, 0u);
    EXPECT_EQ(stub.submits[1].actual_length, 8u);
    EXPECT_EQ(ret_submit_data(stub.submits[1]), data_type(payload.begin() + 8, payload.end()));

    fixture.pipe->on_disconnection(ec);
}

TEST(TestPipeDeviceHandler, ControlRequestDeliveredToRead) {
    // 非标准控制请求（Vendor+Interface+IN）：read() 拿到 setup_req 后
    // write({ep=0}) 应答，handler 回带数据的 RET_SUBMIT
    StringPool string_pool;
    auto fixture = make_pipe_device(string_pool);
    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.pipe->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    const SetupPacket setup{
            .request_type = 0xC1, // Vendor(0x40) | Interface(0x01) | IN(0x80)
            .request = 0x11,
            .value = 0x1234,
            .index = 0,
            .length = 4,
    };

    PipeXfer xfer;
    std::thread reader([&]() { ASSERT_TRUE(fixture.pipe->read(xfer, 2000)); });

    GenericTransferOperator op;
    fixture.pipe->receive_urb(make_cmd_submit(op, 1, 0x00, UsbIpDirection::In, 4, setup), fixture.device->ep0_in,
                              std::nullopt, ec);
    ASSERT_FALSE(ec);
    reader.join();

    ASSERT_TRUE(xfer.setup_req.has_value());
    EXPECT_EQ(xfer.ep, 0);
    EXPECT_EQ(xfer.setup_req->request_type, 0xC1u);
    EXPECT_EQ(xfer.setup_req->request, 0x11u);
    EXPECT_EQ(xfer.setup_req->value, 0x1234u);
    EXPECT_EQ(xfer.setup_req->length, 4u);
    EXPECT_TRUE(xfer.data.empty());

    // write({ep=0}) 应答控制请求
    const data_type reply = {'o', 'k', 'y', '!'};
    EXPECT_EQ(fixture.pipe->write(PipeXfer{.ep = 0, .data = reply}, 2000), reply.size());

    ASSERT_EQ(stub.submits.size(), 1u); // 控制 IN 纯通知取走不应答，只有 write 的应答
    EXPECT_EQ(stub.submits[0].header.seqnum, 1u);
    EXPECT_EQ(stub.submits[0].status, 0u);
    EXPECT_EQ(stub.submits[0].actual_length, reply.size());
    EXPECT_EQ(ret_submit_data(stub.submits[0]), reply);

    fixture.pipe->on_disconnection(ec);
}

TEST(TestPipeDeviceHandler, DisconnectUnblocksReadWrite) {
    // 断连（on_disconnection）唤醒阻塞的 read（返回 false）与 write（返回 0）
    StringPool string_pool;
    auto fixture = make_pipe_device(string_pool);
    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.pipe->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    // read 无限等待；断连时 handler 清理并唤醒
    PipeXfer xfer;
    std::thread reader([&]() { ASSERT_FALSE(fixture.pipe->read(xfer, 0)); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // 确保已阻塞
    fixture.pipe->on_disconnection(ec);
    reader.join();

    // 断连后 write 立即返回 0（不再等待 FIFO 空间）
    EXPECT_EQ(fixture.pipe->write(PipeXfer{.ep = 0x81, .data = {1, 2, 3}}, 0), 0u);
}

TEST(TestPipeDeviceHandler, FifoFullBlocksWriteUntilHostConsumes) {
    // FIFO 满时 write 阻塞（对齐内核 FIFO 语义），宿主 IN 请求取走数据后
    // 继续写完剩余部分；分两次取走的字节拼接后与原始数据一致
    StringPool string_pool;
    auto fixture = make_pipe_device(string_pool);
    fixture.pipe->set_in_fifo_capacity(16); // 连接前设置，FIFO 只装得下 16 字节
    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.pipe->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    auto &intf = fixture.device->interfaces[0];
    const UsbEndpoint ep_in = intf.endpoints[0][0];

    const data_type payload = [] {
        data_type data;
        for (std::uint8_t i = 0; i < 32; i++) {
            data.push_back(i);
        }
        return data;
    }();

    std::size_t written = 0;
    std::thread writer([&]() { written = fixture.pipe->write(PipeXfer{.ep = 0x81, .data = payload}, 0); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // 确保 write 已阻塞在 FIFO 满

    // 宿主取走前 16 字节：write 腾出空间后写完剩余
    GenericTransferOperator op;
    fixture.pipe->receive_urb(make_cmd_submit(op, 1, 0x01, UsbIpDirection::In, 16), ep_in, intf, ec);
    ASSERT_FALSE(ec);

    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(stub.submits[0].header.seqnum, 1u);
    EXPECT_EQ(stub.submits[0].actual_length, 16u);
    EXPECT_EQ(ret_submit_data(stub.submits[0]), data_type(payload.begin(), payload.begin() + 16));

    writer.join();
    EXPECT_EQ(written, payload.size());

    // 再取走剩余 16 字节
    fixture.pipe->receive_urb(make_cmd_submit(op, 2, 0x01, UsbIpDirection::In, 16), ep_in, intf, ec);
    ASSERT_FALSE(ec);
    ASSERT_EQ(stub.submits.size(), 2u);
    EXPECT_EQ(stub.submits[1].header.seqnum, 2u);
    EXPECT_EQ(stub.submits[1].actual_length, 16u);
    EXPECT_EQ(ret_submit_data(stub.submits[1]), data_type(payload.begin() + 16, payload.end()));

    fixture.pipe->on_disconnection(ec);
}

TEST(TestPipeDeviceHandler, CustomDescriptorServedToHost) {
    // set_custom_descriptor 设置的报告描述符经 GET_DESCRIPTOR(0x22) 返回
    // （标准请求 recipient=接口）：HID 设备用此 API 提供报告描述符
    StringPool string_pool;
    auto fixture = make_pipe_device(string_pool);
    // 简版键盘报告描述符（验证机制，内容不限）
    const data_type report_desc = {0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0xC0};
    fixture.pipe->set_custom_descriptor(0x22, report_desc);
    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.pipe->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    // GET_DESCRIPTOR(Report)：IN | Standard | Interface，wValue 高字节=0x22
    const SetupPacket setup{
            .request_type = 0x81,
            .request = 0x06, // GET_DESCRIPTOR
            .value = 0x2200,
            .index = 0,
            .length = static_cast<std::uint16_t>(report_desc.size()),
    };
    GenericTransferOperator op;
    fixture.pipe->receive_urb(make_cmd_submit(op, 1, 0x00, UsbIpDirection::In, report_desc.size(), setup),
                              fixture.device->ep0_in, std::nullopt, ec);
    ASSERT_FALSE(ec);

    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(stub.submits[0].header.seqnum, 1u);
    EXPECT_EQ(stub.submits[0].status, 0u);
    EXPECT_EQ(stub.submits[0].actual_length, report_desc.size());
    EXPECT_EQ(ret_submit_data(stub.submits[0]), report_desc);

    fixture.pipe->on_disconnection(ec);
}

TEST(TestPipeDeviceHandler, ControlOutRequestDeliveredToRead) {
    // 控制请求 OUT 方向：read() 同时拿到 setup_req 与数据阶段
    StringPool string_pool;
    auto fixture = make_pipe_device(string_pool);
    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.pipe->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    // Vendor+Interface+OUT 控制请求，携带 3 字节数据（如 HID SET_REPORT）
    const SetupPacket setup{
            .request_type = 0x41, // Vendor(0x40) | Interface(0x01) | OUT(0x00)
            .request = 0x09,
            .value = 0x0200,
            .index = 0,
            .length = 3,
    };
    const data_type payload = {'L', 'E', 'D'};

    PipeXfer xfer;
    std::thread reader([&]() { ASSERT_TRUE(fixture.pipe->read(xfer, 2000)); });

    GenericTransferOperator op;
    fixture.pipe->receive_urb(make_cmd_submit(op, 1, 0x00, UsbIpDirection::Out, payload.size(), setup, payload),
                              fixture.device->ep0_in, std::nullopt, ec);
    ASSERT_FALSE(ec);
    reader.join();

    ASSERT_TRUE(xfer.setup_req.has_value());
    EXPECT_EQ(xfer.ep, 0);
    EXPECT_EQ(xfer.setup_req->request_type, 0x41u);
    EXPECT_EQ(xfer.setup_req->length, 3u);
    EXPECT_EQ(xfer.data, payload);

    // handler 已回 OK（OUT 无数据阶段）
    ASSERT_EQ(stub.submits.size(), 1u);
    EXPECT_EQ(stub.submits[0].header.seqnum, 1u);
    EXPECT_EQ(stub.submits[0].status, 0u);
    EXPECT_EQ(stub.submits[0].actual_length, payload.size());

    fixture.pipe->on_disconnection(ec);
}

TEST(TestPipeDeviceHandler, ReadWriteTimeout) {
    // read/write 超时语义：无数据/无空间时按 timeout_ms 返回（不无限阻塞）
    StringPool string_pool;
    auto fixture = make_pipe_device(string_pool);
    fixture.pipe->set_in_fifo_capacity(8); // 小 FIFO 让 write 快速填满
    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.pipe->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    // read 超时：无数据时 100ms 后返回 false
    PipeXfer xfer;
    auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(fixture.pipe->read(xfer, 100));
    EXPECT_GE(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(100));

    // write 超时：FIFO 满且无 IN 请求时只写入 FIFO 容量部分
    const data_type payload(16, 0xAB);
    auto written = fixture.pipe->write(PipeXfer{.ep = 0x81, .data = payload}, 100);
    EXPECT_EQ(written, 8u); // 容量 8，写满即停

    fixture.pipe->on_disconnection(ec);
}

TEST(TestPipeDeviceHandler, UnlinkPendingRequest) {
    // 挂起的 IN 请求被 UNLINK 取消：回 RET_UNLINK 且状态为 -ECONNRESET
    // （对齐内核 stub_tx.c 对取消 URB 的处理）
    StringPool string_pool;
    auto fixture = make_pipe_device(string_pool);
    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.pipe->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    auto &intf = fixture.device->interfaces[0];
    const UsbEndpoint ep_in = intf.endpoints[0][0];

    // IN 请求挂起（FIFO 无数据）
    GenericTransferOperator op;
    fixture.pipe->receive_urb(make_cmd_submit(op, 1, 0x01, UsbIpDirection::In, 8), ep_in, intf, ec);
    ASSERT_FALSE(ec);
    // 取消该请求
    fixture.pipe->handle_unlink_seqnum(1, 2);

    ASSERT_EQ(stub.unlinks.size(), 1u);
    EXPECT_EQ(stub.unlinks[0].header.seqnum, 2u); // RET_UNLINK 回的是 unlink 命令自身的 seqnum
    EXPECT_EQ(stub.unlinks[0].status, static_cast<std::uint32_t>(UrbStatusType::StatusECONNRESET));
    EXPECT_TRUE(stub.submits.empty()); // 已取消的请求不再发 RET_SUBMIT

    fixture.pipe->on_disconnection(ec);
}

TEST(TestPipeDeviceHandler, UnlinkCompletedRequestReportsSuccess) {
    // UNLINK 目标已正常完成（RET_SUBMIT 已发出）：请求不在挂起队列里，
    // RET_UNLINK 回 0（URB 已完成，对齐 stub_tx.c：找不到目标回 0）
    StringPool string_pool;
    auto fixture = make_pipe_device(string_pool);
    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.pipe->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);

    auto &intf = fixture.device->interfaces[0];
    const UsbEndpoint ep_in = intf.endpoints[0][0];

    // IN 请求挂起后 write 应答（传输已完成）
    GenericTransferOperator op;
    fixture.pipe->receive_urb(make_cmd_submit(op, 1, 0x01, UsbIpDirection::In, 8), ep_in, intf, ec);
    ASSERT_FALSE(ec);
    const data_type payload = {1, 2, 3, 4};
    EXPECT_EQ(fixture.pipe->write(PipeXfer{.ep = 0x81, .data = payload}, 2000), payload.size());
    ASSERT_EQ(stub.submits.size(), 1u); // RET_SUBMIT 已发出

    // 取消已完成的目标：回 0
    fixture.pipe->handle_unlink_seqnum(1, 2);
    ASSERT_EQ(stub.unlinks.size(), 1u);
    EXPECT_EQ(stub.unlinks[0].header.seqnum, 2u);
    EXPECT_EQ(stub.unlinks[0].status, 0u);

    fixture.pipe->on_disconnection(ec);
}

TEST(TestPipeDeviceHandler, TriggerSessionStopNotifiesResponder) {
    // 设备拔出/异常时 trigger_session_stop → responder->stop_transfer()
    // （通知 Session 立即停止，接口解耦的停止链路）
    StringPool string_pool;
    auto fixture = make_pipe_device(string_pool);
    CaptureResponder stub;
    usbipdcpp::error_code ec;
    fixture.pipe->on_new_connection(stub, ec);
    ASSERT_FALSE(ec);
    EXPECT_FALSE(stub.stopped);

    fixture.pipe->trigger_session_stop();
    EXPECT_TRUE(stub.stopped);

    fixture.pipe->on_disconnection(ec);
}
