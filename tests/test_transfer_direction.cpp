// RET_SUBMIT / CMD_SUBMIT 发送字节流的行为测试：方向（IN/OUT）× 传输类型
// （bulk/iso）× actual_length 组合下，协议层按 transfer_is_in 计算的 length
// 是否正确——核心回归：iso OUT 应答只发 iso 描述符、不得把已收数据当回发内容
// （修复前 GenericTransferOperator 按 length>0 发数据，被 vhci 误读为描述符）

#include <gtest/gtest.h>

#include <cstdint>
#include <thread>
#include <vector>

#include <asio.hpp>

#include "usbipdcpp/DeviceHandler/TransferOperator.h"
#include "usbipdcpp/protocol.h"

using namespace usbipdcpp;

namespace {

/// 本地 TCP 回环 socket 对：send_sock 由 to_socket 写入，recv_sock 读端验证
struct SocketPair {
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor;
    asio::ip::tcp::socket send_sock;
    asio::ip::tcp::socket recv_sock;

    SocketPair()
        : acceptor(io, asio::ip::tcp::endpoint(asio::ip::tcp::v4(), 0)),
          send_sock(io),
          recv_sock(io) {
        auto ep = acceptor.local_endpoint();
        acceptor.listen(1);
        std::thread accept_thread([this] { acceptor.accept(recv_sock); });
        send_sock.connect(ep);
        accept_thread.join();
    }
};

std::vector<std::uint8_t> read_n(asio::ip::tcp::socket &sock, std::size_t n) {
    std::vector<std::uint8_t> buf(n);
    asio::read(sock, asio::buffer(buf));
    return buf;
}

std::uint32_t be32(const std::vector<std::uint8_t> &bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
}

/// 模拟真实应答路径构造 RET：经 alloc_transfer_handle（记录方向）→ 填数据 →
/// create_ret_submit。payload 为数据阶段内容，iso_actual 为各包实际字节数
UsbIpResponse::UsbIpRetSubmit make_ret(GenericTransferOperator &op, bool is_in, std::uint32_t seqnum,
                                       std::uint32_t actual_length, int num_iso,
                                       const std::vector<std::uint8_t> &payload,
                                       const std::vector<std::uint32_t> &iso_actual = {}) {
    UsbIpHeaderBasic header{};
    header.direction = is_in ? UsbIpDirection::In : UsbIpDirection::Out;
    auto *raw = op.alloc_transfer_handle(payload.size(), num_iso, header, {});
    auto *trx = GenericTransfer::from_handle(raw);
    trx->data = payload;
    trx->actual_length = payload.size();
    std::uint32_t offset = 0;
    for (int i = 0; i < num_iso; ++i) {
        auto actual = i < static_cast<int>(iso_actual.size()) ? iso_actual[i] : 0;
        trx->iso_descriptors[i] = {.offset = offset, .length = 96, .actual_length = actual, .status = 0};
        offset += 96;
    }
    TransferHandle handle(raw, &op);
    return UsbIpResponse::UsbIpRetSubmit::create_ret_submit(seqnum, 0, actual_length, 0, num_iso,
                                                            std::move(handle));
}

/// 验证 48 字节 usbip_header（大端布局见协议文档：base 20 字节 + ret_submit 20 字节 + padding 8）
void expect_header(const std::vector<std::uint8_t> &hdr, std::uint32_t seqnum, std::uint32_t actual_length,
                   std::uint32_t num_packets) {
    ASSERT_EQ(hdr.size(), 48u);
    EXPECT_EQ(be32(hdr, 0), USBIP_RET_SUBMIT);
    EXPECT_EQ(be32(hdr, 4), seqnum);
    EXPECT_EQ(be32(hdr, 20), 0u); // status
    EXPECT_EQ(be32(hdr, 24), actual_length);
    EXPECT_EQ(be32(hdr, 32), num_packets);
}

} // namespace

// ============== alloc 方向记录 ==============

TEST(TestTransferDirection, AllocRecordsDirection) {
    GenericTransferOperator op;
    UsbIpHeaderBasic header{};

    header.direction = UsbIpDirection::In;
    auto *in_handle = op.alloc_transfer_handle(64, 0, header, {});
    EXPECT_TRUE(op.transfer_is_in(in_handle));
    op.free_transfer_handle(in_handle);

    header.direction = UsbIpDirection::Out;
    auto *out_handle = op.alloc_transfer_handle(64, 0, header, {});
    EXPECT_FALSE(op.transfer_is_in(out_handle));
    op.free_transfer_handle(out_handle);
}

// ============== RET_SUBMIT 字节流 ==============

TEST(TestTransferDirection, RetSubmitNonIsoInSendsData) {
    GenericTransferOperator op;
    SocketPair sp;
    const std::vector<std::uint8_t> payload = {1, 2, 3, 4, 5};
    auto ret = make_ret(op, /*is_in=*/true, 100, 5, 0, payload);
    usbipdcpp::error_code ec;
    ret.to_socket(sp.send_sock, ec);
    ASSERT_FALSE(ec);

    auto hdr = read_n(sp.recv_sock, 48);
    expect_header(hdr, 100, 5, 0);
    // IN：数据回发，内容与 payload 一致
    auto data = read_n(sp.recv_sock, 5);
    EXPECT_EQ(data, payload);
    EXPECT_EQ(sp.recv_sock.available(), 0u);
}

TEST(TestTransferDirection, RetSubmitNonIsoOutWithoutTransferSendsHeaderOnly) {
    // 现有 OUT 设备的做法：不带 transfer 的应答（create_ret_submit_ok_without_data），
    // 只发 header——actual_length 如实上报接收字节数，数据不回发
    SocketPair sp;
    auto ret = UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(100, 64);
    usbipdcpp::error_code ec;
    ret.to_socket(sp.send_sock, ec);
    ASSERT_FALSE(ec);

    auto hdr = read_n(sp.recv_sock, 48);
    expect_header(hdr, 100, 64, 0);
    EXPECT_EQ(sp.recv_sock.available(), 0u);
}

TEST(TestTransferDirection, RetSubmitIsoInSendsDataAndDescriptors) {
    GenericTransferOperator op;
    SocketPair sp;
    // 2 个 iso 包，每包 96 字节槽位，实际收 96 + 48
    const std::vector<std::uint8_t> payload(144, 0x7A);
    auto ret = make_ret(op, /*is_in=*/true, 101, 144, 2, payload, {96, 48});
    usbipdcpp::error_code ec;
    ret.to_socket(sp.send_sock, ec);
    ASSERT_FALSE(ec);

    auto hdr = read_n(sp.recv_sock, 48);
    expect_header(hdr, 101, 144, 2);
    // IN：数据（紧凑，各包实际字节）先于描述符
    auto data = read_n(sp.recv_sock, 144);
    EXPECT_EQ(data, payload);
    auto desc = read_n(sp.recv_sock, 2 * 16);
    EXPECT_EQ(be32(desc, 0), 0u);   // 包 0 offset
    EXPECT_EQ(be32(desc, 4), 96u);  // 包 0 length
    EXPECT_EQ(be32(desc, 8), 96u);  // 包 0 actual_length
    EXPECT_EQ(be32(desc, 16), 96u); // 包 1 offset
    EXPECT_EQ(be32(desc, 20), 96u); // 包 1 length
    EXPECT_EQ(be32(desc, 24), 48u); // 包 1 actual_length
    EXPECT_EQ(sp.recv_sock.available(), 0u);
}

TEST(TestTransferDirection, RetSubmitIsoOutSendsDescriptorsOnly) {
    // 核心回归：iso OUT 应答（扬声器场景）。actual_length 如实填 576（vhci
    // 校验 sum(iso actual) == actual_length），但数据不得回发——修复前
    // to_socket 传 actual_length 导致 576 字节数据被 vhci 误读成描述符
    GenericTransferOperator op;
    SocketPair sp;
    const std::vector<std::uint8_t> payload(576, 0x5A); // 主机发来的 PCM（应被丢弃，不回发）
    auto ret = make_ret(op, /*is_in=*/false, 102, 576, 6, payload, {96, 96, 96, 96, 96, 96});
    usbipdcpp::error_code ec;
    ret.to_socket(sp.send_sock, ec);
    ASSERT_FALSE(ec);

    auto hdr = read_n(sp.recv_sock, 48);
    expect_header(hdr, 102, 576, 6);
    // OUT：无数据阶段，只有 6 个描述符
    auto desc = read_n(sp.recv_sock, 6 * 16);
    for (int i = 0; i < 6; ++i) {
        EXPECT_EQ(be32(desc, static_cast<std::size_t>(i) * 16), static_cast<std::uint32_t>(i * 96));
        EXPECT_EQ(be32(desc, static_cast<std::size_t>(i) * 16 + 4), 96u);
        EXPECT_EQ(be32(desc, static_cast<std::size_t>(i) * 16 + 8), 96u);
    }
    // 关键断言：没有多余字节（修复前这里会读到 576 字节 PCM）
    EXPECT_EQ(sp.recv_sock.available(), 0u);
}

// ============== CMD_SUBMIT 数据阶段方向 ==============

TEST(TestTransferDirection, CmdSubmitOutSendsDataInSendsNone) {
    // CMD_SUBMIT 的数据阶段只在 OUT 携带（与 from_socket 对称）；IN 恒传 0，
    // 防止 IN 的缓冲内容被误发
    GenericTransferOperator op;

    // OUT：发 transfer_buffer_length 字节
    {
        SocketPair sp;
        UsbIpHeaderBasic header{};
        header.direction = UsbIpDirection::Out;
        auto *raw = op.alloc_transfer_handle(4, 0, header, {});
        auto *trx = GenericTransfer::from_handle(raw);
        trx->data = {9, 8, 7, 6};
        TransferHandle handle(raw, &op);
        UsbIpCommand::UsbIpCmdSubmit submit{};
        submit.header.command = USBIP_CMD_SUBMIT;
        submit.header.seqnum = 200;
        submit.header.direction = UsbIpDirection::Out;
        submit.transfer_buffer_length = 4;
        submit.transfer = std::move(handle);
        usbipdcpp::error_code ec;
        submit.to_socket(sp.send_sock, ec);
        ASSERT_FALSE(ec);
        auto hdr = read_n(sp.recv_sock, 48);
        EXPECT_EQ(be32(hdr, 0), USBIP_CMD_SUBMIT);
        auto data = read_n(sp.recv_sock, 4);
        EXPECT_EQ(data, (std::vector<std::uint8_t>{9, 8, 7, 6}));
    }

    // IN（违反"IN 不带 transfer"约定）：也不得发数据（transfer_is_in 兜底）
    {
        SocketPair sp;
        UsbIpHeaderBasic header{};
        header.direction = UsbIpDirection::In;
        auto *raw = op.alloc_transfer_handle(4, 0, header, {});
        auto *trx = GenericTransfer::from_handle(raw);
        trx->data = {1, 1, 1, 1}; // 缓冲里的内容不得被发出
        TransferHandle handle(raw, &op);
        UsbIpCommand::UsbIpCmdSubmit submit{};
        submit.header.command = USBIP_CMD_SUBMIT;
        submit.header.seqnum = 201;
        submit.header.direction = UsbIpDirection::In;
        submit.transfer_buffer_length = 4;
        submit.transfer = std::move(handle);
        usbipdcpp::error_code ec;
        submit.to_socket(sp.send_sock, ec);
        ASSERT_FALSE(ec);
        auto hdr = read_n(sp.recv_sock, 48);
        EXPECT_EQ(be32(hdr, 0), USBIP_CMD_SUBMIT);
        EXPECT_EQ(sp.recv_sock.available(), 0u);
    }
}
