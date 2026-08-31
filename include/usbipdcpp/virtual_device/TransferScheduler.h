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

class TransferResponder;

/**
 * @brief 传输帧调度器（对齐内核 usbip vudc_transfer.c 的帧调度思想）
 *
 * 虚拟设备没有真实 USB 总线：URB 到达就立即响应的话，主机驱动的
 * "完成→重提交"循环失去总线节流会失控（实测 UAC 虚拟设备超发 158 倍）。
 * 本类用事件驱动的定时器模拟帧节奏，控制 URB 的服务时机。
 *
 * 在自定义 VirtualInterfaceHandler 中的典型用法：
 *   0. 实例由 VirtualDeviceHandler 持有，handler 通过
 *      device_handler->get_transfer_scheduler() 访问
 *   1. 构造时传入设备总线速度（由 VirtualDeviceHandler 从 UsbDevice 取，
 *      端点事务间隔按它推导）
 *   2. on_new_connection() 里 start()，on_disconnection() 里 stop()——
 *      stop() 丢弃未完成 URB 并停线程，重连后 start() 状态干净
 *   3. 收到 iso/bulk/interrupt URB 后按数据是否已就绪选择提交方式：
 *      - 数据已就绪（如麦克风 IN 已生成 PCM，或纯延迟响应）：
 *        submit(session, ep, type, num_iso_packets, ret_submit, data_duration)
 *        ——调度器到服务时刻直接把响应发出去
 *      - 需要 handler 处理（如扬声器 OUT 收流写 sink、bulk 数据处理）：
 *        submit(session, ep, type, data_duration, seqnum, transfer, processor)
 *        ——服务时刻回调 processor 通知 handler 开始处理
 *   4. 处理器版：processor 回调中处理数据（内存级操作，微秒级），处理完
 *      session.submit_ret_submit(...) 发响应 + on_urb_done(ep_address, seqnum)
 *      上报完成——同端点下一个 URB 等上报后才服务
 *   5. 收到 UNLINK 时 cancel(seqnum)（应答语义与内核 vudc_rx.c 一致）
 *   Control URB 不走调度器（控制请求必须快速，对齐 vudc 的 ep0）
 *
 * 调度语义：
 *   - 等时：按数据时长延迟服务——完成节奏 = 主机数据速率，等效内核
 *     gadget u_audio 把 URB 的 N 个包分布在 N 帧完成。data_duration 由
 *     调用方按 transfer_buffer_length / 数据速率折算（可为负 = 提前服务，
 *     收流速率闭环用它修正主机固定提交开销）
 *   - Bulk / Interrupt：服务时刻对齐帧边界（等待 0-1ms），每帧每端点至多
 *     服务一个 URB（vudc 帧驱动语义）
 *   - 端点间独立：一个端点挂起（处理中/NAK）不影响其他端点
 *
 * 线程模型：自持 io_context + 一个调度线程，连接建立时 start()、断开时
 * stop()。队列空时定时器不排期（对齐 vudc 的 IDLE 状态），新 URB 入队
 * 重新排期（对齐 v_kick_timer）。session 由调用方保证存活至 stop()。
 */
class USBIPDCPP_API TransferScheduler {
public:
    /**
     * @brief URB 处理回调（中断语义，vudc 帧服务）
     *
     * 调度线程在服务时刻调用，把数据（TransferHandle 右值引用）交给 handler
     * 开始处理——写 sink / 填数据 / 闭环统计。handler 处理完后自行
     * session.submit_ret_submit 发送响应（与 handler 其他响应路径一致，任意
     * 线程安全），并调用 on_urb_done 上报完成（同端点串行依赖它）。
     *
     * @note 回调在调度线程执行、相当于中断处理程序：必须快速返回（内存级
     * 操作，如 UAC 的写缓冲/生成 PCM，微秒级），禁止阻塞、禁止耗时任务——
     * 否则会卡住调度器、拖累其他端点（同 vudc 的 transfer 同步 memcpy 语义）。
     * 耗时工作（磁盘 IO 等）由 handler 自行安排，不得在回调内等待。
     * 回调不持调度器锁，可安全重入 submit()/on_urb_done()。
     * 闭包不捕获 TransferHandle（它不可拷贝而 std::function 要求回调可拷贝；
     * Android NDK 的 libc++ 没有 move_only_function，参数传递是跨平台唯一
     * 干净写法）
     */
    using UrbProcessCallback = std::function<void(TransferResponder &responder, const UsbEndpoint &ep,
                                                  std::uint32_t seqnum, TransferHandle &&transfer)>;

    /**
     * @brief 构造调度器
     * @param speed 设备总线速度（设备级属性，由 VirtualDeviceHandler 构造时
     * 从 UsbDevice 传入），端点事务间隔按它推导
     */
    explicit TransferScheduler(UsbSpeed speed) : speed(speed) {}

    /**
     * @brief 连接建立时启动调度线程
     * @note 幂等；断连后再次 start 前必须先 stop
     */
    void start();

    /**
     * @brief 连接断开时停止
     *
     * 丢弃未完成 URB（连接已断，不响应，对齐 vudc 的 stop_activity 清空
     * 队列）、取消定时器、停止调度线程。
     * @note 幂等，析构时自动调用
     */
    void stop();

    ~TransferScheduler();

    /**
     * @brief 提交一个待处理的 URB（通知语义）
     *
     * 不立即处理，由调度线程在服务时刻调用 processor 通知 handler 开始处理
     * （transfer 以右值引用交出、seqnum 由调度器传入），handler 处理完后
     * 自行 submit_ret_submit 发送响应并调用 on_urb_done 上报完成。
     * 限速在"服务时机"：等时按 data_duration 延迟服务（完成节奏 = 主机数据
     * 速率；data_duration 由调用方按 transfer_buffer_length/数据速率 折算，
     * 含收流速率闭环修正，负 = 提前服务）；bulk/interrupt 对齐帧边界服务
     * （等待 0-1ms）。同端点下一个 URB 在 on_urb_done 后经相同延迟才被服务。
     * @param session 会话，由调用方保证存活至 stop()（连接断开先 stop 再销毁会话）
     * @param ep 端点（服务时传给 processor）
     * @param type 传输类型（等时按数据时长限速，其余帧对齐）
     * @param data_duration 等时：数据对应的音频时长；bulk/interrupt 忽略
     * @param seqnum URB 序号（取消匹配 + on_urb_done 校验）
     * @param transfer 数据（服务时以右值引用交给 processor）
     * @param processor 处理回调（中断语义，见 UrbProcessCallback 注释）
     * @note 连接已断（stop 后）时丢弃，不处理不响应。
     * 任意线程安全（处理器回调内可安全重入 submit）
     */
    void submit(TransferResponder &responder, const UsbEndpoint &ep, EndpointAttributes type,
                std::chrono::microseconds data_duration, std::uint32_t seqnum, TransferHandle transfer,
                UrbProcessCallback processor);

    /**
     * @brief 提交一个数据已填充完毕的传输请求，由调度器按传输类型控制响应时机
     *
     * 端点版：事务间隔由调度器按端点 bInterval 与设备速度推导。
     * 显式间隔版：由调用方给出事务间隔（如测试用非标准间隔）。
     * 等时：按 data_duration（默认 = num_iso_packets × 间隔）延迟响应，同一
     * 端点 URB 串行完成；num_iso_packets <= 0 时不占调度窗口，立即响应。
     * data_duration 指定时替代 包数×间隔——完成速率 = 主机数据速率，设备
     * 本地时钟与主机时钟的频偏不累积（自适应 OUT 端点跟随主机数据量的正确
     * 行为，见 UacAudioStreamingSinkHandler 注释）。可为负：负 = 提前响应
     * （水位闭环修正主机每 URB 的固定提交开销用，延迟 ≤ 0 时立即完成）。
     * @param session 会话，由调用方保证存活至 stop()
     * @param ep 端点（端点版）／@param ep_address 端点地址（显式间隔版）
     * @param type 传输类型
     * @param num_iso_packets 等时包数——仅等时传输使用，其他类型传 0
     * @param submit 已填充的响应（服务时刻到达时直接发送）
     * @param data_duration 等时：数据对应的音频时长，默认 = 包数 × 间隔
     * @note 连接已断（stop 后）时丢弃，不响应
     */
    void submit(TransferResponder &responder, const UsbEndpoint &ep, EndpointAttributes type, int num_iso_packets,
                UsbIpResponse::UsbIpRetSubmit &&submit,
                std::chrono::microseconds data_duration = std::chrono::microseconds::zero());
    void submit(TransferResponder &responder, std::uint8_t ep_address, EndpointAttributes type,
                std::chrono::microseconds interval, int num_iso_packets, UsbIpResponse::UsbIpRetSubmit &&submit,
                std::chrono::microseconds data_duration = std::chrono::microseconds::zero());

    /**
     * @brief handler 处理完 URB 后调用（通知语义的核心）
     *
     * 解除该端点的处理中状态，放行队列中下一个 URB（其服务时刻在提交时已
     * 定死，不重算——处理耗时/触发延迟不进入节奏）。处理慢（超过链上服务
     * 时刻）时，下一个 URB 在放行后立即服务（串行保护，节奏随即恢复）。
     * @param ep_address 端点地址
     * @param seqnum 处理完成的 URB 序号——须匹配当前处理中的 URB，不匹配时
     * 忽略（处理器已完成收尾的场景）
     * @note 任意线程安全（处理可异步）
     */
    void on_urb_done(std::uint8_t ep_address, std::uint32_t seqnum);

    /**
     * @brief 取消 seqnum 对应的待处理 URB（UNLINK 用）
     * @param seqnum 要取消的 URB 序号
     * @return true = 取消成功（调用方应答 RET_UNLINK(-ECONNRESET)，且不再发
     * RET_SUBMIT）；false = URB 已处理中/已完成或不存在（调用方应答
     * RET_UNLINK(0)）。与内核 vudc_rx.c 的 CMD_UNLINK 语义一致
     */
    bool cancel(std::uint32_t seqnum);

    /**
     * @brief 按 USB 规范 bInterval 语义计算端点事务间隔
     * @param ep 端点（取 bInterval）
     * @param speed 设备总线速度
     * @return 高速 2^(bInterval-1) × 125µs；全速/低速 bInterval × 1ms；
     * Super bInterval × 125µs
     */
    static std::chrono::microseconds endpoint_interval(const UsbEndpoint &ep, UsbSpeed speed);

protected:
    /**
     * @brief URB 完成时的响应出口
     *
     * 默认提交到当前会话的网络发送队列（submit_ret_submit，任意线程安全，
     * 实现见 TransferScheduler.cpp）。
     * @note 子类可 override 拦截响应（如测试收集）
     */
    virtual void on_urb_completed(TransferResponder &current_session, UsbIpResponse::UsbIpRetSubmit &&submit);

private:
    struct PendingUrb {
        std::chrono::steady_clock::time_point service_time; // 服务时刻（排期用）：端点空闲时由 arm_next 计算
        // 非空 = 待处理：服务时调用处理器（通知语义，handler 自发送 +
        // on_urb_done 上报）。空 = 数据已就绪：服务时直接发送 submit
        UrbProcessCallback processor;
        UsbIpResponse::UsbIpRetSubmit submit; // 数据已就绪版使用
        UsbEndpoint ep;                       // 处理器参数（处理器版使用）
        EndpointAttributes type;              // 服务时刻计算：等时按 delay，其余帧对齐
        std::uint32_t seqnum = 0;             // 取消匹配 + on_urb_done 校验
        std::chrono::microseconds delay{};    // 等时：服务时刻 = 串行点 + 数据时长（bulk/int 忽略）
        TransferHandle transfer;              // 处理器版：服务时 move 给处理器
        TransferResponder *responder = nullptr;           // 服务时传给处理器/发送（提交方保证存活至 stop）
    };
    struct EndpointState {
        std::deque<PendingUrb> queue;
        // 上次计划服务时刻（提交时定死的自持链基准）：服务间隔只由
        // data_duration（含收流闭环修正）决定——处理耗时、定时器触发延迟
        // 不进入节奏（on_urb_done 只放行不重算）。若按实际完成时刻推进，
        // 处理耗时 + 触发延迟会加进服务间隔（实测 ~1.15ms），闭环限幅
        // 补不动导致接收速率低于消费速率 → 欠载沙沙
        std::chrono::steady_clock::time_point last_scheduled{};
        bool processing = false;            // 处理中：已通知 handler、等待 on_urb_done（同端点同一时刻至多一个）
        std::uint32_t processing_seqnum = 0; // 处理中的 seqnum（on_urb_done 校验）
    };

    void run();

    /**
     * @brief 持锁调用：定时器未排期（IDLE）时 post 到调度线程排期
     *
     * 已排期（RUNNING）时什么都不做——新 URB 只入队，到期处理时会扫到。
     * 队列空则保持 IDLE。排期一旦生效不取消（对齐 vudc 的 v_timer）
     */
    void kick();

    /**
     * @brief 调度线程：处理 kick 的 post 请求，按最早的队头服务时刻排期定时器
     */
    void schedule_on_thread();

    /**
     * @brief 持锁调用：按最早的队头服务时刻排期定时器；队列空则不排期（IDLE）
     * @note 只能在调度线程调用：跨线程注册 timer 的 async_wait 在 Windows 上
     * 偶发不唤醒（run() 无事件挂起时的注册竞态），统一在调度线程注册则无此问题
     */
    void schedule_next();

    /**
     * @brief 持锁调用：端点空闲（无处理中）且队列非空时，按自持链计算队头
     * 服务时刻并推进链基准
     *
     * 服务时刻 = max(现在, 上次计划服务时刻) 之后：等时 + 数据时长；
     * bulk/interrupt 对齐帧边界。提交时定死，后续 on_urb_done 不重算
     * （处理耗时/触发延迟不进入节奏）
     */
    void arm_next(std::uint8_t ep_address, EndpointState &state);

    /**
     * @brief 服务时刻的帧边界对齐（1ms 网格，vudc 帧定时器语义）
     */
    static std::chrono::steady_clock::time_point align_to_frame(std::chrono::steady_clock::time_point t);

    void on_timer(const asio::error_code &ec);

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
    bool started = false;
    // 定时器是否已排期（RUNNING）：排期生效期间新 URB 只入队不重排，
    // 到期处理（on_timer）后按队列重排；队列空转 IDLE（false）
    bool timer_pending = false;
};

} // namespace usbipdcpp