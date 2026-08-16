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
 * @brief 一个连接创建一个 Session，生命周期由 shared_ptr 管理：session 线程自身
 * 持有 self 保证运行期间不被析构；Server 的会话列表是 weak_ptr 不持有，
 * stop()/~Server 通过快照临时持有以便 join。
 * Session 线程句柄遵循 joinable + join 的标准停止协议：Server::stop() 逐个 join，
 * 客户端主动断开时线程在退出前自行 detach（常规路径唯一的 detach 点，
 * ~Session 的兜底分支除外）。
 * 请确保 Session 存活的时候 Server 未被析构，不然是未定义行为。
 */
class USBIPDCPP_API Session final : public std::enable_shared_from_this<Session> {
    friend class Server;

public:
    explicit Session(Server &server);
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

    /**
     * @brief 等待本 session 线程结束（阻塞）。由 Server::stop() 和 ~Server 兜底调用。
     * 若线程已因客户端断开而自行 detach（句柄已被取走），则立即返回。
     */
    void join();

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
    asio::io_context session_io_context{};
    asio::ip::tcp::socket socket;


    // session 主线程。停止协议（joinable + join）：
    // - Server::stop() 通过 join() 等待本线程结束
    // - 客户端主动断开时，本线程在退出前自行 detach
    // 锁内取出句柄、锁外 join/detach，避免两个路径并发操作 std::thread 对象
    std::thread run_thread;
    std::mutex run_thread_mutex;
};
}
