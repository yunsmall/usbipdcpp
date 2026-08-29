#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <asio/io_context.hpp>
#include <asio/steady_timer.hpp>

#include "usbipdcpp/Endpoint.h"
#include "usbipdcpp/Export.h"
#include "usbipdcpp/protocol.h"

namespace usbipdcpp {

class Session;

/// 传输帧调度器（对齐内核 usbip vudc_transfer.c 的帧调度思想）
///
/// vudc 用一个帧定时器模拟 USB 总线：网络 URB 入队后由定时器按帧推进，
/// 传输完成的节奏就是总线节奏。虚拟设备没有真实总线，URB 完成 = 服务端
/// 网络响应；若 URB 到达就立即响应，主机驱动的"完成→重提交"循环失去
/// 总线节流会失控（实测 UAC 虚拟设备超发 158 倍）。本类用事件驱动的
/// 定时器模拟帧节奏，按传输类型调度：
///
/// - 等时（Isochronous）：每个 URB 的 N 个包分布在 N 个帧（microframe）
///   里完成，延迟 num_iso_packets × 端点事务间隔（bInterval 推导，见
///   endpoint_interval）后响应；同一端点 URB 串行完成（等价 vudc 的
///   already_seen 每帧每端点只服务一个 URB），平均速率恒为
///   1 URB / (num_iso_packets × 间隔)。
/// - Bulk / Interrupt：对齐 vudc 帧驱动语义（v_timer 每 1ms tick 一次、
///   already_seen 每帧每端点只服务一个 URB），URB 对齐到下一个帧边界完成
///   （等待 0-1ms）、同端点串行；端点间独立，一个端点挂起（NAK）不影响
///   其他端点。带宽预算对虚拟设备无实际意义（网络带宽才是瓶颈），不实现
/// - Control：不走帧调度，立即响应（控制请求必须快速，对齐 vudc 的 ep0）
///
/// 线程模型：自持 io_context + 一个调度线程，连接建立时 start()、断开时
/// stop()（对齐 vudc 的 v_start_timer / v_stop_timer）。队列空时定时器不
/// 排期（对齐 vudc 的 IDLE 状态），新 URB 入队重新排期（对齐 v_kick_timer）。
/// 调度线程回调里只做队列操作与响应入队——数据在 URB 到达时已由 handler
/// 填好，调度器不执行数据生产，不会阻塞网络线程。
class USBIPDCPP_API TransferScheduler {
public:
    /// speed 为设备总线速度（设备级属性，由 VirtualDeviceHandler 构造时
    /// 从 UsbDevice 传入），端点事务间隔据此推导
    explicit TransferScheduler(UsbSpeed speed) : speed(speed) {}

    /// 连接建立时启动调度线程。断连后再次 start 前必须先 stop
    void start(Session &current_session);

    /// 连接断开时停止：丢弃未完成 URB（连接已断，不响应，对齐 vudc 的
    /// stop_activity 清空队列）、取消定时器、停止调度线程。幂等，析构时自动调用
    void stop();

    ~TransferScheduler();

    /// 提交一个数据已填充完毕的传输请求，由调度器按传输类型控制响应时机。
    /// 端点版：事务间隔由调度器按端点 bInterval 与设备速度推导。
    /// 显式间隔版：由调用方给出事务间隔（如测试用非标准间隔）。
    /// num_iso_packets 为等时包数——仅等时传输使用，其他类型传 0。
    /// 等时：延迟 num_iso_packets × 间隔后响应，同一端点 URB 按提交顺序
    /// 串行完成；num_iso_packets <= 0 时不占调度窗口，立即响应。
    /// data_duration 为本次 URB 数据对应的实际时长（默认 0 = 未指定）：
    /// 指定时按它延迟（替代 包数×间隔）——完成速率 = 主机数据速率，
    /// 设备本地时钟与主机时钟的频偏不累积（自适应 OUT 端点跟随主机
    /// 数据量的正确行为，见 UacAudioStreamingSinkHandler 注释）。
    /// 可为负：负 = 提前响应（水位闭环修正主机每 URB 的固定提交开销用，
    /// 延迟 ≤ 0 时立即完成）。连接已断（stop 后）时丢弃，不响应
    void submit(const UsbEndpoint &ep, EndpointAttributes type, int num_iso_packets,
                UsbIpResponse::UsbIpRetSubmit &&submit,
                std::chrono::microseconds data_duration = std::chrono::microseconds::zero());
    void submit(std::uint8_t ep_address, EndpointAttributes type, std::chrono::microseconds interval,
                int num_iso_packets, UsbIpResponse::UsbIpRetSubmit &&submit,
                std::chrono::microseconds data_duration = std::chrono::microseconds::zero());

    /// 取消 seqnum 对应的待处理 URB（UNLINK 用）。返回 true 表示取消成功
    ///（调用方应答 RET_UNLINK(-ECONNRESET)，且不再发 RET_SUBMIT）；false
    /// 表示 URB 已完成或不存在（调用方应答 RET_UNLINK(0)）。
    /// 与内核 vudc_rx.c 的 CMD_UNLINK 语义一致
    bool cancel(std::uint32_t seqnum);

    /// 按 USB 规范 bInterval 语义计算端点事务间隔：
    /// 高速 2^(bInterval-1) × 125µs；全速/低速 bInterval × 1ms；Super bInterval × 125µs
    static std::chrono::microseconds endpoint_interval(const UsbEndpoint &ep, UsbSpeed speed);

protected:
    /// URB 完成时的响应出口：默认提交到当前会话的网络发送队列
    ///（submit_ret_submit，任意线程安全，实现见 TransferScheduler.cpp）。
    /// 调度线程与提交线程都会调用，参数 current_session 在调用期间保证存活
    ///（成员 session 指针可能已被 stop 清空，必须用参数传递）；子类可
    /// override 拦截响应（如测试收集）
    virtual void on_urb_completed(Session &current_session, UsbIpResponse::UsbIpRetSubmit &&submit);

private:
    void run();

    /// 持锁调用：定时器未排期（IDLE）时 post 到调度线程排期；
    /// 已排期（RUNNING）时什么都不做——新 URB 只入队，到期处理时会扫到。
    /// 队列空则保持 IDLE。排期一旦生效不取消（对齐 vudc 的 v_timer）
    void kick();

    /// 调度线程：处理 kick 的 post 请求，按最早的队头 deadline 排期定时器
    void schedule_on_thread();

    /// 持锁调用：按最早的队头 deadline 排期定时器；队列空则不排期（IDLE）。
    /// 只能在调度线程调用：跨线程注册 timer 的 async_wait 在 Windows 上偶发
    /// 不唤醒（run() 无事件挂起时的注册竞态），统一在调度线程注册则无此问题
    void schedule_next();

    void on_timer(const asio::error_code &ec);

    struct PendingUrb {
        std::chrono::steady_clock::time_point deadline;
        UsbIpResponse::UsbIpRetSubmit submit;
    };
    struct EndpointState {
        std::deque<PendingUrb> queue;
        // 上一 URB 的完成时刻：同一端点 URB 串行完成（对齐 vudc 的
        // already_seen），新 URB 完成时刻 = max(现在, 上一完成时刻) + N×间隔
        std::chrono::steady_clock::time_point last_deadline{};
    };

    std::mutex mutex;
    std::unordered_map<std::uint8_t, EndpointState> endpoints;
    // 设备总线速度：端点事务间隔按它推导（高速 125µs×2^(bInterval-1) 等）
    UsbSpeed speed;
    asio::io_context io_context;
    // run() 保活：io_context 无 pending 操作时 run() 会立即返回（之后才
    // 到达的 post/timer 注册无人处理，导致调度死等）。work_guard 保证
    // run() 常驻，直到 stop() 显式 reset 放行。用 unique_ptr 持有：
    // work_guard 带引用成员不可重新赋值
    std::unique_ptr<asio::executor_work_guard<asio::io_context::executor_type>> work_guard;
    asio::steady_timer timer{io_context};
    std::thread thread;
    Session *session = nullptr;
    bool started = false;
    // 定时器是否已排期（RUNNING）：排期生效期间新 URB 只入队不重排，
    // 到期处理（on_timer）后按队列重排；队列空转 IDLE（false）
    bool timer_pending = false;
};

} // namespace usbipdcpp
