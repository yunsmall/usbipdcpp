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
 *  - 生命周期：on_new_connection 时调 bind_session + on_new_connection，
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

    /// 绑定会话（handler 的 on_new_connection 时调用）
    void bind_session(Session *current_session) {
        session = current_session;
    }

    /// 设置挂起上限（0 = 无限，默认）；达到上限后新请求回 EPIPE（设备忙）
    void set_max_pending(std::size_t max_pending) {
        std::lock_guard lock(mutex_);
        max_pending_ = max_pending;
    }

    /// 挂起中的请求数
    std::size_t size() const {
        std::lock_guard lock(mutex_);
        return pending.size();
    }

    /// 挂起队列最旧请求的 seqnum（无请求返回 nullopt）
    /// 供跨端点排序（read 按全局到达顺序取：seqnum 单调递增）
    std::optional<std::uint32_t> front_seqnum() const {
        std::lock_guard lock(mutex_);
        if (pending.empty()) {
            return std::nullopt;
        }
        return pending.front().seqnum;
    }

    /**
     * @brief 处理一个 OUT 请求（handler 的 OUT 回调里调用）
     *
     * 请求挂起（数据在 transfer 里），等业务侧 take()。setup 仅控制请求
     * （ep==0）传入；纯通知请求可不带 transfer（take 只返回 setup 不应答）
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

    /// 非阻塞取一条；无数据返回 nullopt
    std::optional<Pending> try_take() {
        std::lock_guard lock(mutex_);
        if (pending.empty()) {
            return std::nullopt;
        }
        return take_locked();
    }

    /**
     * @brief 阻塞取一条。timeout_ms=0 无限等；断连立即返回 nullopt
     * @return 有数据返回 Pending；超时或断连返回 nullopt
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

    /// 取消挂起的请求（UNLINK 处理）
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

    /// 新连接：从干净状态开始（断连标记清除；队列由上次断连清空）
    void on_new_connection() {
        std::lock_guard lock(mutex_);
        disconnected = false;
    }

    /// 断连：清挂起请求，唤醒阻塞的消费者让它们按断连返回
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

    Session *session = nullptr;

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
 * session->submit_ret_submit 回 RET_SUBMIT（status=0：OUT 数据消费完成；
 * 非 0：如上限拒绝的 EPIPE）。测试可继承 OutEndpointChannelBase 提供自己的
 * reply() 记录应答，不产生虚函数开销
 */
class OutEndpointChannel : public OutEndpointChannelBase<OutEndpointChannel> {
public:
    void reply(std::uint32_t seqnum, std::uint32_t length, std::uint32_t status = 0) {
        if (status == 0) {
            session->submit_ret_submit(
                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(seqnum, length));
        }
        else {
            // 错误状态应答：actual_length 填 0（协议上错误传输不带有效数据）
            session->submit_ret_submit(
                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(seqnum, status, 0));
        }
    }
};

} // namespace usbipdcpp
