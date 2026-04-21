#pragma once

#include <atomic>
#include <unordered_map>
#include <shared_mutex>

#include <chrono>
#include <thread>
#include <condition_variable>
#include <deque>

#include <asio/ip/tcp.hpp>

#include "utils/LatencyTracker.h"
#include "protocol.h"
#include "type.h"

namespace usbipdcpp {
class Server;
class AbstDeviceHandler;

/**
 * @brief 自行处理生命周期，一个连接创建一个Session，创建完服务器就对Session脱离管控了。
 * 请确保Session存活的时候Server未被析构，不然是未定义行为
 */
class Session final : public std::enable_shared_from_this<Session> {
    friend class Server;

public:
    explicit Session(Server &server);
    Session(const Session &) = delete;
    Session(Session &&) = delete;

    /**
     * @brief 该函数异步，不阻塞。内部直接向asio context提交任务，因此不用加锁。内部线程安全。
     * 请确保每个urb都需要提交返回的包
     * @param unlink
     */
    void submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink &&unlink);

    /**
     * @brief 该函数异步，不阻塞。内部直接向asio context提交任务，因此不用加锁。内部线程安全。
     * 请确保每个urb都需要提交返回的包
     * @param submit
     */
    void submit_ret_submit(UsbIpResponse::UsbIpRetSubmit &&submit);

    /**
     * @brief 置停止标志位，并且关闭socket。只能由Server和AbstDeviceHandler::trigger_session_stop调用。
     * 内部不会关闭线程，只会通知线程关闭
     */
    void immediately_stop();

    ~Session();

    LATENCY_TRACKER_MEMBER(latency_tracker);

private:
    void submit_ret_submit_impl(UsbIpResponse::UsbIpRetSubmit &&submit);
    void submit_ret_unlink_impl(UsbIpResponse::UsbIpRetUnlink &&unlink);

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
    asio::io_context session_io_context{};
    asio::ip::tcp::socket socket;


    //这个线程结束后自动析构this
    std::thread run_thread;
};
}
