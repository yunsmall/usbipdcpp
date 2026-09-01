#pragma once

#include <asio.hpp>
#include <gtest/gtest.h>

#include "crash_handler.h"

#include <chrono>
#include <thread>

#include "usbipdcpp/DeviceHandler/TransferOperator.h"
#include "usbipdcpp/Server.h"
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

        [[maybe_unused]] auto version = usbipdcpp::read_u16(server_socket);
        auto op_command = usbipdcpp::read_u16(server_socket);
        received.from_socket(server_socket);
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

// 构造 CMD_SUBMIT（对齐 CmdSubmit::from_socket 的分配：IN/OUT 都分配
// transfer，IN 请求不读数据阶段、OUT 请求带数据）。op 由调用方持有，
// 必须活得比 submit 长（TransferHandle 析构时用它释放）
inline UsbIpCommand::UsbIpCmdSubmit make_cmd_submit(GenericTransferOperator &op, std::uint32_t seqnum,
                                                    std::uint8_t ep_num, std::uint32_t direction,
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
    UsbIpHeaderBasic header{};
    header.direction = direction;
    auto *trx = GenericTransfer::from_handle(op.alloc_transfer_handle(transfer_buffer_length, num_iso, header, setup));
    if (direction == UsbIpDirection::Out && !out_data.empty()) {
        trx->data = out_data;
    }
    TransferHandle handle(trx, &op);
    submit.transfer = std::move(handle);
    return submit;
}

}
}
