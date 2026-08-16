#pragma once

#include <vector>
#include <map>
#include <shared_mutex>
#include <memory>
#include <list>
#include <thread>
#include <cstddef>
#include <functional>
#include <optional>

#include <asio/ip/tcp.hpp>
#include <asio/awaitable.hpp>

#include "usbipdcpp/Device.h"


namespace usbipdcpp {
class Session;

/**
 * @brief 线程用途标识，用于线程创建前回调
 */
enum class ThreadPurpose {
    NetworkIO,      // Server的网络IO线程
    SessionMain,    // Session主线程
    SessionSender   // Session发送线程
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

/**
 * @brief USB/IP 服务器
 *
 * @attention 线程安全摘要：
 *   - 构造 / start / stop / ~Server：生命周期方法，必须在同一线程串行调用
 *   - add_device / has_bound_device / get_session_count / print_bound_devices / register_session_exit_callback：
 *     内部加锁，任意线程安全
 *   - get_available_devices / get_using_devices：不锁，调用方必须自行持有 get_devices_mutex()
 *   - get_devices_mutex：始终安全，仅返回 mutex 引用
 *   - set_before_thread_create_callback / set_after_thread_create_callback：必须在 start() 之前调用
 */
class USBIPDCPP_API Server final {
public:
    friend class Session;

    Server() = default;
    explicit Server(const ServerNetworkConfig &network_config);
    explicit Server(std::vector<UsbDevice> &&devices, ServerNetworkConfig network_config = {});
    Server(const Server &) = delete;
    Server(Server &&) = delete;
    /**
     * @brief 不阻塞地启动一个服务器，内部启动网络线程运行 io_context 事件循环（含 accept 协程）。
     * 在start前后调用add_device都可以。
     * @param ep 监听地址
     *
     * @thread_safety 不可并发调用。至多调用一次（重复调用需先 stop()）。
     */
    void start(asio::ip::tcp::endpoint &ep);
    /**
     * @brief 关闭监听并等待所有会话线程结束，调用后所有 Session 均已停止，
     *        可安全再次 start() 或销毁 Server。
     *        顺序：关闭 acceptor 退出网络线程 → 对所有 session 发停止请求
     *        （shutdown+cancel 打断阻塞读）→ 逐个 join 等待线程结束 → 清空会话列表
     *
     * @thread_safety 不可并发调用。每次 start() 之后调用一次；支持 start→stop→start 循环。
     */
    void stop();

    /**
     * @brief 添加一个device，线程安全。不管server是否启动都可以调用
     * @param device 待添加的设备
     * @return 添加的设备
     *
     * @thread_safety 内部加锁，任意线程安全。
     */
    std::shared_ptr<UsbDevice> add_device(std::shared_ptr<UsbDevice> &&device);

    /**
     * @thread_safety 内部加锁，任意线程安全。
     */
    bool has_bound_device(const std::string &busid);

    /**
     * @thread_safety 内部加锁，任意线程安全。
     */
    size_t get_session_count();

    /**
     * @thread_safety 内部加锁，任意线程安全。
     */
    void print_bound_devices();

    /**
     * @brief 毫无线程安全性，请自行调用get_devices_mutex来获取锁
     * @return
     *
     * @thread_safety 调用方必须持有 get_devices_mutex() 的读锁或写锁。
     */
    [[nodiscard]] std::vector<std::shared_ptr<UsbDevice>> &get_available_devices() {
        return available_devices;
    }

    /**
     * @brief 毫无线程安全性，请自行调用get_devices_mutex来获取锁
     * @return
     *
     * @thread_safety 调用方必须持有 get_devices_mutex() 的读锁或写锁。
     */
    [[nodiscard]] std::map<std::string, std::shared_ptr<UsbDevice>> &get_using_devices() {
        return using_devices;
    }

    /**
     * @brief 操作设备数据请调用这个函数获取锁后使用
     * @return
     *
     * @thread_safety 始终安全（仅返回引用）。
     */
    [[nodiscard]] std::shared_mutex &get_devices_mutex() const {
        return devices_mutex;
    }

    /**
     * @thread_safety 内部加锁，任意线程安全。
     */
    void register_session_exit_callback(std::function<void()> &&callback);

    /**
     * @brief 设置线程创建前回调，用于嵌入式平台设置线程核心亲和性等
     * @param callback 回调函数，接收线程用途标识
     *
     * @thread_safety 必须在 start() 之前调用。
     */
    void set_before_thread_create_callback(std::function<void(ThreadPurpose)> &&callback) {
        before_thread_create_callback = std::move(callback);
    }

    /**
     * @brief 设置线程创建后回调，用于设置线程名称等
     * @param callback 回调函数，接收线程用途标识和线程引用
     *
     * @thread_safety 必须在 start() 之前调用。
     */
    void set_after_thread_create_callback(std::function<void(ThreadPurpose, std::thread&)> &&callback) {
        after_thread_create_callback = std::move(callback);
    }

    /**
     * @brief 移除指定的 session 并触发 on_session_exit
     * @param session 要移除的 session 指针
     *
     * @thread_safety 内部加锁，但仅应在 Session 退出路径中调用。
     */
    void remove_session(Session *session);

    ~Server();

protected:
    asio::awaitable<void> do_accept();

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

    ServerNetworkConfig network_config;

    // 线程创建前回调
    std::function<void(ThreadPurpose)> before_thread_create_callback;
    // 线程创建后回调
    std::function<void(ThreadPurpose, std::thread&)> after_thread_create_callback;

    std::list<std::weak_ptr<Session>> sessions;
    mutable std::shared_mutex session_list_mutex;

    //网络通信请异步使用这个io_context
    asio::io_context asio_io_context;
    //监听 acceptor 作为成员保存：stop() 时需显式关闭释放端口，否则 stop 后
    //再次 start() 会与旧协程中的 acceptor 冲突导致 bind 失败。
    //访问纪律（acceptor 是 asio 共享对象，一切操作必须与 async_accept 同线程）：
    // - open/bind/listen：start() 主线程（网络线程尚未启动，无并发）
    // - async_accept：网络线程协程
    // - close：post 到网络线程（跨线程 close 与 async_accept 并发操作 reactor
    //   的 fd→state 映射，数据竞态曾导致 CI 崩溃）
    // - 析构（reset）：stop() 主线程，join 网络线程之后（协程已死，无并发）
    std::optional<asio::ip::tcp::acceptor> acceptor;
    //所有网络通信请运行在下面这个线程，网络通信不可运行在其他线程中
    std::thread network_io_thread;

private:
    void on_session_exit();

    std::list<std::function<void()>> session_exit_callbacks;

    //可供导入的设备
    std::vector<std::shared_ptr<UsbDevice>> available_devices;
    //正在使用的设备，busid做索引只供索引使用，与usbip协议无关
    std::map<std::string, std::shared_ptr<UsbDevice>> using_devices;
    //锁available_devices和using_devices两个变量
    mutable std::shared_mutex devices_mutex;
};
}
