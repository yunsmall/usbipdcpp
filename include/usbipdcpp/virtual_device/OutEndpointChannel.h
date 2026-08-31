#pragma once

#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

#include "usbipdcpp/DeviceHandler/TransferOperator.h"
#include "usbipdcpp/Session.h"
#include "usbipdcpp/SetupPacket.h"
#include "usbipdcpp/protocol.h"
#include "usbipdcpp/type.h"

namespace usbipdcpp {

/**
 * @brief OUT 数据通道基类（CRTP 静态分发）：主机 OUT 请求挂起，业务侧取走时应答（NAK 背压）
 *
 * 与 InEndpointChannelBase 对称：In 的请求挂起是「设备没数据可发」，Out 的请求
 * 挂起是「设备没空位可收」（业务侧未消费）。主机 OUT 请求到达后数据留在
 * transfer 里挂起、不回 RET_SUBMIT——主机 URB 挂着，天然停发（对齐内核
 * vudc：gadget 请求耗尽时 OUT 端点 NAK，见 u_serial.c gs_rx_push）。
 * 业务侧 take() 取走时读出数据并回 RET_SUBMIT，空位释放后主机恢复发送。
 *
 * 数据读取走 TransferHandle 持有的 TransferOperator::get_transfer_data，
 * 组件不假设 transfer 内部结构。
 *
 * 单锁即可：阻塞等待（take）时释放锁，请求侧（on_out_request）随时可入队，
 * 不需要 In 通道的双锁（In 双锁是因为写侧等空间时不能占请求队列锁）。
 *
 * 纯通知请求（transfer 为空，如 Pipe 控制 IN 的 setup 透出）：take 只返回
 * setup、不应答，用于把「主机有控制请求待应答」通知给业务侧
 *
 * 典型接入（handler 内）：
 *  - 主机 OUT 回调里调 on_out_request(...)：请求挂起，数据留在 transfer 里
 *  - 业务线程取数据：take()（阻塞，取走时读出数据并应答，空位释放主机恢复发送）
 *    或 try_take()（非阻塞，没数据立即返回 nullopt）
 *  - 生命周期：on_new_connection 时传会话调 on_new_connection(会话)，
 *    on_disconnection 时调 on_disconnection（清挂起请求、唤醒阻塞的 take）
 */
template <typename Derived>
class OutEndpointChannelBase {
public:
    /// take() 返回的一次消费（业务侧拿到数据后自行处理）
    struct Pending {
        std::uint8_t ep;                       // 端点地址（含方向位）；控制请求时为 0
        std::optional<SetupPacket> setup_req;  // 仅控制请求（ep==0）时有效
        data_type data;                        // OUT 数据（纯通知请求为空）
    };

    /**
     * @brief 设置挂起请求上限（0 = 无限，默认）。挂起请求数达到上限后，主机新
     * OUT 请求立即回 EPIPE（设备忙）、不占队列；业务消费腾出空位后才放行。
     * 与 InEndpointChannelBase::set_max_pending_requests 同方向：都限「主机
     * 发来的请求」堆积（设备没空位可收时），非数据缓冲上限
     */
    void set_max_pending_requests(std::size_t max_pending) {
        std::lock_guard lock(mutex_);
        max_pending_ = max_pending;
    }

    /** @brief 当前挂起中（已收未消费）的请求数 */
    std::size_t size() const {
        std::lock_guard lock(mutex_);
        return pending.size();
    }

    /**
     * @brief 挂起队列最旧一个请求的 seqnum（空队列返回 nullopt）
     * @note 供跨通道/跨端点按 USB 全局到达顺序取数据：seqnum 单调递增
     */
    std::optional<std::uint32_t> front_seqnum() const {
        std::lock_guard lock(mutex_);
        if (pending.empty()) {
            return std::nullopt;
        }
        return pending.front().seqnum;
    }

    /**
     * @brief 处理一个主机 OUT 请求（接口 OUT 传输回调里转发，session receiver 线程）
     *
     * 请求挂起、不立即回 RET_SUBMIT——主机 URB 挂着即背压停发；等业务侧
     * take()/try_take() 取走数据时再应答（NAK 背压，对齐内核 u_serial 的 OUT 处理）。
     * @param ep 端点地址（含方向位，如 0x02）
     * @param seqnum 命令序列号
     * @param transfer 本次 OUT 句柄，数据在句柄里，take() 时经 op 读出
     * @param setup_req 仅非标准控制请求（ep==0）透传给业务侧；普通端点 OUT 不用传
     */
    void on_out_request(std::uint8_t ep, std::uint32_t seqnum, TransferHandle transfer,
                        std::optional<SetupPacket> setup_req = std::nullopt) {
        std::lock_guard lock(mutex_);
        if (disconnected) {
            return;  // 断连后不再接收：请求直接释放（会话已关闭，无从应答）
        }
        // 达到上限：设备忙，回 EPIPE 拒绝（请求直接释放，不占挂起队列）
        if (max_pending_ != 0 && pending.size() >= max_pending_) {
            self().reply(seqnum, 0, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
            return;
        }
        pending.emplace_back(PendingRequest{ep, seqnum, setup_req, std::move(transfer)});
        data_cv.notify_one();
    }

    /**
     * @brief 非阻塞取一条已挂起的 OUT 请求（业务线程调）
     * @return 有请求则返回 Pending（含端点与读出数据），并自动应答（主机恢复发送）；
     *         队列空返回 nullopt
     */
    std::optional<Pending> try_take() {
        std::lock_guard lock(mutex_);
        if (pending.empty()) {
            return std::nullopt;
        }
        return take_locked();
    }

    /**
     * @brief 阻塞取一条已挂起的 OUT 请求（业务线程调）
     * @param timeout_ms 等待超时（毫秒）；0 = 无限等
     * @return 有请求返回 Pending（含端点与读出数据，自动应答）；超时或断连返回 nullopt
     */
    std::optional<Pending> take(std::uint32_t timeout_ms = 0) {
        std::unique_lock lock(mutex_);
        while (pending.empty() && !disconnected) {
            if (timeout_ms == 0) {
                data_cv.wait(lock);
            }
            else {
                if (data_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms)) == std::cv_status::timeout) {
                    // 超时时刻数据可能恰好入队：跳出循环走统一判断，避免丢数据
                    break;
                }
            }
        }
        if (pending.empty()) {
            return std::nullopt;
        }
        return take_locked();
    }

    /**
     * @brief 取消一个挂起的 OUT 请求（主机的 UNLINK 命令处理）
     * @return true = 请求确实还在挂起队列里、已取消（应答 -ECONNRESET）；
     *         false = 请求不存在或已应答过（应答 0）
     */
    bool cancel_pending(std::uint32_t unlink_seqnum) {
        std::lock_guard lock(mutex_);
        for (auto it = pending.begin(); it != pending.end(); ++it) {
            if (it->seqnum == unlink_seqnum) {
                pending.erase(it);  // transfer 析构自动释放
                return true;
            }
        }
        return false;
    }

    /**
     * @brief 新连接激活通道：设会话指针并复位断连标记（队列由上次断连清空）。
     * handler 的 on_new_connection 里传当前会话调用；无参（测试桩复位）时保持
     * 已绑定的会话不变
     */
    void on_new_connection(TransferResponder *current_session = nullptr) {
        std::lock_guard lock(mutex_);
        if (current_session) {
            responder = current_session;
        }
        disconnected = false;
    }

    /**
     * @brief 断连：清挂起请求（transfer 析构自动释放）、唤醒阻塞的 take 按断连返回。
     * 之后 take 立即返回 nullopt；连接状态由 handler 管理
     */
    void on_disconnection() {
        {
            std::lock_guard lock(mutex_);
            disconnected = true;
            pending.clear();  // TransferHandle 析构自动释放
        }
        data_cv.notify_all();
    }

protected:
    Derived &self() {
        return static_cast<Derived &>(*this);
    }

    TransferResponder *responder = nullptr;

private:
    struct PendingRequest {
        std::uint8_t ep;
        std::uint32_t seqnum;
        std::optional<SetupPacket> setup_req;
        TransferHandle transfer;  // 数据载体（OUT 数据已读入）；纯通知请求为空
    };

    // 调用者必须已持有 mutex_
    std::optional<Pending> take_locked() {
        auto req = std::move(pending.front());
        pending.pop_front();
        if (req.transfer) {
            // 有数据：读出并应答（数据消费完成 = 空位释放，主机恢复发送）
            assert(req.transfer.get_operator() != nullptr);
            bool supported = false;
            data_type data;
            auto len = req.transfer.get_operator()->get_transfer_data(req.transfer.get(), data, supported);
            assert(supported);  // 挂起-消费的 op 必须支持读数据
            self().reply(req.seqnum, len);
            return Pending{req.ep, req.setup_req, std::move(data)};
        }
        // 纯通知（控制 IN 的 setup 透出）：不应答，只把 setup 交给业务侧
        return Pending{req.ep, req.setup_req, {}};
    }

    mutable std::mutex mutex_;
    std::condition_variable data_cv;  // 挂起队列非空或断连
    std::deque<PendingRequest> pending;
    std::size_t max_pending_ = 0;  // 0 = 无限
    bool disconnected = true;      // 初始为断连，on_new_connection 后可用
};

/**
 * @brief OUT 数据通道默认实现
 *
 * 派生类需实现 reply()（CRTP 接口，应答一个请求）：通常走
 * responder->submit_ret_submit 回 RET_SUBMIT（status=0：OUT 数据消费完成；
 * 非 0：如上限拒绝的 EPIPE）。测试可继承 OutEndpointChannelBase 提供自己的
 * reply() 记录应答，不产生虚函数开销
 */
class OutEndpointChannel : public OutEndpointChannelBase<OutEndpointChannel> {
public:
    /**
     * @brief 应答一个挂起的请求（本通道的 CRTP 实现，内部调用，无需直接使用）
     * @param seqnum 对应请求的序列号
     * @param length 已消费的数据长度（status==0 时主机以为本次大小）
     * @param status 0 = 正常完成；非 0 = 错误状态（如上限拒绝的 EPIPE，长度填 0）
     */
    void reply(std::uint32_t seqnum, std::uint32_t length, std::uint32_t status = 0) {
        if (status == 0) {
            responder->submit_ret_submit(
                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(seqnum, length));
        }
        else {
            // 错误状态应答：actual_length 填 0（协议上错误传输不带有效数据）
            responder->submit_ret_submit(
                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(seqnum, status, 0));
        }
    }
};

} // namespace usbipdcpp
