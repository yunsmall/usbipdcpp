#include "usbipdcpp/virtual_device/TransferScheduler.h"

#include <algorithm>
#include <vector>

#include "usbipdcpp/Session.h"
#include "spdlog/spdlog.h"

namespace usbipdcpp {

void TransferScheduler::start(Session &current_session) {
    std::lock_guard lock(mutex);
    if (started)
        return;
    started = true;
    session = &current_session;
    io_context.restart();
    work_guard = std::make_unique<asio::executor_work_guard<asio::io_context::executor_type>>(io_context.get_executor());
    thread = std::thread([this] { run(); });
}

void TransferScheduler::stop() {
    {
        std::lock_guard lock(mutex);
        if (!started)
            return;
        started = false;
        // 连接已断：丢弃未完成 URB（不响应，对齐 vudc stop_activity 的 nuke 队列）
        endpoints.clear();
        session = nullptr;
        // 取消排期：aborted 回调会因 started=false 直接返回
        timer_pending = false;
        timer.cancel();
    }
    // 放行 work_guard（允许 run() 在无 work 时返回）+ 停止 io_context
    // 让 run() 立即返回（正在执行的 on_timer 会先跑完），join 等待
    work_guard.reset();
    io_context.stop();
    if (thread.joinable())
        thread.join();
}

void TransferScheduler::submit(const UsbEndpoint &ep, EndpointAttributes type, int num_iso_packets,
                               UsbIpResponse::UsbIpRetSubmit &&submit,
                               std::chrono::microseconds data_duration) {
    // 端点版：间隔按 bInterval 与设备速度推导（对齐 USB 规范）。
    // 参数名 submit 遮蔽了成员函数，需 this-> 显式解析
    this->submit(ep.address, type, endpoint_interval(ep, speed), num_iso_packets, std::move(submit), data_duration);
}

void TransferScheduler::submit(std::uint8_t ep_address, EndpointAttributes type, std::chrono::microseconds interval,
                               int num_iso_packets, UsbIpResponse::UsbIpRetSubmit &&submit,
                               std::chrono::microseconds data_duration) {
    // 无等时包：不占调度窗口，立即响应
    if (num_iso_packets <= 0) {
        Session *s;
        {
            std::lock_guard lock(mutex);
            s = session;
        }
        if (s)
            on_urb_completed(*s, std::move(submit));
        return;
    }

    // 非等时传输的帧调度语义尚未接入（预留：vudc 的帧内撮合/带宽预算），
    // 当前调度器只被等时设备使用，此处不应出现其他类型
    if (type != EndpointAttributes::Isochronous) {
        SPDLOG_ERROR("TransferScheduler 暂不支持 {} 类型的帧调度", static_cast<int>(type));
        Session *s;
        {
            std::lock_guard lock(mutex);
            s = session;
        }
        if (s)
            on_urb_completed(*s, std::move(submit));
        return;
    }

    std::lock_guard lock(mutex);
    // stop 之后到达的 URB：连接已断，直接丢弃
    if (!started)
        return;

    auto &state = endpoints[ep_address];
    auto now = std::chrono::steady_clock::now();
    // 显式数据时长优先：跟随主机数据量而非本地时钟固定间隔，
    // 设备与主机时钟的频偏不累积（自适应端点跟随行为）。
    // 允许负值（提前响应，水位闭环修正主机提交开销用）；
    // 结果为负/零时 deadline 不早于当前时刻，到点即完成
    auto delay = data_duration != std::chrono::microseconds::zero() ? data_duration
                                                                   : interval * num_iso_packets;
    auto deadline = std::max(now, state.last_deadline) + delay;
    state.last_deadline = deadline;
    state.queue.push_back(PendingUrb{deadline, std::move(submit)});
    kick();
}

bool TransferScheduler::cancel(std::uint32_t seqnum) {
    std::lock_guard lock(mutex);
    for (auto &[ep, state] : endpoints) {
        for (auto it = state.queue.begin(); it != state.queue.end(); ++it) {
            if (it->submit.header.seqnum == seqnum) {
                state.queue.erase(it);
                // last_deadline 不回退：被取消 URB 占用的总线时间不回收
                // （对齐真实总线：时隙空着也流逝），后续 URB 节奏不变
                return true;
            }
        }
    }
    return false;
}

std::chrono::microseconds TransferScheduler::endpoint_interval(const UsbEndpoint &ep, UsbSpeed speed) {
    auto interval = ep.interval;
    if (interval == 0)
        interval = 1; // 防御非法 bInterval=0
    switch (speed) {
        case UsbSpeed::High:
            // 高速：2^(bInterval-1) 个 microframe，每个 125µs（bInterval 上限 16）
            interval = std::min(interval, static_cast<std::uint8_t>(16));
            return std::chrono::microseconds(125) * (std::uint64_t{1} << (interval - 1));
        case UsbSpeed::Super:
        case UsbSpeed::SuperPlus:
            // SuperSpeed：bInterval × 125µs
            return std::chrono::microseconds(125) * interval;
        default:
            // 全速/低速：bInterval 个帧，每帧 1ms
            return std::chrono::milliseconds(1) * interval;
    }
}

void TransferScheduler::run() {
    io_context.run();
}

void TransferScheduler::on_urb_completed(Session &current_session, UsbIpResponse::UsbIpRetSubmit &&submit) {
    current_session.submit_ret_submit(std::move(submit));
}

void TransferScheduler::kick() {
    // 定时器已排期（RUNNING）：新 URB 只入队，到期处理时会扫到
    // （同端点串行保证 deadline 递增，不会被现有排期拖延）
    if (timer_pending)
        return;
    // 排期动作交给调度线程（post 的唤醒是 io_context 核心机制，可靠）：
    // 跨线程直接注册 timer 在 Windows 上偶发不唤醒，见 schedule_next 注释
    asio::post(io_context, [this] { schedule_on_thread(); });
}

void TransferScheduler::schedule_on_thread() {
    std::lock_guard lock(mutex);
    if (!started)
        return;
    // 另一路（on_timer 的 schedule_next）可能已排期
    if (timer_pending)
        return;
    schedule_next();
}

void TransferScheduler::schedule_next() {
    auto next = std::chrono::steady_clock::time_point::max();
    for (auto &[ep, state] : endpoints) {
        if (!state.queue.empty())
            next = std::min(next, state.queue.front().deadline);
    }
    if (next == std::chrono::steady_clock::time_point::max())
        return; // 队列空：定时器转 IDLE（对齐 vudc 的 IDLE 状态）

    timer_pending = true;
    timer.expires_at(next);
    timer.async_wait([this](const asio::error_code &ec) { on_timer(ec); });
}

void TransferScheduler::on_timer(const asio::error_code &ec) {
    std::vector<UsbIpResponse::UsbIpRetSubmit> done;
    Session *s;
    {
        std::lock_guard lock(mutex);
        timer_pending = false;
        // aborted：仅 stop() 取消排期场景，连接已断不处理
        if (ec || !started)
            return;
        s = session;
        if (!s)
            return;

        auto now = std::chrono::steady_clock::now();
        // 完成所有到期的队头 URB（不同端点可同时到期；同端点因串行不会重叠）
        for (auto &[ep, state] : endpoints) {
            while (!state.queue.empty() && state.queue.front().deadline <= now) {
                done.push_back(std::move(state.queue.front().submit));
                state.queue.pop_front();
            }
        }
        // 队列可能仍有未到期 URB：排期下一次
        schedule_next();
    }
    for (auto &sub : done)
        on_urb_completed(*s, std::move(sub));
}

TransferScheduler::~TransferScheduler() {
    stop();
}

} // namespace usbipdcpp
