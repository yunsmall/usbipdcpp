#pragma once

#include <vector>
#include <map>
#include <shared_mutex>
#include <memory>
#include <list>
#include <thread>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <unordered_map>

#include <asio/ip/tcp.hpp>
#include <asio/awaitable.hpp>
#include <asio/co_spawn.hpp>

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
     * @brief 不阻塞地启动一个服务器（幂等：已运行时直接返回成功），内部
     * 启动网络线程运行协程式 accept 循环（async_accept）。在start前后调用
     * add_device都可以。
     * @param ep 监听地址。若端口为 0（由系统分配），start 返回后 ep 会被更新
     * 为实际监听端点，可直接用 ep 发起连接；也可用 endpoint() 查询实际端点。
     * @return 启动失败时返回错误（如端口被占），成功时无错误。不抛异常，
     * 便于无异常环境的嵌入式平台使用
     *
     * @attention 幂等仅作防御，正常用法应避免 start 后未 stop 就再次 start
     * （已在运行时直接返回，不会重新监听）。
     * @thread_safety 不可并发调用。stop() 之后可再次调用以重启。
     */
    usbipdcpp::error_code start(asio::ip::tcp::endpoint &ep);
    /**
     * @brief 当前实际监听端点。start() 用端口 0（由系统分配）启动后，
     *        用这个函数查询实际端点（含地址和端口）；未 start 时返回空端点。
     *
     * @thread_safety 内部无锁，仅应在 start() 返回后、stop() 前读取。
     */
    asio::ip::tcp::endpoint endpoint() const;

    /**
     * @brief 关闭监听并等待所有会话自析构完成，调用后所有 Session 均已停止
     *        并已析构，可安全再次 start() 或销毁 Server。
     *        顺序：置停止标志 → 锁下 cancel 挂起的异步 accept（io_context.stop()
     *        兜底）→ join 网络线程 → 关闭 acceptor 释放端口 → restart+poll
     *        清残留接受协程 → 对所有 session 发停止请求（shutdown+cancel 打断
     *        阻塞读）→ 释放快照引用，等待活跃会话计数归零（所有会话析构体
     *        执行完）→ 清空会话列表。析构前必须调用本函数（析构不自动 stop，
     *        见类注释契约）。
     *
     * @attention 幂等仅作防御（未运行时只清理残留线程后返回），正常用法应
     * 避免 stop 后未 start 就再次 stop。
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
     * @param id 要移除的 session 的 id
     *
     * @thread_safety 内部加锁，但仅应在 Session 退出路径中调用。
     */
    void remove_session(std::uint64_t id);

    ~Server();

protected:
    /**
     * @brief 协程式接受循环：在网络线程的 io_context 上调度（co_spawn），
     * 每轮创建一个会话并异步接受连接，stop() 用 acceptor_.cancel() 取消
     * （cancel 是 asio 官方的异步取消接口），io_context_.stop() 兜底。
     * 瞬态错误（对端在 accept 前重置/中止连接）继续循环，致命错误（acceptor
     * 被取消/关闭）由 stop() 的 running 标志接管退出。
     */
    asio::awaitable<void> accept_loop();

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

    // 会话连接表：Server 仅持 weak_ptr 观察，Session 生命周期由自身管理
    // （session 线程作为主线程，return 时最后一个引用释放即自析构）。
    // weak_ptr 供 stop() 锁定存活会话以停止，以及查询活跃数。
    // id 由 next_session_id 原子分配：单调递增、跨 start/stop 永不重置，
    // 保证不会出现两个客户端拿到相同 id
    std::unordered_map<std::uint64_t, std::weak_ptr<Session>> sessions;
    std::atomic<std::uint64_t> next_session_id{1};
    // 存活会话计数（原子，无需锁）：accept_loop 创建会话后立即 +1，
    // 会话析构体末尾的析构通知回调 -1。stop() 等待它归零，语义是
    // "所有曾创建的会话都已析构完成"——与 sessions 快照解耦：
    // 会话在 remove_session 与析构完成之间的窗口（已从表移除但析构未完）
    // 不会被快照漏掉，也不会被其他会话的析构提前满足等待条件。
    // 不能用 weak_ptr::expired() 判断——expired 在析构体开始时就为 true，
    // 此时析构体仍在执行，stop() 提前返回并析构 Server 会撞上仍在执行的
    // 析构回调（其访问 Server 成员，use-after-free）。
    std::condition_variable reap_cv;
    std::atomic<std::size_t> active_sessions{0};
    mutable std::mutex session_list_mutex;

    // 网络栈采用长命 io_context + acceptor：
    // start() 重新 open/bind/listen 初始化 acceptor，stop() 在 join 网络线程后
    // close 释放端口。网络线程跑协程式 accept_loop（co_spawn 到本 io_context
    // 并 run()），Session 的 socket 自持各自的 io_context（见 Session.h），
    // 不存在跨 io_context 的转移。
    // （曾用"整代重建"——每次 start() 重建 io_context + acceptor——来处理停止
    // 残留问题，并配合 Session 各自持有 io_context，在 macOS 上触发跨
    // io_context accept 失败 EINVAL，故改为长命 io_context + 协程。）
    asio::io_context asio_io_context;
    //监听 acceptor 作为成员保存，stop() 时关闭以释放端口，保证可以再次 start()。
    //访问纪律（acceptor 是 asio 共享对象，一切操作必须单线程）：
    // - open/bind/listen：start() 主线程（网络线程尚未启动，无并发）
    // - async_accept：网络线程协程，唯一操作者
    // - cancel（stop()）：主线程，在 acceptor_mutex_ 下调用（async_accept 是
    //   asio 支持跨线程取消的异步操作，cancel() 是其官方取消接口）
    // - close（stop()）：主线程，join 网络线程之后（无并发）
    asio::ip::tcp::acceptor acceptor{asio_io_context};
    // 保护 acceptor 的取消操作（stop() 在锁内 cancel 挂起的异步 accept）。
    // 协程的注册发生在 co_await 求值（await_suspend）时，无法持锁，
    // 取消的可靠性由 stop() 的 io_context_.stop() 兜底（run() 立即返回）。
    // running 为原子变量，start/stop/accept_loop 各自独立读写，无需锁。
    mutable std::mutex acceptor_mutex;
    // 停止标志：stop() 置 false 后，被取消/唤醒的 accept_loop 检查到即退出。
    // 生命周期方法要求串行调用，原子读写足够，无需额外互斥
    std::atomic_bool running = false;
    // start() 时记录实际监听端点（端口 0 时由系统分配），endpoint() 直接读。
    // stop() 后置空（未监听语义）
    asio::ip::tcp::endpoint actual_endpoint;
    //所有网络通信请运行在下面这个线程，网络通信不可运行在其他线程中
    std::thread network_io_thread;

private:
    void on_session_exit();

    // Session 析构体末尾调用（Session 是 friend）：递减存活计数并唤醒
    // stop() 的等待（计数语义见 active_sessions 的注释）。递减必须在
    // session_list_mutex 下进行：stop() 的谓词检查与进入等待以同一把锁同步，
    // 若此处不持锁，递减+通知可能落在"谓词检查与 wait 之间"的窗口而被丢弃
    // （lost wakeup），stop() 误超时
    void notify_session_destroyed() {
        {
            std::lock_guard lock(session_list_mutex);
            active_sessions.fetch_sub(1);
        }
        reap_cv.notify_all();
    }

    std::list<std::function<void()>> session_exit_callbacks;

    //可供导入的设备
    std::vector<std::shared_ptr<UsbDevice>> available_devices;
    //正在使用的设备，busid做索引只供索引使用，与usbip协议无关
    std::map<std::string, std::shared_ptr<UsbDevice>> using_devices;
    //锁available_devices和using_devices两个变量
    mutable std::shared_mutex devices_mutex;
};
}
