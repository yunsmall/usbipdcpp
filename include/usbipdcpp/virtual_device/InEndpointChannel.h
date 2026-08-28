#pragma once

#include <cassert>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "usbipdcpp/DeviceHandler/TransferOperator.h"
#include "usbipdcpp/Session.h"
#include "usbipdcpp/protocol.h"
#include "usbipdcpp/utils/RingBuffer.h"

namespace usbipdcpp {

/**
 * @brief 端点请求队列，按端点地址管理传输请求（纯数据容器，不加锁）
 *
 * 用于管理每个端点的待处理 IN 传输请求。
 * 注意：所有方法都不加锁，调用者需自行管理互斥锁。
 */
class EndpointRequestQueue {
public:
    struct Request {
        std::uint32_t seqnum;
        std::uint32_t length;
        TransferHandle transfer;
    };

    /**
     * @brief 向指定端点入队请求
     * @note 调用者需已持有互斥锁
     */
    void enqueue(std::uint8_t ep_address, Request request) {
        queues_[ep_address].push_back(std::move(request));
    }

    /**
     * @brief 从指定端点出队请求
     * @note 调用者需已持有互斥锁
     */
    std::optional<Request> dequeue(std::uint8_t ep_address) {
        auto it = queues_.find(ep_address);
        if (it == queues_.end() || it->second.empty()) {
            return std::nullopt;
        }
        auto req = std::move(it->second.front());
        it->second.pop_front();
        return req;
    }

    /**
     * @brief 从任何有请求的端点出队请求（返回端点地址和请求）
     * @return pair<端点地址, 请求>，如果所有队列都为空返回 nullopt
     * @note 调用者需已持有互斥锁
     */
    std::optional<std::pair<std::uint8_t, Request>> dequeue_any() {
        for (auto &[ep, queue]: queues_) {
            if (!queue.empty()) {
                auto req = std::move(queue.front());
                queue.pop_front();
                return std::make_pair(ep, std::move(req));
            }
        }
        return std::nullopt;
    }

    /**
     * @brief 获取指定端点队列的首个请求（不出队）
     * @note 调用者需已持有互斥锁
     */
    Request *peek(std::uint8_t ep_address) {
        auto it = queues_.find(ep_address);
        if (it == queues_.end() || it->second.empty()) {
            return nullptr;
        }
        return &it->second.front();
    }

    /**
     * @brief 检查指定端点队列是否为空
     * @note 调用者需已持有互斥锁
     */
    bool empty(std::uint8_t ep_address) const {
        auto it = queues_.find(ep_address);
        return it == queues_.end() || it->second.empty();
    }

    /**
     * @brief 按 seqnum 取消请求（用于 UNLINK）
     * @return 如果找到并移除了请求返回 true
     * @note 调用者需已持有互斥锁
     */
    bool cancel_by_seqnum(std::uint32_t unlink_seqnum) {
        for (auto &[ep, queue]: queues_) {
            auto it = std::find_if(queue.begin(), queue.end(),
                                   [unlink_seqnum](const Request &r) { return r.seqnum == unlink_seqnum; });
            if (it != queue.end()) {
                queue.erase(it);
                return true;
            }
        }
        return false;
    }

    /**
     * @brief 清空所有队列
     * @note 调用者需已持有互斥锁
     */
    void clear() {
        queues_.clear();
    }

private:
    std::unordered_map<std::uint8_t, std::deque<Request>> queues_;
};

/**
 * @brief IN 数据通道基类（CRTP 静态分发）
 *
 * 封装「挂起-应答」模式的公共部分：IN 请求先挂起（有数据立即应答）、数据
 * 到达时匹配挂起请求、双锁、断连清理与唤醒。缓冲与消费语义由派生类实现
 * （消息模式整条消费 / 字节流模式按长度分片），类型在编译期确定，无虚函数。
 *
 * 派生类必须实现（CRTP 要求，均要求在持有双锁时调用）：
 * - buffer_empty()：缓冲是否为空
 * - try_send_one(ep, seqnum, length, transfer)：从缓冲取数据应答一个请求
 *   （取数据填 transfer 后调 session->submit_ret_submit 提交）
 * - try_pull_data(length)：可选 pull 模型：缓冲空且无挂起请求时现场生成数据，
 *   默认返回空
 * - send_pulled_locked(ep, seqnum, length, transfer, pulled)：消费 pull 生成的
 *   数据（字节流：本次请求优先发 min 部分，剩余入缓冲；消息模式整条入缓冲）
 * - push_locked(data)：数据入缓冲（满时按各自语义：消息丢最旧 / 字节流丢弃超出部分）
 * - buffer_clear()：清空缓冲
 *
 * 典型接入（handler 内）：
 *  - 主机 IN 回调里调 on_in_request(...)：缓冲有数据立即应答，没数据则挂起请求
 *  - 业务线程产生数据：push()（消息模式，一条数据=一个消息）或 write()/write_nb()
 *    （字节流模式，按请求长度分片），通道自动匹配挂起的请求应答
 *  - 生命周期：on_new_connection 时调 bind_session + on_new_connection，
 *    on_disconnection 时调 on_disconnection（清空缓冲与挂起请求）
 */
template <typename Derived>
class InEndpointChannelBase {
public:
    /// 绑定会话（handler 的 on_new_connection / on_disconnection 时调用）
    void bind_session(Session *current_session) {
        session = current_session;
    }

    /**
     * @brief 处理一个 IN 请求（handler 的 IN 请求回调里调用）
     * @param ep 端点地址（含方向位）
     * @note 请求侧逻辑：同端点无挂起请求且缓冲有数据 → 立即应答；否则尝试
     * 派生类 pull 现场生成数据；再不行则挂起请求
     */
    void on_in_request(std::uint8_t ep, std::uint32_t seqnum, std::uint32_t length, TransferHandle transfer) {
        std::lock(channel_mutex, requests_mutex);
        std::lock_guard lock1(channel_mutex, std::adopt_lock);
        std::lock_guard lock2(requests_mutex, std::adopt_lock);

        // 同端点已有挂起请求时必须排在后面（同端点 FIFO 顺序），即使缓冲有数据
        if (endpoint_requests.empty(ep) && !self().buffer_empty()) {
            self().try_send_one(ep, seqnum, length, std::move(transfer));
            return;
        }
        // pull 模型：派生类可现场生成数据（如 CDC 数据接口的 on_data_requested）。
        // 仅在缓冲空且同端点无挂起请求时调用（缓冲空由上面的分支保证）。
        // 消费方式由派生类决定（字节流：本次请求优先，剩余入缓冲）
        if (endpoint_requests.empty(ep)) {
            auto pulled = self().try_pull_data(length);
            if (!pulled.empty()) {
                self().send_pulled_locked(ep, seqnum, length, std::move(transfer), std::move(pulled));
                try_send_pending_locked();
                return;
            }
        }
        // 挂起请求；若同端点已有挂起且缓冲有数据，顺带推进队列（发前面的请求）
        endpoint_requests.enqueue(ep, {seqnum, length, std::move(transfer)});
        try_send_pending_locked();
    }

    /**
     * @brief 取消挂起的请求（UNLINK 处理）
     * @return 真的取消了返回 true（回 -ECONNRESET），请求不存在返回 false（回 0）
     */
    bool cancel_pending(std::uint32_t unlink_seqnum) {
        std::lock_guard lock(requests_mutex);
        return endpoint_requests.cancel_by_seqnum(unlink_seqnum);
    }

    /// 新连接：从干净状态开始（缓冲清空、断连标记清除）
    void on_new_connection() {
        std::lock_guard lock(channel_mutex);
        disconnected = false;
        self().buffer_clear();
    }

    /// 断连：清空缓冲与挂起请求，唤醒阻塞的写者让它们按断连返回
    void on_disconnection() {
        {
            std::lock_guard lock(channel_mutex);
            disconnected = true;
            self().buffer_clear();
        }
        {
            std::lock_guard lock(requests_mutex);
            // TransferHandle 析构时自动释放
            endpoint_requests.clear();
        }
        space_cv.notify_all();
    }

protected:
    /// 尝试应答挂起请求：缓冲非空就一直发（dequeue_any 取任意端点，缓冲空即停）。
    /// 调用者必须已持有 channel_mutex 和 requests_mutex
    void try_send_pending_locked() {
        while (!self().buffer_empty()) {
            auto req_opt = endpoint_requests.dequeue_any();
            if (!req_opt.has_value()) {
                break;
            }
            auto &[ep_addr, req] = req_opt.value();
            self().try_send_one(ep_addr, req.seqnum, req.length, std::move(req.transfer));
        }
    }

    Derived &self() {
        return static_cast<Derived &>(*this);
    }

    // 会话指针（handler 绑定，try_send_one 提交应答用）
    Session *session = nullptr;

    // 缓冲锁与请求队列锁：数据侧与请求侧可能在不同线程（业务线程 vs session
    // receiver 线程），双锁让写侧等空间时不必持有队列锁。
    // mutable：const 查询方法（size/available）也要锁
    mutable std::mutex channel_mutex;
    mutable std::mutex requests_mutex;
    // 挂起的 IN 传输请求（按端点排队）
    EndpointRequestQueue endpoint_requests;
    // 缓冲腾出空间时唤醒阻塞的写者（字节流模式用）
    std::condition_variable space_cv;
    // 断连标记（初始为断连，on_new_connection 后可用）
    bool disconnected = true;
};

/**
 * @brief 消息模式 IN 通道：缓冲是消息队列，整条消费
 *
 * 适用 HID 输入报告 / UVC 状态通知等「一条数据 = 一个完整消息」的场景。
 * push 非阻塞：有挂起请求直接应答，否则入缓冲，超上限丢最旧（主机长期
 * 不读时防止内存无限增长）。
 *
 * 数据填充走 TransferHandle 持有的 TransferOperator::set_transfer_data，
 * 组件不假设 transfer 内部结构（支持 GenericTransfer、libusb 等任意实现）
 */
class MessageInChannel : public InEndpointChannelBase<MessageInChannel> {
public:
    /**
     * @brief 推入一条消息（非阻塞）
     * @note 任意线程可调用。有挂起请求时直接应答，否则入缓冲
     */
    void push(data_type data) {
        std::lock(this->channel_mutex, this->requests_mutex);
        std::lock_guard lock1(this->channel_mutex, std::adopt_lock);
        std::lock_guard lock2(this->requests_mutex, std::adopt_lock);
        push_locked(std::move(data));
        this->try_send_pending_locked();
    }

    /// 设置缓冲上限（0 = 无限），默认 0
    void set_max_pending(std::size_t max_pending) {
        std::lock_guard lock(this->channel_mutex);
        max_pending_ = max_pending;
    }

    /// 缓冲中待发消息数（HID 的 has_pending_input_reports 等查询用）
    std::size_t size() const {
        std::lock_guard lock(this->channel_mutex);
        return pending.size();
    }

    // ===== CRTP 接口（锁内调用） =====

    bool buffer_empty() const {
        return pending.empty();
    }

    void try_send_one(std::uint8_t ep, std::uint32_t seqnum, std::uint32_t length, TransferHandle transfer) {
        // 调用者已持双锁且缓冲非空：取最旧一条消息，按请求长度截断应答
        auto &front = pending.front();
        auto send_len = std::min(front.size(), static_cast<std::size_t>(length));
        data_type data(front.begin(), front.begin() + send_len);
        pending.pop_front();
        // 数据填充由 op 完成（组件不假设 transfer 内部结构）。
        // 前置条件：能到达这里的 transfer 必然持有 op（CMD_SUBMIT 解析时由
        // alloc_transfer_handle 创建并封装进 TransferHandle，空句柄不会入队）
        assert(transfer.get_operator() != nullptr);
        auto written = transfer.get_operator()->set_transfer_data(transfer.get(), data, length);
        this->session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
                seqnum, static_cast<std::uint32_t>(written), std::move(transfer)));
    }

    data_type try_pull_data(std::uint32_t length) {
        // 消息模式一般不用 pull（数据由业务线程主动推），默认空
        return {};
    }

    void send_pulled_locked(std::uint8_t ep, std::uint32_t seqnum, std::uint32_t length, TransferHandle transfer,
                            data_type pulled) {
        // 消息模式不常用 pull；若派生类重写 try_pull_data 后走到这里，
        // 整条入缓冲后由本次请求消费（与 push 语义一致）
        push_locked(std::move(pulled));
        try_send_one(ep, seqnum, length, std::move(transfer));
    }

    void push_locked(data_type data) {
        // 缓冲超限丢最旧（保持最新消息语义）
        if (max_pending_ != 0 && pending.size() >= max_pending_) {
            pending.pop_front();
        }
        pending.emplace_back(std::move(data));
    }

    void buffer_clear() {
        pending.clear();
    }

private:
    std::deque<data_type> pending;
    std::size_t max_pending_ = 0;
};

/**
 * @brief 字节流模式 IN 通道：缓冲是字节流（RingBuffer），按请求长度分片消费
 *
 * 适用 CDC 数据接口、通用管道等「数据是连续字节流」的场景。
 * write 阻塞：缓冲满时等宿主取走（timeout_ms=0 无限），对齐内核 FIFO 语义。
 *
 * 数据填充走 TransferHandle 持有的 TransferOperator::set_transfer_data，
 * 组件不假设 transfer 内部结构（支持 GenericTransfer、libusb 等任意实现）
 */
class ByteStreamInChannel : public InEndpointChannelBase<ByteStreamInChannel> {
public:
    /**
     * @brief 非阻塞写：写入可用空间，立即尝试应答挂起请求
     * @return 实际写入字节数（满时小于 size）
     */
    std::size_t write_nb(const std::uint8_t *data, std::size_t size) {
        std::lock(this->channel_mutex, this->requests_mutex);
        std::lock_guard lock1(this->channel_mutex, std::adopt_lock);
        std::lock_guard lock2(this->requests_mutex, std::adopt_lock);
        if (this->disconnected) {
            return 0;
        }
        std::size_t written = buffer.write(data, size);
        this->try_send_pending_locked();
        return written;
    }

    std::size_t write_nb(const data_type &data) {
        return write_nb(data.data(), data.size());
    }

    /**
     * @brief 阻塞写：缓冲满时等待宿主取走数据（timeout_ms=0 无限等）
     * @return 实际写入字节数；超时可能小于 size；断连返回已写入量
     */
    std::size_t write(const std::uint8_t *data, std::size_t size, std::uint32_t timeout_ms = 0) {
        std::size_t total_written = 0;
        std::size_t offset = 0;

        while (offset < size) {
            // 阶段1：等待缓冲有空间
            {
                std::unique_lock lock(this->channel_mutex);
                if (this->disconnected) {
                    return total_written;
                }
                if (buffer.available() == 0) {
                    // 先尝试应答挂起的请求腾出空间
                    {
                        std::lock_guard queue_lock(this->requests_mutex);
                        this->try_send_pending_locked();
                    }
                    if (this->disconnected) {
                        return total_written;
                    }
                    // 仍满则等待：宿主 IN 请求取走数据后由 try_send_one 唤醒
                    while (buffer.available() == 0 && !this->disconnected) {
                        if (timeout_ms == 0) {
                            this->space_cv.wait(lock);
                        }
                        else {
                            if (this->space_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms)) ==
                                std::cv_status::timeout) {
                                // 超时时刻空间可能恰好腾出：跳出循环走统一判断，能写则写
                                break;
                            }
                        }
                    }
                    if (this->disconnected) {
                        return total_written;
                    }
                    if (buffer.available() == 0) {
                        return total_written; // 超时且仍未腾出空间
                    }
                }
                std::size_t written = buffer.write(data + offset, size - offset);
                total_written += written;
                offset += written;
            }
            // 阶段2：缓冲有数据了，尝试应答挂起的请求
            {
                std::lock(this->channel_mutex, this->requests_mutex);
                std::lock_guard lock1(this->channel_mutex, std::adopt_lock);
                std::lock_guard lock2(this->requests_mutex, std::adopt_lock);
                this->try_send_pending_locked();
            }
        }
        return total_written;
    }

    std::size_t write(const data_type &data, std::uint32_t timeout_ms = 0) {
        return write(data.data(), data.size(), timeout_ms);
    }

    /// 设置缓冲容量（默认 64KB），必须在连接前调用
    void set_capacity(std::size_t capacity) {
        std::lock_guard lock(this->channel_mutex);
        buffer.resize(capacity);
    }

    std::size_t capacity() const {
        std::lock_guard lock(this->channel_mutex);
        return buffer.capacity();
    }

    /// 缓冲中待发字节数
    std::size_t size() const {
        std::lock_guard lock(this->channel_mutex);
        return buffer.size();
    }

    /// 缓冲可写字节数
    std::size_t available() const {
        std::lock_guard lock(this->channel_mutex);
        return buffer.available();
    }

    /**
     * @brief 设置 pull 回调：IN 请求到达且缓冲/队列都空时调用，可现场生成数据
     * @note 在锁内调用，回调里不要调用本通道的写方法（会死锁）
     */
    void set_pull_callback(std::function<data_type(std::uint32_t length)> callback) {
        pull_callback = std::move(callback);
    }

    // ===== CRTP 接口（锁内调用） =====

    bool buffer_empty() const {
        return buffer.empty();
    }

    void try_send_one(std::uint8_t ep, std::uint32_t seqnum, std::uint32_t length, TransferHandle transfer) {
        // 调用者已持双锁且缓冲非空：按请求长度分片取出
        std::size_t send_len = std::min(buffer.size(), static_cast<std::size_t>(length));
        data_type data(send_len);
        buffer.read(data.data(), send_len);
        // 数据填充由 op 完成（组件不假设 transfer 内部结构）。
        // 前置条件：能到达这里的 transfer 必然持有 op（CMD_SUBMIT 解析时由
        // alloc_transfer_handle 创建并封装进 TransferHandle，空句柄不会入队）
        assert(transfer.get_operator() != nullptr);
        auto written = transfer.get_operator()->set_transfer_data(transfer.get(), data, length);
        // 缓冲腾出空间，唤醒阻塞等待的写者
        this->space_cv.notify_one();
        this->session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
                seqnum, static_cast<std::uint32_t>(written), std::move(transfer)));
    }

    data_type try_pull_data(std::uint32_t length) {
        if (pull_callback) {
            return pull_callback(length);
        }
        return {};
    }

    void send_pulled_locked(std::uint8_t ep, std::uint32_t seqnum, std::uint32_t length, TransferHandle transfer,
                            data_type pulled) {
        // pull 数据优先满足本次请求（对齐 CdcAcm 原语义：发 min 部分），
        // 超出部分写入缓冲（写不下的丢弃，与 push_locked 一致）
        auto send_len = std::min(pulled.size(), static_cast<std::size_t>(length));
        data_type data(pulled.begin(), pulled.begin() + send_len);
        // 数据填充由 op 完成（前置条件见 try_send_one）
        assert(transfer.get_operator() != nullptr);
        auto written = transfer.get_operator()->set_transfer_data(transfer.get(), data, length);
        this->session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
                seqnum, static_cast<std::uint32_t>(written), std::move(transfer)));
        if (pulled.size() > send_len) {
            buffer.write(pulled.data() + send_len, pulled.size() - send_len);
        }
    }

    void push_locked(data_type data) {
        // 整包写入，写不下的部分丢弃（调用方通常保证不超容量）
        buffer.write(data.data(), data.size());
    }

    void buffer_clear() {
        buffer.clear();
    }

private:
    RingBuffer buffer;
    std::function<data_type(std::uint32_t)> pull_callback;
};

} // namespace usbipdcpp
