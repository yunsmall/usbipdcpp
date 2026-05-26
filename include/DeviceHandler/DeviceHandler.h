#pragma once

#include <vector>
#include <cstdint>
#include <optional>
#include <system_error>
#include <mutex>
#include <spdlog/spdlog.h>

#include <asio.hpp>

#include "Device.h"
#include "type.h"
#include "protocol.h"
#include "DeviceHandler/TransferOperator.h"


namespace usbipdcpp {
struct UsbEndpoint;
struct UsbInterface;
struct SetupPacket;

class Session;

/**
 * @brief USB 设备处理抽象基类。
 *
 * 每个导入的设备对应一个 DeviceHandler 实例。传输数据的读写通过
 * TransferOperator 完成，由 get_transfer_operator() 获取。
 * 默认使用 GenericTransferOperator，子类可替换。
 */
class USBIPDCPP_API AbstDeviceHandler {
public:
    explicit AbstDeviceHandler(UsbDevice &handle_device, std::unique_ptr<TransferOperator> op = nullptr) :
        handle_device(handle_device),
        transfer_op_(op ? std::move(op) : std::make_unique<GenericTransferOperator>()) {
    }

    AbstDeviceHandler(AbstDeviceHandler &&other) noexcept;

    /**
     * @brief 处理 URB 请求的统一入口
     * @param cmd 完整的 CMD_SUBMIT 命令
     * @param ep 端点信息
     * @param interface 可选的接口信息（非控制传输必须有）
     * @param ec 错误码
     */
    virtual void receive_urb(
            UsbIpCommand::UsbIpCmdSubmit cmd,
            UsbEndpoint ep,
            std::optional<UsbInterface> interface,
            usbipdcpp::error_code &ec
            ) = 0;

    /**
     * @brief 新的客户端连接时会调这个函数，可以阻塞。子类实现时请在函数开头调用这个函数
     * @param current_session 请自行储存通信用的session
     * @param ec 发生的ec
     */
    virtual void on_new_connection(Session &current_session, error_code &ec) {
        std::lock_guard lock(session_mutex_);
        session = &current_session;
    }

    /**
     * @brief 当发生错误等情况需要完全终止传输时会调用这个函数。被调用后禁止再提交消息和使用Session对象\n
     * 可以阻塞，处理所有需要处理的事务。子类实现时请在函数末尾调用这个函数
     */
    virtual void on_disconnection(error_code &ec) {
        std::lock_guard lock(session_mutex_);
        session = nullptr;
    }

    /**
     * @brief 检查设备是否已被移除
     * @return true 表示设备已物理拔出
     */
    virtual bool is_device_removed() const {
        return false;  // 默认实现
    }

    /**
     * @brief 设备被物理移除时调用
     */
    virtual void on_device_removed() {}

    /**
     * @brief 线程安全地停止 Session
     */
    void trigger_session_stop();

    /**
     * @brief 处理 USBIP_CMD_UNLINK（客户端要求取消一个未完成的传输）。
     *
     * 协议约定：
     * - 每个 CMD_UNLINK 必须对应一个 RET_UNLINK，status 为 USBIP 错误码。
     * - 如果目标传输已经正常完成，RET_UNLINK 的 status 必须为 0（URB 已完成）。
     * - RET_SUBMIT 必须在 RET_UNLINK 之前发送。客户端先收到 URB 的实际结果，
     *   再收到 unlink 的确认，不能反过来。如果实现中同一个传输可能既发 RET_SUBMIT
     *   又发 RET_UNLINK，必须保证 RET_SUBMIT 先于 RET_UNLINK 到达客户端。
     *
     * @param unlink_seqnum 要取消的 CMD_SUBMIT 的 seqnum
     * @param cmd_seqnum    CMD_UNLINK 自己的 seqnum（用在 RET_UNLINK 里原样返回）
     *
     * 目标传输在 unlink 到达时可能处于以下几种状态，实现需要分别处理：
     *
     * 状态 1 —— 传输尚未开始或仍在进行中：
     *   取消该传输。如果取消成功，传输完成时发送 RET_UNLINK(cmd_seqnum, -ECONNRESET)
     *   （表示 URB 被取消），并且不再发送 RET_SUBMIT。
     *
     * 状态 2 —— 传输已经完成，但 RET_SUBMIT 还未发送：
     *   发送 RET_UNLINK(cmd_seqnum, 实际完成状态码)，如无错误则为 0。
     *   不再发送 RET_SUBMIT。
     *
     * 状态 3 —— RET_SUBMIT 已经发送：
     *   直接发送 RET_UNLINK(cmd_seqnum, 0)。RET_SUBMIT 已先行发送，顺序正确。
     *
     * 状态 4 —— 传输已完全处理完毕（RET_SUBMIT 及可能的 RET_UNLINK 均已发出）：
     *   直接发送 RET_UNLINK(cmd_seqnum, 0)。
     */
    virtual void handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) = 0;

    // ========== TransferOperator 接口 ==========

    /**
     * @brief 获取传输操作器（子类可重写以替换默认实现）
     */
    TransferOperator* get_transfer_operator() const {
        return transfer_op_.get();
    }

    /**
     * @brief 替换传输操作器（所有权转移）
     */
    void set_transfer_operator(std::unique_ptr<TransferOperator> op) {
        transfer_op_ = std::move(op);
    }

    /**
     * @brief 查询 handle 是否使用自定义 I/O 路径（send/recv_transfer_data）
     */
    bool is_custom_transfer_io(void* handle) const {
        return get_transfer_operator()->is_custom_io(handle);
    }

    virtual ~AbstDeviceHandler() = default;

protected:
    UsbDevice &handle_device;
    Session *session = nullptr;
    mutable std::mutex session_mutex_;
    std::unique_ptr<TransferOperator> transfer_op_ = std::make_unique<GenericTransferOperator>();
};
}
