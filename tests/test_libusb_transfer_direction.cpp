// libusb 后端控制传输方向专项：libusb_fill_control_transfer 把控制传输的
// endpoint 固定为 0，方向只能从 setup 包的 bmRequestType 判断。
// 回归场景：此前 transfer_is_in 用 endpoint 判断，所有控制传输都被当成 OUT，
// RET_SUBMIT 只发 48 字节 header，IN 数据阶段（如 GET_DESCRIPTOR 的描述符）
// 被丢弃，客户端表现为 status=0、actual_length 正常但拿不到数据。

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include <asio.hpp>
#include <libusb.h>

#include "usbipdcpp/DeviceHandler/TransferOperator.h"
#include "usbipdcpp/LibusbHandler/LibusbTransferOperator.h"
#include "usbipdcpp/protocol.h"
#include "test_utils.h"

using namespace usbipdcpp;
using usbipdcpp::test::SocketPair;
using usbipdcpp::test::read_n;
using usbipdcpp::test::expect_header;

namespace {

// bmRequestType 方向位：bit7 = 1 设备到主机（IN）
constexpr std::uint8_t REQUEST_TYPE_IN = 0x80;
constexpr std::uint8_t REQUEST_TYPE_OUT = 0x00;

/// 等接收端出现 n 字节可读数据；超时返回 false（修复前数据阶段缺失时会走到这里）
bool wait_available(asio::ip::tcp::socket &sock, std::size_t n, std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (sock.available() < n) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

/// 把 transfer 配成 libusb_fill_control_transfer 之后的状态：endpoint 恒为 0、
/// 类型为 CONTROL，setup 包写在 buffer 开头的 LIBUSB_CONTROL_SETUP_SIZE 字节
void prepare_control_transfer(void *handle, std::uint8_t bm_request_type, std::uint16_t w_length) {
    auto *trx = static_cast<libusb_transfer *>(handle);
    trx->type = LIBUSB_TRANSFER_TYPE_CONTROL;
    trx->endpoint = 0;
    libusb_fill_control_setup(trx->buffer, bm_request_type, 0x06, 0x0100, 0, w_length);
}

} // namespace

// ============== transfer_is_in 方向判断 ==============

TEST(TestLibusbTransferDirection, ControlUsesSetupPacketNotEndpoint) {
    LibusbTransferOperator op;
    UsbIpHeaderBasic header{};
    header.ep = 0; // 控制端点
    constexpr std::size_t length = 18;

    auto *in_raw = op.alloc_transfer_handle(length, 0, header, {});
    ASSERT_NE(in_raw, nullptr);
    prepare_control_transfer(in_raw, REQUEST_TYPE_IN, length);
    // endpoint 为 0 时仍必须识别出 IN
    EXPECT_TRUE(op.transfer_is_in(in_raw));
    op.free_transfer_handle(in_raw);

    auto *out_raw = op.alloc_transfer_handle(length, 0, header, {});
    ASSERT_NE(out_raw, nullptr);
    prepare_control_transfer(out_raw, REQUEST_TYPE_OUT, length);
    EXPECT_FALSE(op.transfer_is_in(out_raw));
    op.free_transfer_handle(out_raw);
}

TEST(TestLibusbTransferDirection, NonControlStillUsesEndpointAddress) {
    // 非控制传输的端点地址自带方向位，不能因为控制传输修复而改变行为
    LibusbTransferOperator op;
    UsbIpHeaderBasic header{};

    header.ep = 0x81;
    auto *in_raw = op.alloc_transfer_handle(4, 0, header, {});
    ASSERT_NE(in_raw, nullptr);
    static_cast<libusb_transfer *>(in_raw)->type = LIBUSB_TRANSFER_TYPE_BULK;
    static_cast<libusb_transfer *>(in_raw)->endpoint = 0x81;
    EXPECT_TRUE(op.transfer_is_in(in_raw));
    op.free_transfer_handle(in_raw);

    header.ep = 0x01;
    auto *out_raw = op.alloc_transfer_handle(4, 0, header, {});
    ASSERT_NE(out_raw, nullptr);
    static_cast<libusb_transfer *>(out_raw)->type = LIBUSB_TRANSFER_TYPE_BULK;
    static_cast<libusb_transfer *>(out_raw)->endpoint = 0x01;
    EXPECT_FALSE(op.transfer_is_in(out_raw));
    op.free_transfer_handle(out_raw);
}

// ============== RET_SUBMIT 字节流 ==============

TEST(TestLibusbTransferDirection, ControlInRetSubmitSendsDataStage) {
    LibusbTransferOperator op;
    SocketPair sp;
    constexpr std::size_t length = 18;
    // 设备描述符前 18 字节
    const std::vector<std::uint8_t> payload = {0x12, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 0x40, 0x6d,
                                               0x04, 0x5e, 0x08, 0x00, 0x01, 0x01, 0x02, 0x00, 0x01};

    UsbIpHeaderBasic header{};
    header.ep = 0;
    auto *raw = op.alloc_transfer_handle(length, 0, header, {});
    ASSERT_NE(raw, nullptr);
    prepare_control_transfer(raw, REQUEST_TYPE_IN, length);
    auto *trx = static_cast<libusb_transfer *>(raw);
    std::memcpy(trx->buffer + LIBUSB_CONTROL_SETUP_SIZE, payload.data(), payload.size());
    trx->actual_length = length;

    TransferHandle handle(raw, &op);
    auto ret = UsbIpResponse::UsbIpRetSubmit::create_ret_submit(300, 0, length, 0, 0, std::move(handle));
    usbipdcpp::error_code ec;
    ret.to_socket(sp.send_sock, ec);
    ASSERT_FALSE(ec);

    auto hdr = read_n(sp.recv_sock, 48);
    expect_header(hdr, 300, length, 0);
    // 关键断言：修复前 transfer_is_in 误判为 OUT，这 18 字节不会发出
    ASSERT_TRUE(wait_available(sp.recv_sock, length, std::chrono::seconds(1)));
    EXPECT_EQ(read_n(sp.recv_sock, length), payload);
}

TEST(TestLibusbTransferDirection, ControlOutRetSubmitSendsHeaderOnly) {
    // 控制 OUT 的 actual_length 是实际发出的字节数，同样不得回发数据
    LibusbTransferOperator op;
    SocketPair sp;
    constexpr std::size_t length = 8;

    UsbIpHeaderBasic header{};
    header.ep = 0;
    auto *raw = op.alloc_transfer_handle(length, 0, header, {});
    ASSERT_NE(raw, nullptr);
    prepare_control_transfer(raw, REQUEST_TYPE_OUT, length);

    TransferHandle handle(raw, &op);
    auto ret = UsbIpResponse::UsbIpRetSubmit::create_ret_submit(301, 0, length, 0, 0, std::move(handle));
    usbipdcpp::error_code ec;
    ret.to_socket(sp.send_sock, ec);
    ASSERT_FALSE(ec);

    auto hdr = read_n(sp.recv_sock, 48);
    expect_header(hdr, 301, length, 0);
    EXPECT_EQ(sp.recv_sock.available(), 0u);
}
