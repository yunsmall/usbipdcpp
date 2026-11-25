#pragma once

#include <asio.hpp>
#include <gtest/gtest.h>

#include "crash_handler.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <thread>

#include "usbipdcpp/DeviceHandler/TransferOperator.h"
#include "usbipdcpp/Server.h"
#include "usbipdcpp/TransferSource.h"
#include "usbipdcpp/network.h"
#include "usbipdcpp/protocol.h"
#include "usbipdcpp/type.h"
#include "usbipdcpp/utils/utils.h"
#include "usbipdcpp/virtual_device/TransferResponder.h"

namespace usbipdcpp {
namespace test {

// 测试专用比较函数
inline void expect_header_equal(const UsbIpHeaderBasic &actual, const UsbIpHeaderBasic &expected) {
    EXPECT_EQ(actual.command, expected.command);
    EXPECT_EQ(actual.seqnum, expected.seqnum);
    EXPECT_EQ(actual.devid, expected.devid);
    EXPECT_EQ(actual.direction, expected.direction);
    EXPECT_EQ(actual.ep, expected.ep);
}

inline void expect_cmd_unlink_equal(const UsbIpCommand::UsbIpCmdUnlink &actual,
                                     const UsbIpCommand::UsbIpCmdUnlink &expected) {
    expect_header_equal(actual.header, expected.header);
    EXPECT_EQ(actual.unlink_seqnum, expected.unlink_seqnum);
}

    template<typename T>
    concept with_header = requires(T &&t)
    {
        std::forward<T>(t).header;
    };

// ---- 网络测试公共工具 ----

/// 本地 TCP 回环 socket 对：send_sock 由 to_socket 写入，recv_sock 读端验证
struct SocketPair {
    asio::io_context io;
    asio::ip::tcp::acceptor acceptor;
    asio::ip::tcp::socket send_sock;
    asio::ip::tcp::socket recv_sock;

    SocketPair()
        : acceptor(io, asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)),
          send_sock(io),
          recv_sock(io) {
        auto ep = acceptor.local_endpoint();
        acceptor.listen(1);
        std::thread accept_thread([this] { acceptor.accept(recv_sock); });
        send_sock.connect(ep);
        accept_thread.join();
    }
};

inline std::vector<std::uint8_t> read_n(asio::ip::tcp::socket &sock, std::size_t n) {
    std::vector<std::uint8_t> buf(n);
    asio::read(sock, asio::buffer(buf));
    return buf;
}

inline std::uint32_t be32(const std::vector<std::uint8_t> &bytes, std::size_t offset) {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24) |
           (static_cast<std::uint32_t>(bytes[offset + 1]) << 16) |
           (static_cast<std::uint32_t>(bytes[offset + 2]) << 8) |
           static_cast<std::uint32_t>(bytes[offset + 3]);
}

/// 验证 48 字节 usbip_header（大端布局：base 20 字节 + ret_submit 20 字节 + padding 8）
inline void expect_header(const std::vector<std::uint8_t> &hdr, std::uint32_t seqnum, std::uint32_t actual_length,
                          std::uint32_t num_packets) {
    ASSERT_EQ(hdr.size(), 48u);
    EXPECT_EQ(be32(hdr, 0), USBIP_RET_SUBMIT);
    EXPECT_EQ(be32(hdr, 4), seqnum);
    EXPECT_EQ(be32(hdr, 20), 0u); // status
    EXPECT_EQ(be32(hdr, 24), actual_length);
    EXPECT_EQ(be32(hdr, 32), num_packets);
}

// 探测一个空闲端口。固定端口才能验证多次 start 的监听不冲突
inline std::uint16_t probe_free_port(asio::io_context &io) {
    asio::ip::tcp::acceptor probe(io);
    probe.open(asio::ip::tcp::v4());
    probe.bind(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    return probe.local_endpoint().port();
}

// 连接服务器，轮询等待监听就绪。失败时重建 socket 重试，返回是否连接成功
inline bool connect_with_retry(asio::ip::tcp::socket &client, const asio::ip::tcp::endpoint &ep) {
    for (int i = 0; i < 200; i++) {
        std::error_code ec;
        client.connect(ep, ec);
        if (!ec) {
            return true;
        }
        client.close();
        client = asio::ip::tcp::socket(client.get_executor());
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

// 以 RST 方式断开连接（linger=0 时不发送 FIN）：模拟客户端异常掉线，
// 服务器侧读到的是连接重置错误而不是 EOF，走不同的错误分支
inline void rst_disconnect(asio::ip::tcp::socket &client) {
    std::error_code ec;
    client.set_option(asio::socket_base::linger(true, 0), ec);
    client.close();
}

// 轮询等待服务器清理完所有已断开的连接：客户端断开后 session 线程收尾
// 并移除自身（get_session_count 返回存活会话数），归零即清理完成。连接若还
// 停留在 accept 队列中（未创建 session）计数同样为 0，此时提前返回无碍——
// 后续 stop() 的活跃会话计数等待（含所有已创建 session）会兜底等干净
inline bool wait_sessions_gone(Server &server, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (server.get_session_count() == 0) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return server.get_session_count() == 0;
}

// 发送 import 请求并读取回复，返回回复中的 status；任何错误返回最大值
inline std::uint32_t import_device(asio::ip::tcp::socket &client, const std::string &busid) {
    UsbIpCommand::OpReqImport req{.status = 0, .busid = {}};
    std::copy(busid.begin(), busid.end(), req.busid.begin());
    usbipdcpp::error_code send_ec;
    req.to_socket(client, send_ec);
    if (send_ec) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    std::uint16_t version = 0;
    std::uint16_t command = 0;
    usbipdcpp::error_code read_ec;
    data_read_from_socket(client, read_ec, version, command);
    if (read_ec) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    if (command != OP_REP_IMPORT) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    // 成功响应还带设备描述符（312 字节结构），必须一起读掉，否则后续
    // 读 RET_SUBMIT 会先读到这段残留
    UsbIpResponse::OpRepImport rep;
    rep.from_socket(client, read_ec);
    if (read_ec) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    return rep.status;
}

    template<usbipdcpp::Serializable T>
    T reread_from_socket_with_command(const T &origin, std::uint16_t cmd) {
        asio::io_context io_context;
        asio::ip::tcp::acceptor acceptor(io_context);
        auto server_endpoint = asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0);
        acceptor.open(server_endpoint.protocol());

        acceptor.bind(server_endpoint);
        acceptor.listen();

        auto server_port = acceptor.local_endpoint().port();

        std::thread sender([&]() {
            auto sock = acceptor.accept();
            usbipdcpp::data_type buffer;
            // 发送版本号 + 命令码
            usbipdcpp::vector_append_to_net(buffer, static_cast<std::uint16_t>(USBIP_VERSION));
            usbipdcpp::vector_append_to_net(buffer, (std::uint16_t) cmd);
            auto data = origin.to_bytes();
            sock.send(asio::buffer(data));
        });

        T received{};
        asio::ip::tcp::socket server_socket(io_context);
        asio::error_code ec;
        server_socket.connect(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), server_port), ec);

        [[maybe_unused]] auto version = usbipdcpp::read_u16(server_socket, ec);
        auto op_command = usbipdcpp::read_u16(server_socket, ec);
        received.from_socket(server_socket, ec);
        EXPECT_FALSE(ec);
        // SPDLOG_INFO("Received header from server");
        if constexpr (with_header<T>) {
            received.header.command = op_command;
        }
        else {
            received.command = op_command;
        }
        server_socket.close();

        sender.join();

        return received;
    }

// ---- 虚拟设备数据面测试公共工具 ----

// 记录型 TransferResponder 桩：捕获所有提交的应答，测试直接断言内容。
// 替换真实 Session 后，handler/通道/调度器的数据面可脱离网络测试
//
// 生命周期：submits 里的 RetSubmit 持 TransferHandle（绑定 make_cmd_submit
// 的 op），析构本桩时 handle 析构要调用 op 释放。因此 op 必须活得比桩久：
// 测试里把 op 放在 stub 之前声明，或做成 fixture 成员（fixture 先于 stub
// 声明、逆序析构时后死）——否则 clang ASan 报 stack-use-after-scope
class CaptureResponder : public TransferResponder {
public:
    void submit_ret_submit(UsbIpResponse::UsbIpRetSubmit &&submit) override {
        submits.push_back(std::move(submit));
    }
    void submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink &&unlink) override {
        unlinks.push_back(std::move(unlink));
    }
    void enqueue_ret_submit(UsbIpResponse::UsbIpRetSubmit &&submit) override {
        enqueued_submits.push_back(std::move(submit));
    }
    void enqueue_ret_unlink(UsbIpResponse::UsbIpRetUnlink &&unlink) override {
        enqueued_unlinks.push_back(std::move(unlink));
    }
    void wakeup_sender() override {
        ++wakeup_count;
    }
    void stop_transfer() override {
        stopped = true;
    }

    std::vector<UsbIpResponse::UsbIpRetSubmit> submits;
    std::vector<UsbIpResponse::UsbIpRetUnlink> unlinks;
    std::vector<UsbIpResponse::UsbIpRetSubmit> enqueued_submits;
    std::vector<UsbIpResponse::UsbIpRetUnlink> enqueued_unlinks;
    int wakeup_count = 0;
    bool stopped = false;
};

// 从 RET_SUBMIT 取数据阶段（未接管 transfer 时为空，如 OUT 应答）
inline data_type ret_submit_data(const UsbIpResponse::UsbIpRetSubmit &ret) {
    if (auto *trx = GenericTransfer::from_handle(ret.transfer.get())) {
        return trx->data;
    }
    return {};
}

// 从字节流偏移处读小端整数（测试断言线格式数据字段用）
inline std::uint32_t read_le32_at(const data_type &d, std::size_t offset) {
    std::uint32_t v{};
    std::memcpy(&v, d.data() + offset, sizeof(v));
    return v;
}

inline std::uint16_t read_le16_at(const data_type &d, std::size_t offset) {
    std::uint16_t v{};
    std::memcpy(&v, d.data() + offset, sizeof(v));
    return v;
}

// 内存版 TransferSource：测试里把命令的 payload 当字节源喂给 handler
class MemoryTransferSource : public TransferSource {
public:
    MemoryTransferSource(data_type payload, std::vector<UsbIpIsoPacketDescriptor> iso = {}) :
        payload_(std::move(payload)), iso_(std::move(iso)) {
    }

    [[nodiscard]] std::size_t remaining() const noexcept override { return payload_.size() - pos_; }
    [[nodiscard]] int iso_packets_remaining() const noexcept override {
        return static_cast<int>(iso_.size()) - iso_pos_;
    }

    std::size_t read(std::span<std::byte> dst, std::error_code &ec) override {
        (void) ec;
        auto n = std::min(dst.size(), remaining());
        std::memcpy(dst.data(), payload_.data() + pos_, n);
        pos_ += n;
        return n;
    }

    std::size_t read_some(std::span<std::byte> dst, std::error_code &ec) override { return read(dst, ec); }

    void read_iso_descriptors(std::span<UsbIpIsoPacketDescriptor> out, std::error_code &ec) override {
        (void) ec;
        auto n = std::min(out.size(), iso_.size() - iso_pos_);
        std::copy_n(iso_.begin() + static_cast<std::ptrdiff_t>(iso_pos_), n, out.begin());
        iso_pos_ += n;
    }

    void skip_remaining(std::error_code &ec) override {
        (void) ec;
        pos_ = payload_.size();
    }

    [[nodiscard]] asio::ip::tcp::socket *socket() noexcept override { return nullptr; }

    void mark_consumed(std::size_t n) noexcept override { pos_ += std::min(n, remaining()); }

private:
    data_type payload_;
    std::size_t pos_ = 0;
    std::vector<UsbIpIsoPacketDescriptor> iso_;
    std::size_t iso_pos_ = 0;
};

// 构造 CMD_SUBMIT 的固定部分与 payload（客户端发送 / 直接喂 handler 用）。
// OUT 方向把 out_data 放进 cmd.data；等时描述符由调用方填 cmd.iso_descriptors
inline UsbIpCommand::UsbIpCmdSubmit make_cmd_submit(std::uint32_t seqnum, std::uint8_t ep_num,
                                                    std::uint32_t direction,
                                                    std::uint32_t transfer_buffer_length,
                                                    const SetupPacket &setup = {}, const data_type &out_data = {},
                                                    int num_iso = 0) {
    UsbIpCommand::UsbIpCmdSubmit submit{};
    submit.header.command = USBIP_CMD_SUBMIT;
    submit.header.seqnum = seqnum;
    submit.header.devid = 1;
    submit.header.direction = direction;
    submit.header.ep = ep_num; // 线格式端点号（不带方向位，方向由 direction 字段给出）
    submit.transfer_flags = 0;
    submit.transfer_buffer_length = transfer_buffer_length;
    submit.start_frame = 0;
    submit.number_of_packets = num_iso;
    submit.interval = 0;
    submit.setup = setup;
    if (direction == UsbIpDirection::Out) {
        submit.data = out_data;
    }
    return submit;
}

// 为接口 handler 直接构造 TransferHandle（模拟 receive_urb 里 alloc + 读数据）
inline TransferHandle make_transfer(GenericTransferOperator &op, std::uint32_t direction,
                                    std::uint32_t transfer_buffer_length, const SetupPacket &setup = {},
                                    const data_type &out_data = {}, int num_iso = 0) {
    UsbIpHeaderBasic header{};
    header.direction = direction;
    auto *trx = GenericTransfer::from_handle(op.alloc_transfer_handle(transfer_buffer_length, num_iso, header, setup));
    if (direction == UsbIpDirection::Out && !out_data.empty()) {
        trx->data = out_data;
    }
    return TransferHandle(trx, &op);
}

// 把一条 CMD_SUBMIT 连同它的 payload 交给设备 handler（设备层入口测试用）
template<typename HandlerPtr>
void deliver_urb(HandlerPtr handler, UsbIpCommand::UsbIpCmdSubmit cmd, UsbEndpoint ep,
                 std::optional<UsbInterface> interface, usbipdcpp::error_code &ec) {
    MemoryTransferSource source(cmd.data, cmd.iso_descriptors);
    handler->receive_urb(std::move(cmd), source, std::move(ep), std::move(interface), ec);
}

}
}
