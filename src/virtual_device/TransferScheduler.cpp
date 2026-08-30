#include "usbipdcpp/virtual_device/TransferScheduler.h"

#include <algorithm>
#include <vector>

#include "usbipdcpp/Session.h"

namespace usbipdcpp {

namespace {

/// 从服务开始时刻对齐到下一个帧边界（1ms 网格）的等待时长：
/// vudc 帧定时器按 1ms tick 推进，帧边界 = 1ms 网格。
/// 注意：精度受平台定时器分辨率限制（Windows 默认 ~15.6ms 粒度，
/// asio timer 无法精确触发亚毫秒 deadline），实际节流粒度
/// 1ms~15.6ms，批量到期时同端点多个 URB 可能同批完成——平均吞吐仍受限
std::chrono::microseconds frame_alignment_delay(std::chrono::steady_clock::time_point service_start) {
    return std::chrono::microseconds(1000 - static_cast<std::int64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(service_start.time_since_epoch()).count() % 1000));
}

} // namespace

void TransferScheduler::start() {
    std::lock_guard lock(mutex);
    if (started)
        return;
    started = true;
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

void TransferScheduler::submit(Session &session, const UsbEndpoint &ep, EndpointAttributes type,
                               std::chrono::microseconds data_duration, std::uint32_t seqnum,
                               TransferHandle transfer, UrbProcessCallback processor) {
    // 防御：控制请求不走处理器版（handler 无此调用场景），立即通知处理
    if (type == EndpointAttributes::Control) {
        processor(session, ep, seqnum, std::move(transfer));
        return;
    }

    std::lock_guard lock(mutex);
    // stop 之后到达的 URB：连接已断，直接丢弃
    if (!started)
        return;

    auto &state = endpoints[ep.address];
    bool queue_was_empty = state.queue.empty();
    state.queue.push_back(PendingUrb{{}, std::move(processor), {}, ep, type, seqnum, data_duration,
                                     std::move(transfer), &session});
    // 成为队头即定死服务时刻（自持链）；processing 中只算不排期
    //（放行后 on_urb_done 的 kick 排期）
    if (queue_was_empty) {
        arm_next(ep.address, state);
        if (!state.processing)
            kick();
    }
}

void TransferScheduler::submit(Session &session, const UsbEndpoint &ep, EndpointAttributes type,
                               int num_iso_packets, UsbIpResponse::UsbIpRetSubmit &&submit,
                               std::chrono::microseconds data_duration) {
    // 端点版：间隔按 bInterval 与设备速度推导（对齐 USB 规范）。
    // 参数名 submit 遮蔽了成员函数，需 this-> 显式解析
    this->submit(session, ep.address, type, endpoint_interval(ep, speed), num_iso_packets, std::move(submit),
                 data_duration);
}

void TransferScheduler::submit(Session &session, std::uint8_t ep_address, EndpointAttributes type,
                               std::chrono::microseconds interval, int num_iso_packets,
                               UsbIpResponse::UsbIpRetSubmit &&submit,
                               std::chrono::microseconds data_duration) {
    // 控制请求与无等时包的 iso URB：不占调度窗口，立即响应
    //（控制端点必须快速，对齐 vudc 的 ep0 无延迟处理）
    if (type == EndpointAttributes::Control || (type == EndpointAttributes::Isochronous && num_iso_packets <= 0)) {
        on_urb_completed(session, std::move(submit));
        return;
    }

    std::lock_guard lock(mutex);
    // stop 之后到达的 URB：连接已断，直接丢弃
    if (!started)
        return;

    auto &state = endpoints[ep_address];
    // 等时延迟：显式数据时长优先（跟随主机数据量，允许负值 = 提前响应，
    // 收流速率闭环用），否则按包数×间隔。
    // bulk/interrupt：服务时刻对齐帧边界（arm_next 里做，delay 不用）
    auto delay = (type == EndpointAttributes::Isochronous)
                         ? (data_duration != std::chrono::microseconds::zero()
                                    ? data_duration
                                    : interval * num_iso_packets)
                         : std::chrono::microseconds::zero();
    auto seqnum = submit.header.seqnum;
    bool queue_was_empty = state.queue.empty();
    state.queue.push_back(PendingUrb{{}, {}, std::move(submit), UsbEndpoint{}, type, seqnum, delay, {}, &session});
    // 成为队头即定死服务时刻（自持链）；processing 中只算不排期
    //（放行后 on_urb_done 的 kick 排期）
    if (queue_was_empty) {
        arm_next(ep_address, state);
        if (!state.processing)
            kick();
    }
}

void TransferScheduler::on_urb_done(std::uint8_t ep_address, std::uint32_t seqnum) {
    std::lock_guard lock(mutex);
    // stop 之后（队列已清）到达的上报：忽略
    if (!started)
        return;
    auto it = endpoints.find(ep_address);
    if (it == endpoints.end())
        return;
    auto &state = it->second;
    // seqnum 校验：须匹配处理中的 URB（处理器完成收尾后的重复上报被忽略）
    if (!state.processing || state.processing_seqnum != seqnum)
        return;
    // 只放行不重算：下一个 URB 的服务时刻在提交时已定死（自持链），
    // 处理耗时/定时器触发延迟不进入节奏。处理慢（链时刻已过）时
    // schedule_next 按已过的 service_time 排期，立即触发服务
    state.processing = false;
    state.processing_seqnum = 0;
    if (!state.queue.empty())
        kick();
}

bool TransferScheduler::cancel(std::uint32_t seqnum) {
    std::lock_guard lock(mutex);
    for (auto &[ep, state] : endpoints) {
        // 已交给处理器（处理中）的 URB 取消不了
        if (state.processing && state.processing_seqnum == seqnum)
            return false;
        for (auto it = state.queue.begin(); it != state.queue.end(); ++it) {
            if (it->seqnum == seqnum) {
                auto is_front = (it == state.queue.begin());
                auto canceled_service_time = it->service_time;
                state.queue.erase(it);
                // 被取消的是队头：其服务窗口不回收（对齐真实总线：时隙
                // 空着也流逝）——链基准推进到它的服务时刻，后继 URB 仍按
                // 原节奏完成，不提前
                if (is_front)
                    state.last_scheduled = std::max(state.last_scheduled, canceled_service_time);
                // 队列仍有待服务 URB 且端点空闲：重算新队头服务时刻
                if (!state.processing && !state.queue.empty())
                    arm_next(ep, state);
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
    //（同端点串行保证服务时刻递增，不会被现有排期拖延）
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
        // 处理中（等待 on_urb_done）或队列空：不参与排期
        if (state.processing || state.queue.empty())
            continue;
        next = std::min(next, state.queue.front().service_time);
    }
    if (next == std::chrono::steady_clock::time_point::max())
        return; // 队列空：定时器转 IDLE（对齐 vudc 的 IDLE 状态）

    timer_pending = true;
    timer.expires_at(next);
    timer.async_wait([this](const asio::error_code &ec) { on_timer(ec); });
}

void TransferScheduler::arm_next(std::uint8_t ep_address, EndpointState &state) {
    if (state.queue.empty())
        return;
    auto &front = state.queue.front();
    auto now = std::chrono::steady_clock::now();
    // 首次排期（链基准未初始化 = epoch）：从提交时刻起算，否则
    // epoch + 延迟远小于 now，max 会取 now 让首个 URB 立即响应
    if (state.last_scheduled == std::chrono::steady_clock::time_point{})
        state.last_scheduled = now;
    // 自持链：服务时刻 = max(现在, 上次计划服务时刻 + 延迟)。链在"成为
    // 队头"的时刻定死（提交/取走前一个/取消队头）：主机提前提交（池机制）
    // 时服务时刻延续链——处理耗时、定时器触发延迟、主机重提交延迟完全不
    // 进入节奏（若按实际完成时刻或提交时刻起算，间隔会多出这些开销，收流
    // 闭环限幅补不动 → 接收速率低于消费速率 → 欠载沙沙）；主机提交太慢
    //（链时刻已过）时取 now，立即服务（主机控制节奏）
    // 等时：完成节奏 = 主机数据速率（数据时长，可负 = 提前服务）；
    // bulk/interrupt：对齐帧边界（等待 0-1ms，平均 0.5ms）
    front.service_time = (front.type == EndpointAttributes::Isochronous)
                                 ? std::max(now, state.last_scheduled + front.delay)
                                 : align_to_frame(std::max(now, state.last_scheduled));
    state.last_scheduled = front.service_time;
}

std::chrono::steady_clock::time_point TransferScheduler::align_to_frame(std::chrono::steady_clock::time_point t) {
    return t + frame_alignment_delay(t);
}

void TransferScheduler::on_timer(const asio::error_code &ec) {
    struct DueUrb {
        Session *session;
        PendingUrb urb;
        std::uint8_t ep_address;
    };
    std::vector<DueUrb> due;
    {
        std::lock_guard lock(mutex);
        timer_pending = false;
        // aborted：仅 stop() 取消排期场景，连接已断不处理
        if (ec || !started)
            return;

        auto now = std::chrono::steady_clock::now();
        // 服务所有到期的队头 URB（每端点至多一个：处理中的端点跳过；
        // 不同端点可同时到期）。取出队头并标记处理中——同端点下一个
        // URB 必须等 handler 的 on_urb_done 上报后才排期
        for (auto &[addr, state] : endpoints) {
            if (state.processing || state.queue.empty())
                continue;
            auto &front = state.queue.front();
            if (front.service_time <= now) {
                due.push_back({front.session, std::move(front), addr});
                state.queue.pop_front();
                state.processing = true;
                state.processing_seqnum = due.back().urb.seqnum;
                // 下一个变队头：立即按自持链定死服务时刻（取走时定死，
                // 处理耗时/触发延迟不进入节奏；on_urb_done 只放行不重算）
                arm_next(addr, state);
            }
        }
        // 队列可能仍有未到期 URB：排期下一次
        schedule_next();
    }
    // 锁外处理（处理器可安全重入 submit()/on_urb_done()，不持锁不阻塞）：
    // 待处理 URB → 通知处理器（handler 自发送 + on_urb_done 上报）；
    // 数据已就绪 URB → 直接发送，发送即完成，立即推进串行点
    for (auto &d : due) {
        if (d.urb.processor) {
            d.urb.processor(*d.session, d.urb.ep, d.urb.seqnum, std::move(d.urb.transfer));
        }
        else {
            on_urb_completed(*d.session, std::move(d.urb.submit));
            on_urb_done(d.ep_address, d.urb.seqnum);
        }
    }
}

TransferScheduler::~TransferScheduler() {
    stop();
}

} // namespace usbipdcpp
