#pragma once

#include <vector>
#include <map>
#include <shared_mutex>
#include <memory>
#include <list>
#include <thread>
#include <condition_variable>
#include <cstddef>
#include <functional>

#include <asio/ip/tcp.hpp>
#include <asio/awaitable.hpp>

#include "Device.h"


namespace usbipdcpp {
class Session;

/**
 * @brief 线程用途标识，用于线程创建前回调
 */
enum class ThreadPurpose {
    NetworkIO,      // Server的网络IO线程
    SessionMain,    // Session主线程
    SessionSender   // Session发送线程（非协程版本）
};

/**
 * @brief 服务器网络配置
 */
struct ServerNetworkConfig {
    /// socket 接收缓冲区大小（字节），0 表示使用系统默认值
    std::size_t socket_recv_buffer_size = 128 * 1024;
    /// socket 发送缓冲区大小（字节），0 表示使用系统默认值
    std::size_t socket_send_buffer_size = 128 * 1024;
    /// 是否禁用 Nagle 算法（减少小包延迟）
    bool tcp_no_delay = true;
};

class Server final {
public:
    friend class Session;

    Server() = default;
    explicit Server(const ServerNetworkConfig &network_config);
    explicit Server(std::vector<UsbDevice> &&devices, ServerNetworkConfig network_config = {});
    Server(const Server &) = delete;
    Server(Server &&) = delete;
    /**
     * @brief 不阻塞地启动一个服务器，内部启动了一个获取socket的线程。
     * 在start前后调用add_device都可以。
     * @param ep 监听地址
     */
    void start(asio::ip::tcp::endpoint &ep);
    /**
     * @brief 内部先关闭每一个session的socket，再关闭io_context。
     * 效果相当于每个客户端都调用了detach
     */
    void stop();

    /**
     * @brief 添加一个device，线程安全。不管server是否启动都可以调用
     * @param device 待添加的设备
     */
    void add_device(std::shared_ptr<UsbDevice> &&device);

    bool has_bound_device(const std::string &busid);

    size_t get_session_count();

    void print_bound_devices();

    /**
     * @brief 毫无线程安全性，请自行调用get_devices_mutex来获取锁
     * @return
     */
    [[nodiscard]] std::vector<std::shared_ptr<UsbDevice>> &get_available_devices() {
        return available_devices;
    }

    /**
     * @brief 毫无线程安全性，请自行调用get_devices_mutex来获取锁
     * @return
     */
    [[nodiscard]] std::map<std::string, std::shared_ptr<UsbDevice>> &get_using_devices() {
        return using_devices;
    }

    /**
     * @brief 操作设备数据请调用这个函数获取锁后使用
     * @return
     */
    [[nodiscard]] std::shared_mutex &get_devices_mutex() {
        return devices_mutex;
    }

    void register_session_exit_callback(std::function<void()> &&callback);

    /**
     * @brief 设置线程创建前回调，用于嵌入式平台设置线程核心亲和性等
     * @param callback 回调函数，接收线程用途标识
     */
    void set_before_thread_create_callback(std::function<void(ThreadPurpose)> &&callback) {
        before_thread_create_callback = std::move(callback);
    }

    /**
     * @brief 设置线程创建后回调，用于设置线程名称等
     * @param callback 回调函数，接收线程用途标识和线程引用
     */
    void set_after_thread_create_callback(std::function<void(ThreadPurpose, std::thread&)> &&callback) {
        after_thread_create_callback = std::move(callback);
    }

    /**
     * @brief 移除指定的 session 并触发 on_session_exit
     * @param session 要移除的 session 指针
     */
    void remove_session(Session *session);

# if !defined(USBIPDCPP_USE_COROUTINE) && defined(USBIPDCPP_ENABLE_BUSY_WAIT)
    void set_busy_wait_callback(std::function<void()> &&callback) {
        busy_wait_callback = std::move(callback);
    }
#endif

    ~Server();

protected:
    asio::awaitable<void> do_accept(asio::ip::tcp::acceptor &acceptor);

    bool is_device_using(const std::string &busid);

    void try_moving_device_to_available(const std::string &busid);

    /**
     * @brief Try to move device to using_devices, and return this device,
     * return nullptr if there is no such device in available_devices or moved failed.
     * @param busid device busid
     * @return device or nullptr when error
     */
    std::shared_ptr<UsbDevice> try_moving_device_to_using(const std::string &busid);

    void print_devices();

    std::atomic_bool should_stop = false;

    ServerNetworkConfig network_config;

    // 线程创建前回调
    std::function<void(ThreadPurpose)> before_thread_create_callback;
    // 线程创建后回调
    std::function<void(ThreadPurpose, std::thread&)> after_thread_create_callback;

# if !defined(USBIPDCPP_USE_COROUTINE) && defined(USBIPDCPP_ENABLE_BUSY_WAIT)
    // busy-wait 回调，由 LibusbServer 设置，在 sender 的 busy-wait 循环中调用
    std::function<void()> busy_wait_callback;
# endif

    std::list<std::weak_ptr<Session>> sessions;
    std::shared_mutex session_list_mutex;
    std::condition_variable_any all_sessions_closed_cv;

    //网络通信请异步使用这个io_context
    asio::io_context asio_io_context;
    //所有网络通信请运行在下面这个线程，网络通信不可运行在其他线程中
    std::thread network_io_thread;

private:
    void on_session_exit();

    std::list<std::function<void()>> session_exit_callbacks;
    std::shared_mutex exit_callbacks_mutex;

    //可供导入的设备
    std::vector<std::shared_ptr<UsbDevice>> available_devices;
    //正在使用的设备，busid做索引只供索引使用，与usbip协议无关
    std::map<std::string, std::shared_ptr<UsbDevice>> using_devices;
    //锁available_devices和using_devices两个变量
    std::shared_mutex devices_mutex;
};
}
