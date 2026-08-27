#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <limits>
#include <string>
#include <thread>

#include "test_utils.h"

#include "usbipdcpp/Device.h"
#include "usbipdcpp/DeviceHandler/TransferOperator.h"
#include "usbipdcpp/Server.h"
#include "usbipdcpp/network.h"
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

// 发送 import 请求并读完回复（含设备描述符），返回回复中的 status；任何错误返回最大值
std::uint32_t import_device(asio::ip::tcp::socket &client, const std::string &busid) {
    UsbIpCommand::OpReqImport req{.status = 0, .busid = {}};
    std::copy(busid.begin(), busid.end(), req.busid.begin());
    usbipdcpp::error_code send_ec;
    req.to_socket(client, send_ec);
    if (send_ec) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    std::uint16_t version = 0;
    std::uint16_t command = 0;
    data_read_from_socket(client, version, command);
    if (command != OP_REP_IMPORT) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    UsbIpResponse::OpRepImport rep;
    rep.from_socket(client); // 顺带消费设备描述符，避免残留字节干扰后续响应读取
    return rep.status;
}

// 发送 CMD_SUBMIT。out_data 非空时为 OUT 传输（携带数据阶段）；
// IN 请求的 transfer_buffer_length 是期望返回的长度
void send_cmd_submit(asio::ip::tcp::socket &client, std::uint32_t seqnum, std::uint8_t ep_num,
                     std::uint32_t direction, std::uint32_t transfer_buffer_length,
                     const SetupPacket &setup = {}, const data_type &out_data = {}) {
    // op 必须声明在 submit 之前（后声明先析构）：submit 析构时 transfer 要
    // 用 op 释放 handle，op 得比 submit 活得久
    GenericTransferOperator out_op;
    UsbIpCommand::UsbIpCmdSubmit submit{};
    submit.header.command = USBIP_CMD_SUBMIT;
    submit.header.seqnum = seqnum;
    submit.header.devid = 1;
    submit.header.direction = direction;
    submit.header.ep = ep_num; // 线格式端点号（不带方向位，方向由 direction 字段给出）
    submit.transfer_flags = 0;
    submit.transfer_buffer_length = transfer_buffer_length;
    submit.start_frame = 0;
    submit.number_of_packets = 0;
    submit.interval = 0;
    submit.setup = setup;
    if (direction == UsbIpDirection::Out && !out_data.empty()) {
        auto *trx = new GenericTransfer{};
        trx->data = out_data;
        TransferHandle handle(trx, &out_op);
        submit.transfer = std::move(handle);
    }
    usbipdcpp::error_code send_ec;
    submit.to_socket(client, send_ec);
    ASSERT_FALSE(send_ec);
}

// 发送 CMD_UNLINK 取消指定的挂起请求
void send_cmd_unlink(asio::ip::tcp::socket &client, std::uint32_t seqnum, std::uint32_t unlink_seqnum) {
    UsbIpCommand::UsbIpCmdUnlink unlink{};
    unlink.header.command = USBIP_CMD_UNLINK;
    unlink.header.seqnum = seqnum;
    unlink.header.devid = 1;
    unlink.header.direction = 0;
    unlink.header.ep = 0;
    unlink.unlink_seqnum = unlink_seqnum;
    usbipdcpp::error_code send_ec;
    unlink.to_socket(client, send_ec);
    ASSERT_FALSE(send_ec);
}

// 读取一个 RET_SUBMIT 响应。from_socket 只读 48 字节头部（数据阶段是否
// 存在只有发起方知道：OUT 响应无数据阶段，IN 才有——对齐内核 vhci 按 URB
// 方向决定），has_data_phase 由调用方按自己发出的传输方向传入
struct RetSubmitRead {
    std::uint32_t seqnum = 0;
    std::uint32_t status = 0;
    std::uint32_t actual_length = 0;
    data_type data;
};

RetSubmitRead read_ret_submit(asio::ip::tcp::socket &client, bool has_data_phase) {
    // 命令码不读进结构体（对齐 CmdSubmit::from_socket 模式），先消费 4 字节
    std::uint32_t command = read_u32(client);
    if (command != USBIP_RET_SUBMIT) {
        // 不匹配则不再读，避免错位读到垃圾数据
        EXPECT_EQ(command, USBIP_RET_SUBMIT);
        return {};
    }
    UsbIpResponse::UsbIpRetSubmit ret;
    ret.from_socket(client);
    RetSubmitRead r{.seqnum = ret.header.seqnum,
                    .status = ret.status,
                    .actual_length = ret.actual_length};
    if (has_data_phase && ret.actual_length > 0) {
        r.data.resize(ret.actual_length);
        std::error_code recv_ec;
        asio::read(client, asio::buffer(r.data), recv_ec);
        EXPECT_FALSE(recv_ec);
    }
    return r;
}

// 读取一个 RET_UNLINK 响应，返回 status；out_seqnum 带回 RET_UNLINK 的 seqnum
std::uint32_t read_ret_unlink(asio::ip::tcp::socket &client, std::uint32_t *out_seqnum = nullptr) {
    // 命令码不读进结构体，先消费 4 字节（对齐 RetSubmit 的读取方式）
    std::uint32_t command = read_u32(client);
    if (command != USBIP_RET_UNLINK) {
        // 不匹配则不再读，避免错位读到垃圾数据
        EXPECT_EQ(command, USBIP_RET_UNLINK);
        return std::numeric_limits<std::uint32_t>::max();
    }
    UsbIpResponse::UsbIpRetUnlink ret;
    ret.from_socket(client);
    if (out_seqnum) {
        *out_seqnum = ret.header.seqnum;
    }
    return ret.status;
}

} // namespace

TEST(TestPipeDeviceHandler, OutDataReachesRead) {
    // OUT 传输：数据经 read() 返回，ep 与数据一致，服务器回 OK RET_SUBMIT
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    StringPool string_pool;
    auto fixture = make_pipe_device(string_pool);
    usbipdcpp::Server server;
    server.add_device(std::move(fixture.device));

    ASSERT_FALSE(server.start(ep));
    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u);

    // 业务线程阻塞 read；主线程发 OUT 传输
    PipeXfer xfer;
    std::thread reader([&]() { ASSERT_TRUE(fixture.pipe->read(xfer, 2000)); });

    const data_type payload = {'p', 'i', 'p', 'e', '!'};
    send_cmd_submit(client, 1, 0x02, UsbIpDirection::Out, payload.size(), {}, payload);

    auto ret = read_ret_submit(client, false); // OUT 响应无数据阶段
    EXPECT_EQ(ret.seqnum, 1u);
    EXPECT_EQ(ret.status, 0u);
    EXPECT_EQ(ret.actual_length, payload.size());

    reader.join();
    EXPECT_EQ(xfer.ep, 0x02);
    EXPECT_FALSE(xfer.setup_req.has_value());
    EXPECT_EQ(xfer.data, payload);

    client.close();
    ASSERT_TRUE(wait_sessions_gone(server));
    server.stop();
}

TEST(TestPipeDeviceHandler, InWriteServesPendingRequests) {
    // IN 请求先挂起（FIFO 无数据），write 后按请求长度分片应答：
    // 两个 8 字节请求各取走 16 字节数据的一半
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    StringPool string_pool;
    auto fixture = make_pipe_device(string_pool);
    usbipdcpp::Server server;
    server.add_device(std::move(fixture.device));

    ASSERT_FALSE(server.start(ep));
    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u);

    // 先挂两个 IN 请求（每个期望 8 字节）
    send_cmd_submit(client, 1, 0x01, UsbIpDirection::In, 8);
    send_cmd_submit(client, 2, 0x01, UsbIpDirection::In, 8);

    // 16 字节数据一次写入，两个挂起请求各取 8 字节
    const data_type payload = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    EXPECT_EQ(fixture.pipe->write(PipeXfer{.ep = 0x81, .data = payload}, 2000), payload.size());

    auto ret1 = read_ret_submit(client, true); // IN 响应带数据阶段
    EXPECT_EQ(ret1.seqnum, 1u);
    EXPECT_EQ(ret1.status, 0u);
    EXPECT_EQ(ret1.actual_length, 8u);
    EXPECT_EQ(ret1.data, data_type(payload.begin(), payload.begin() + 8));

    auto ret2 = read_ret_submit(client, true);
    EXPECT_EQ(ret2.seqnum, 2u);
    EXPECT_EQ(ret2.status, 0u);
    EXPECT_EQ(ret2.actual_length, 8u);
    EXPECT_EQ(ret2.data, data_type(payload.begin() + 8, payload.end()));

    client.close();
    ASSERT_TRUE(wait_sessions_gone(server));
    server.stop();
}

TEST(TestPipeDeviceHandler, ControlRequestDeliveredToRead) {
    // 非标准控制请求（Vendor+Interface+IN）：read() 拿到 setup_req 后
    // write({ep=0}) 应答，服务器回带数据的 RET_SUBMIT
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    StringPool string_pool;
    auto fixture = make_pipe_device(string_pool);
    usbipdcpp::Server server;
    server.add_device(std::move(fixture.device));

    ASSERT_FALSE(server.start(ep));
    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u);

    const SetupPacket setup{
            .request_type = 0xC1, // Vendor(0x40) | Interface(0x01) | IN(0x80)
            .request = 0x11,
            .value = 0x1234,
            .index = 0,
            .length = 4,
    };

    PipeXfer xfer;
    std::thread reader([&]() { ASSERT_TRUE(fixture.pipe->read(xfer, 2000)); });
    send_cmd_submit(client, 1, 0x00, UsbIpDirection::In, 4, setup);
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

    auto ret = read_ret_submit(client, true); // 控制应答 IN 带数据阶段
    EXPECT_EQ(ret.seqnum, 1u);
    EXPECT_EQ(ret.status, 0u);
    EXPECT_EQ(ret.actual_length, reply.size());
    EXPECT_EQ(ret.data, reply);

    client.close();
    ASSERT_TRUE(wait_sessions_gone(server));
    server.stop();
}

TEST(TestPipeDeviceHandler, DisconnectUnblocksReadWrite) {
    // 断连（server.stop()）唤醒阻塞的 read（返回 false）与 write（返回 0）
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    StringPool string_pool;
    auto fixture = make_pipe_device(string_pool);
    usbipdcpp::Server server;
    server.add_device(std::move(fixture.device));

    ASSERT_FALSE(server.start(ep));
    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u);

    // read 无限等待；stop() 时 handler 断连清理并唤醒
    PipeXfer xfer;
    std::thread reader([&]() { ASSERT_FALSE(fixture.pipe->read(xfer, 0)); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // 确保已阻塞
    server.stop();
    reader.join();

    // 断连后 write 立即返回 0（不再等待 FIFO 空间）
    EXPECT_EQ(fixture.pipe->write(PipeXfer{.ep = 0x81, .data = {1, 2, 3}}, 0), 0u);

    client.close();
    ASSERT_TRUE(wait_sessions_gone(server));
    server.stop();
}

TEST(TestPipeDeviceHandler, FifoFullBlocksWriteUntilHostConsumes) {
    // FIFO 满时 write 阻塞（对齐内核 FIFO 语义），宿主 IN 请求取走数据后
    // 继续写完剩余部分；分两次取走的字节拼接后与原始数据一致
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    StringPool string_pool;
    auto fixture = make_pipe_device(string_pool);
    fixture.pipe->set_in_fifo_capacity(16); // 连接前设置，FIFO 只装得下 16 字节
    usbipdcpp::Server server;
    server.add_device(std::move(fixture.device));

    ASSERT_FALSE(server.start(ep));
    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u);

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
    send_cmd_submit(client, 1, 0x01, UsbIpDirection::In, 16);
    auto ret1 = read_ret_submit(client, true); // IN 响应带数据阶段
    EXPECT_EQ(ret1.seqnum, 1u);
    EXPECT_EQ(ret1.actual_length, 16u);
    EXPECT_EQ(ret1.data, data_type(payload.begin(), payload.begin() + 16));

    writer.join();
    EXPECT_EQ(written, payload.size());

    // 再取走剩余 16 字节
    send_cmd_submit(client, 2, 0x01, UsbIpDirection::In, 16);
    auto ret2 = read_ret_submit(client, true);
    EXPECT_EQ(ret2.seqnum, 2u);
    EXPECT_EQ(ret2.actual_length, 16u);
    EXPECT_EQ(ret2.data, data_type(payload.begin() + 16, payload.end()));

    client.close();
    ASSERT_TRUE(wait_sessions_gone(server));
    server.stop();
}

TEST(TestPipeDeviceHandler, CustomDescriptorServedToHost) {
    // set_custom_descriptor 设置的报告描述符经 GET_DESCRIPTOR(0x22) 返回
    // （标准请求 recipient=接口）：HID 设备用此 API 提供报告描述符
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    StringPool string_pool;
    auto fixture = make_pipe_device(string_pool);
    // 简版键盘报告描述符（验证机制，内容不限）
    const data_type report_desc = {0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0xC0};
    fixture.pipe->set_custom_descriptor(0x22, report_desc);
    usbipdcpp::Server server;
    server.add_device(std::move(fixture.device));

    ASSERT_FALSE(server.start(ep));
    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u);

    // GET_DESCRIPTOR(Report)：IN | Standard | Interface，wValue 高字节=0x22
    const SetupPacket setup{
            .request_type = 0x81,
            .request = 0x06, // GET_DESCRIPTOR
            .value = 0x2200,
            .index = 0,
            .length = static_cast<std::uint16_t>(report_desc.size()),
    };
    send_cmd_submit(client, 1, 0x00, UsbIpDirection::In, report_desc.size(), setup);

    auto ret = read_ret_submit(client, true); // 标准请求响应带数据阶段
    EXPECT_EQ(ret.seqnum, 1u);
    EXPECT_EQ(ret.status, 0u);
    EXPECT_EQ(ret.actual_length, report_desc.size());
    EXPECT_EQ(ret.data, report_desc);

    client.close();
    ASSERT_TRUE(wait_sessions_gone(server));
    server.stop();
}

TEST(TestPipeDeviceHandler, ControlOutRequestDeliveredToRead) {
    // 控制请求 OUT 方向：read() 同时拿到 setup_req 与数据阶段
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    StringPool string_pool;
    auto fixture = make_pipe_device(string_pool);
    usbipdcpp::Server server;
    server.add_device(std::move(fixture.device));

    ASSERT_FALSE(server.start(ep));
    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u);

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
    send_cmd_submit(client, 1, 0x00, UsbIpDirection::Out, payload.size(), setup, payload);
    reader.join();

    ASSERT_TRUE(xfer.setup_req.has_value());
    EXPECT_EQ(xfer.ep, 0);
    EXPECT_EQ(xfer.setup_req->request_type, 0x41u);
    EXPECT_EQ(xfer.setup_req->length, 3u);
    EXPECT_EQ(xfer.data, payload);

    // 服务器已回 OK（无数据阶段）
    auto ret = read_ret_submit(client, false);
    EXPECT_EQ(ret.seqnum, 1u);
    EXPECT_EQ(ret.status, 0u);
    EXPECT_EQ(ret.actual_length, payload.size());

    client.close();
    ASSERT_TRUE(wait_sessions_gone(server));
    server.stop();
}

TEST(TestPipeDeviceHandler, ReadWriteTimeout) {
    // read/write 超时语义：无数据/无空间时按 timeout_ms 返回（不无限阻塞）
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    StringPool string_pool;
    auto fixture = make_pipe_device(string_pool);
    fixture.pipe->set_in_fifo_capacity(8); // 小 FIFO 让 write 快速填满
    usbipdcpp::Server server;
    server.add_device(std::move(fixture.device));

    ASSERT_FALSE(server.start(ep));
    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u);

    // read 超时：无数据时 100ms 后返回 false
    PipeXfer xfer;
    auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(fixture.pipe->read(xfer, 100));
    EXPECT_GE(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(100));

    // write 超时：FIFO 满且无 IN 请求时只写入 FIFO 容量部分
    const data_type payload(16, 0xAB);
    auto written = fixture.pipe->write(PipeXfer{.ep = 0x81, .data = payload}, 100);
    EXPECT_EQ(written, 8u); // 容量 8，写满即停

    client.close();
    ASSERT_TRUE(wait_sessions_gone(server));
    server.stop();
}

TEST(TestPipeDeviceHandler, UnlinkPendingRequest) {
    // 挂起的 IN 请求被 UNLINK 取消：回 RET_UNLINK 且状态为 -ECONNRESET
    // （对齐内核 stub_tx.c 对取消 URB 的处理）
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    StringPool string_pool;
    auto fixture = make_pipe_device(string_pool);
    usbipdcpp::Server server;
    server.add_device(std::move(fixture.device));

    ASSERT_FALSE(server.start(ep));
    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u);

    // IN 请求挂起（FIFO 无数据）
    send_cmd_submit(client, 1, 0x01, UsbIpDirection::In, 8);
    // 取消该请求
    send_cmd_unlink(client, 2, 1);

    std::uint32_t ret_seqnum = 0;
    auto status = read_ret_unlink(client, &ret_seqnum);
    EXPECT_EQ(ret_seqnum, 2u); // RET_UNLINK 回的是 unlink 命令自身的 seqnum
    EXPECT_EQ(status, static_cast<std::uint32_t>(UrbStatusType::StatusECONNRESET));

    client.close();
    ASSERT_TRUE(wait_sessions_gone(server));
    server.stop();
}