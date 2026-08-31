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
     * @brief 获取指定端点队列中的请求数
     * @note 调用者需已持有互斥锁
     */
    std::size_t size(std::uint8_t ep_address) const {
        auto it = queues_.find(ep_address);
        return it == queues_.end() ? 0 : it->second.size();
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
 * @brief IN 数据通道基类（CRTP 静态分发）：管理「主机的 IN 请求」与「设备要发的数据」配对应答
 *
 * 数据方向：设备 → 主机（USB IN 端点）。主机的 IN 请求到来时设备可能还没数据，
 * 请求先挂起；业务侧把数据写进通道后自动匹配挂起的请求应答（主机读走）。
 * 主机 IN 回调与业务数据写入可能在不同线程，通道内部已上锁，直接调用即可。
 *
 * 按数据处理形式选派生类（编译期确定，无虚函数）：
 *  - MessageInChannel    一条数据 = 一个完整消息（HID 输入报告、UVC 状态通知）
 *  - ByteStreamInChannel 数据是连续字节流，按请求长度分片（CDC 数据口、通用管道）
 *
 * 接入三件事（handler 内，缺一不可）：
 *  1. 生命周期：on_new_connection 时调 on_new_connection(会话)，
 *     断连时调 on_disconnection()（清空缓冲与挂起请求、唤醒阻塞写者）
 *  2. 主机 IN 请求到达：在接口的 IN 传输回调里调 on_in_request(ep, seqnum, length, transfer)
 *  3. 业务侧产生数据：调 push()（消息模式）或 write()/write_nb()（字节流模式）
 *
 * 可选配置（按需调用，别把两个"上限"搞混——方向相反）：
 *  - set_max_pending_requests：限「主机发来的 IN 请求」挂起队列。设备没数据
 *    应答时请求会堆积，超限挤掉最旧并回空完成（主机 URB 空完成即重提交，
 *    等价 USB 总线 NAK 背压）。默认 0 = 无限
 *  - set_max_pending_messages（消息模式）/ set_capacity（字节流模式）：限「设备待发的
 *    数据」缓冲。主机长期不读时数据堆积，超限由 push 的 drop_oldest 决定丢最旧
 *    或返回 false，push_blocking/write 则阻塞等待。默认 0 = 无限
 *
 * 派生类需实现（锁内调用的 CRTP 接口，见各派生类声明）：
 *  buffer_empty / buffer_clear：缓冲为空/清空
 *  try_send_one：从缓冲取数据应答一个请求
 *  try_pull_data / send_pulled_locked：可选 pull 模型（请求到达时现场生成数据）
 *  push_locked：数据入缓冲
 */
template <typename Derived>
class InEndpointChannelBase {
public:
    /**
     * @brief 新连接激活通道：设会话指针并复位状态（缓冲清空、断连标记清除）。
     * handler 的 on_new_connection 里传当前会话调用；无参（测试桩复位）时保持
     * 已绑定的会话不变
     */
    void on_new_connection(Session *current_session = nullptr) {
        std::lock_guard lock(channel_mutex);
        if (current_session) {
            session = current_session;
        }
        disconnected = false;
        self().buffer_clear();
    }

    /**
     * @brief 处理一个主机 IN 请求（接口 IN 传输回调里转发，session receiver 线程）
     *
     * 通道自动判断：有缓冲数据 / 能现场 pull → 立即回 RET_SUBMIT；否则请求挂起，
     * 等业务数据写入后自动应答。
     * @param ep 端点地址（含方向位，如 0x81）
     * @param seqnum 本次命令序列号（CMD_SUBMIT 头里的）
     * @param length 主机请求的字节数（transfer_buffer_length）
     * @param transfer 本次传输句柄，应答时用它承载数据发给主机
     */
    void on_in_request(std::uint8_t ep, std::uint32_t seqnum, std::uint32_t length, TransferHandle transfer) {
        std::lock(channel_mutex, requests_mutex);
        std::lock_guard lock1(channel_mutex, std::adopt_lock);
        std::lock_guard lock2(requests_mutex, std::adopt_lock);

        // 断连后不再接收：请求直接释放（transfer 析构自动回收），不入队不应答
        //（对齐 OutEndpointChannel 的断连处理，防断连竞态下请求残留到下次连接）
        if (disconnected) {
            return;
        }

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
        // 挂起请求；若同端点已有挂起且缓冲有数据，顺带推进队列（发前面的请求）。
        // 先按上限挤掉最旧的：该端点挂起已达上限时，把最早的请求出队并应答空
        // 完成（0 字节）——主机 URB 空完成后重新提交，等价 USB 总线的 NAK 背压，
        // 挂起队列保持有界（设备来不及回复时不让主机请求无限堆积）
        if (max_pending_requests_ != 0 && endpoint_requests.size(ep) >= max_pending_requests_) {
            auto evicted = endpoint_requests.dequeue(ep);
            if (evicted.has_value()) {
                self().reply_empty(evicted->seqnum, std::move(evicted->transfer));
            }
        }
        endpoint_requests.enqueue(ep, {seqnum, length, std::move(transfer)});
        try_send_pending_locked();
    }

    /**
     * @brief 设置挂起请求上限（0 = 无限，默认）。达到上限时新请求挤掉该端点
     * 最早的请求，并应答空完成（actual_length=0、status 正常）——主机 URB
     * 空完成即重新提交，等价 USB 总线的 NAK 背压，挂起队列保持有界
     * @note 这是「主机发来的请求」的上限，与数据缓冲上限
     * （MessageInChannel::set_max_pending / ByteStreamInChannel::set_capacity）
     * 方向相反：前者限请求堆积（设备没数据应答时），后者限数据堆积（主机不读时）
     */
    void set_max_pending_requests(std::size_t max_pending) {
        std::lock_guard lock(requests_mutex);
        max_pending_requests_ = max_pending;
    }

    /**
     * @brief 取消一个挂起的 IN 请求（处理主机的 UNLINK 命令时调用）
     * @param unlink_seqnum 要取消的请求 seqnum
     * @return true = 请求确实还在挂起队列里、已取消（应答 -ECONNRESET）；
     *         false = 请求不存在或已应答过（应答 0）
     */
    bool cancel_pending(std::uint32_t unlink_seqnum) {
        std::lock_guard lock(requests_mutex);
        return endpoint_requests.cancel_by_seqnum(unlink_seqnum);
    }

    /**
     * @brief 断连：清空缓冲与挂起的请求（transfer 析构自动释放）、唤醒阻塞
     * 的写者。之后写接口返回 0/短写；连接状态由 handler 管理
     */
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
    // 挂起请求上限（0 = 无限）；on_in_request 按端点检查
    std::size_t max_pending_requests_ = 0;
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
     * @brief 非阻塞推入一条消息（任意线程可调）
     *
     * 有挂起的 IN 请求时直接应答；没有则入缓冲等主机来读。
     * @param data 一条完整消息
     * @param drop_oldest true（默认）= 缓冲超上限时丢最旧的一条，保持「最新消息
     *        优先」语义（历史调用行为不变）；false = 缓冲满时不入队，返回 false
     *        告诉调用者满了、本次未发送成功，由调用者决定丢弃或重试
     * @return 是否成功入队（drop_oldest=true 时恒为 true）
     */
    bool push(data_type data, bool drop_oldest = true) {
        std::lock(this->channel_mutex, this->requests_mutex);
        std::lock_guard lock1(this->channel_mutex, std::adopt_lock);
        std::lock_guard lock2(this->requests_mutex, std::adopt_lock);
        // 断连后不再接收：返回 false 告诉调用者本次未发送成功（数据会在下次
        // 连接被清空，白攒；对齐 write_nb 断连返回 0 的语义）
        if (this->disconnected) {
            return false;
        }
        // 满且调用者不丢旧：不入队，告诉调用者满了
        if (!drop_oldest && max_pending_ != 0 && pending.size() >= max_pending_) {
            return false;
        }
        push_locked(std::move(data));
        this->try_send_pending_locked();
        return true;
    }

    /**
     * @brief 阻塞推入一条消息：缓冲满时等待主机读走腾出空位再入队
     *
     * 适用「数据必须完整送达、可等待」的场景（对称 ByteStream 的阻塞 write）。
     * 等待期间释放 channel_mutex，主机请求仍能入队应答取走消息（try_send_one
     * 腾出空位后唤醒）。
     * @param data 一条完整消息
     * @param timeout_ms 每阶段等待空位的超时（毫秒）；0 = 无限等
     * @return true = 入队成功；false = 断连或等待超时（仍满）
     */
    bool push_blocking(data_type data, std::uint32_t timeout_ms = 0) {
        std::unique_lock lock(this->channel_mutex);
        while (max_pending_ != 0 && pending.size() >= max_pending_) {
            if (this->disconnected) {
                return false;
            }
            if (timeout_ms == 0) {
                this->space_cv.wait(lock);
            }
            else if (this->space_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms)) == std::cv_status::timeout) {
                break;
            }
        }
        if (this->disconnected) {
            return false;
        }
        if (max_pending_ != 0 && pending.size() >= max_pending_) {
            return false; // 等待超时仍满
        }
        // 空位就绪：补 requests 锁入队并推进挂起请求（与 push 同锁序）
        std::lock_guard lock2(this->requests_mutex);
        push_locked(std::move(data));
        this->try_send_pending_locked();
        return true;
    }

    /**
     * @brief 设置待发消息缓冲上限（0 = 无限，默认）
     * @note 主机一直不读时 push 按此上限丢最旧，防内存无限增长。
     * 这是「设备待发数据」的上限，与基类 set_max_pending_requests
     * （「主机请求」的上限，NAK 背压）方向相反
     */
    void set_max_pending_messages(std::size_t max_pending) {
        std::lock_guard lock(this->channel_mutex);
        max_pending_ = max_pending;
    }

    /**
     * @brief 缓冲中待发（未应答）的消息数
     * @note 供 has_pending_input_reports 等查询用；任意线程可调
     */
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
        this->space_cv.notify_one(); // 消息取走腾出空位，唤醒阻塞的 push_blocking
        this->session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
                seqnum, static_cast<std::uint32_t>(written), std::move(transfer)));
    }

    void reply_empty(std::uint32_t seqnum, TransferHandle /*transfer*/) {
        // 挤出的挂起请求：应答空完成（0 字节，status 正常），transfer 析构自动
        // 释放。主机 URB 空完成后重新提交（等价 USB 总线 NAK 背压）
        this->session->submit_ret_submit(
                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(seqnum, 0));
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
     * @brief 非阻塞写：把数据写入缓冲并尝试应答挂起的 IN 请求（任意线程可调）
     * @return 实际写入字节数；断连返回 0；缓冲满时可能小于 size（超出部分丢弃）
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
     * @brief 阻塞写：把数据全部写入缓冲才返回。缓冲满时等待主机读走
     * （timeout_ms=0 无限等，对齐内核 FIFO「写满阻塞」语义）。
     * @param data 数据起始指针
     * @param size 要写的字节数
     * @param timeout_ms 每阶段等待缓冲空间的超时（毫秒）；0 = 无限等
     * @return 实际写入字节数；断连返回已写入量；超时返回部分写入（< size）
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

    /**
     * @brief 设置缓冲容量（字节，默认 64KB）
     * @note 必须在连接前调用；连接后再改无效。
     * 这是「设备待发数据」的上限，与基类 set_max_pending_requests
     * （「主机请求」的上限，NAK 背压）方向相反
     */
    void set_capacity(std::size_t capacity) {
        std::lock_guard lock(this->channel_mutex);
        buffer.resize(capacity);
    }

    /** @brief 缓冲总容量（字节） */
    std::size_t capacity() const {
        std::lock_guard lock(this->channel_mutex);
        return buffer.capacity();
    }

    /** @brief 缓冲中待发（未读走）的字节数 */
    std::size_t size() const {
        std::lock_guard lock(this->channel_mutex);
        return buffer.size();
    }

    /** @brief 缓冲剩余可写字节数 */
    std::size_t available() const {
        std::lock_guard lock(this->channel_mutex);
        return buffer.available();
    }

    /**
     * @brief 设置 pull 回调：IN 请求到达且缓冲/挂起队列都空时调用，可现场生成
     * 数据（如 CDC 数据口由派生数据生成器产生持续输出）。
     * @note 回调在通道持锁时被调用，回调里不要调用本通道的写方法（会死锁）
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

    void reply_empty(std::uint32_t seqnum, TransferHandle /*transfer*/) {
        // 挤出的挂起请求：应答空完成（0 字节，status 正常），transfer 析构自动
        // 释放。主机 URB 空完成后重新提交（等价 USB 总线 NAK 背压）
        this->session->submit_ret_submit(
                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(seqnum, 0));
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
