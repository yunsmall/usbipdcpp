#pragma once

#include <atomic>
#include <unordered_map>
#include <shared_mutex>

#include <chrono>
#include <thread>
#include <condition_variable>
#include <deque>
#include <mutex>

#include <asio/ip/tcp.hpp>

#include "usbipdcpp/Export.h"
#include "usbipdcpp/utils/LatencyTracker.h"
#include "usbipdcpp/protocol.h"
#include "usbipdcpp/type.h"

namespace usbipdcpp {
class Server;
class AbstDeviceHandler;

/**
 * @brief 一个连接创建一个 Session，生命周期自管：session 线程持有 shared_ptr
 * 快照（run() 获取 self），线程 return 时最后一个引用释放即自析构（析构前
 * 调 server.remove_session(id) 让 Server 移除自身 weak_ptr 记录，析构体末尾
 * 调 server.notify_session_destroyed() 通知 Server 回收完成）。Server 的会话
 * 列表是 weak_ptr 不持有，stop() 释放快照引用后等待全部会话自析构完成，
 * 保证 Server 存活期间无 Session 线程残留。
 * socket 自持本会话的 io_context（见成员声明顺序），连接的生命周期完全由
 * 本会话自己管理，不依赖 Server 的 io_context。
 * 请确保 Session 存活的时候 Server 未被析构，不然是未定义行为。
 */
class USBIPDCPP_API Session final : public std::enable_shared_from_this<Session> {
    friend class Server;

public:
    /**
     * @brief 由 Server 创建。id 由 Server 分配（原子递增，永不重复），会话
     *        收尾时用它从 Server 的会话表中移除自身
     */
    Session(Server &server, std::uint64_t id);
    Session(const Session &) = delete;
    Session(Session &&) = delete;

    /**
     * @brief 该函数异步，不阻塞。把响应包入队 write_buffer 并唤醒 sender 线程，
     *        实际网络写入由 sender 线程完成。内部加锁，任意线程安全。
     * 请确保每个urb都需要提交返回的包
     * @param unlink
     */
    void submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink &&unlink);

    /**
     * @brief 该函数异步，不阻塞。把响应包入队 write_buffer 并唤醒 sender 线程，
     *        实际网络写入由 sender 线程完成。内部加锁，任意线程安全。
     * 请确保每个urb都需要提交返回的包
     * @param submit
     */
    void submit_ret_submit(UsbIpResponse::UsbIpRetSubmit &&submit);

    /**
     * @brief 只入队 write_buffer，不唤醒 sender。
     * 用于需要连续入队多条响应再统一唤醒的场景。
     */
    void enqueue_ret_unlink(UsbIpResponse::UsbIpRetUnlink &&unlink);
    void enqueue_ret_submit(UsbIpResponse::UsbIpRetSubmit &&submit);

    /**
     * @brief 唤醒 sender 线程，不塞任何数据。
     * 与 enqueue_ret_* 配合使用：先连续 enqueue，最后调一次 wakeup_sender。
     */
    void wakeup_sender();

    /**
     * @brief 置停止标志位，shutdown + cancel 打断挂起的阻塞读，并唤醒 sender 线程。
     * 仅供 Server 停止流程、AbstDeviceHandler::trigger_session_stop 和语言绑定调用。
     * 内部不会关闭线程，只会通知线程关闭
     */
    void immediately_stop();

    ~Session();

    LATENCY_TRACKER_MEMBER(latency_tracker);

private:
    /**
     * @brief 新建Session时由Server调用
     */
    void run();

    // 双缓冲队列：生产者写入 write_buffer，消费者读取 read_buffer
    // 交换时短暂加锁，大幅减少锁竞争
    std::deque<UsbIpResponse::RetVariant> write_buffer;
    std::deque<UsbIpResponse::RetVariant> read_buffer;
    mutable std::mutex swap_mutex;
    std::condition_variable data_available_cv;
    std::atomic_bool has_data{false};

    void parse_op();

    /**
     * @brief 不停地传输urb
     * @param transferring_ec 传输urb途中的ec
     */
    void transfer_loop(usbipdcpp::error_code &transferring_ec);
    void receiver(usbipdcpp::error_code &receiver_ec);
    void sender(usbipdcpp::error_code &ec);
    std::optional<UsbIpResponse::RetVariant> sender_get_data(usbipdcpp::error_code &ec);

    std::atomic_bool should_immediately_stop = false;

    //是否在传输ret_submit的阶段
    std::atomic_bool cmd_transferring = false;

    //传输过程中不允许为空，传输过程中禁止任何写入。不允许在非网络线程读，除非加锁
    std::optional<std::string> current_import_device_id = std::nullopt;
    //传输过程中不允许为空，传输过程中禁止任何写入。不允许在非网络线程读，除非加锁
    std::shared_ptr<UsbDevice> current_import_device = nullptr;
    //直接持有 handler，避免通过 device 中转
    std::shared_ptr<AbstDeviceHandler> current_handler = nullptr;
    //上面变量的值的锁
    std::shared_mutex current_import_device_data_mutex;

    Server &server;
    // 会话 id：由 Server 分配（原子递增，永不重复），收尾时用它从 Server
    // 的会话表中移除自身
    std::uint64_t id;
    // 本会话自持的处理上下文，声明在 socket 之前：析构时 socket 先析构
    // （上下文还活着），随后 io_context 才销毁，顺序安全。socket 自持上下文
    // 后连接的生命周期完全由本会话自己管理，不依赖 Server 的 io_context——
    // accept 由 Server 的协程式 async_accept 直接接受进本 socket，不存在
    // 跨 io_context 转移
    asio::io_context io_context;
    // 关联自持上下文，由 accept_loop 接受连接
    asio::ip::tcp::socket socket{io_context};
    // 保护 socket 的关闭类操作：会话收尾的 close 与 immediately_stop 的
    // cancel/shutdown 分属不同线程，close 与 cancel 并发会破坏 asio 内部
    // 状态（未定义行为），必须互斥。读写的 send/receive 不经此锁（asio
    // 文档允许与 shutdown 并发）
    std::mutex socket_mutex;


    // 主线程句柄不在成员中：run() 用局部句柄就地 detach——线程收尾不再
    // 触碰自身句柄，避免与 run() 的赋值并发访问 std::thread 对象。子线程
    // （sender）是 transfer_loop 的局部变量，创建/join/析构全在本线程内，
    // 无需成员句柄
    // sender 线程已退出标记：transfer_loop 收尾用它做限时等待（sender 可能
    // 卡在挂起的写，超时后 close 强制打断，见 transfer_loop）
    std::atomic_bool sender_done = false;
};
}
