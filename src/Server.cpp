#include "usbipdcpp/Server.h"

#include <thread>
#include <iostream>
#include <csignal>

#include <asio/ip/tcp.hpp>
#include <asio/detached.hpp>
#include <asio/redirect_error.hpp>
#include <asio/use_awaitable.hpp>
#include <spdlog/spdlog.h>

#include "usbipdcpp/utils/utils.h"
#include "usbipdcpp/protocol.h"
#include "usbipdcpp/type.h"
#include "usbipdcpp/Session.h"

usbipdcpp::Server::Server(const ServerNetworkConfig &network_config) :
    network_config(std::move(network_config)) {
}

usbipdcpp::Server::Server(std::vector<UsbDevice> &&devices, ServerNetworkConfig network_config) :
    network_config(std::move(network_config)) {
    available_devices.reserve(devices.size());
    for (auto &device: devices) {
        available_devices.emplace_back(std::make_shared<UsbDevice>(std::move(device)));
    }
}

usbipdcpp::error_code usbipdcpp::Server::start(asio::ip::tcp::endpoint &ep) {
    // 幂等防御：已在运行时直接返回成功（正常用法应避免 start 后再 start）
    if (running) {
        return {};
    }

#ifdef SIGPIPE
    // 对端关闭连接后继续发送会触发 SIGPIPE（默认终止进程），服务器应忽略：
    // 写失败统一走 error_code 路径处理。asio 的 socket 发送已带 MSG_NOSIGNAL，
    // 此处显式忽略作为跨版本防御（重复调用无害）。
    std::signal(SIGPIPE, SIG_IGN);
#endif

    running = true;

    // acceptor 长命成员：start() 重新 open/bind/listen 初始化。在 start() 所在
    // 线程同步执行：stop() 已 join 网络线程并 close，此时无并发。失败（如端口
    // 被占）返回错误给调用方处理，并清理半初始化状态——open 后 bind/listen
    // 失败时 acceptor 仍处于打开状态，不关闭会导致下次 start() 的 open 失败
    // （不能重复 open），服务无法恢复
    asio::error_code ec;
    acceptor.open(ep.protocol(), ec);
    if (!ec) {
        acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true), ec);
    }
    if (!ec) {
        acceptor.bind(ep, ec);
    }
    if (!ec) {
        acceptor.listen(asio::socket_base::max_listen_connections, ec);
    }
    if (ec) {
        running = false;
        asio::error_code ignored;
        acceptor.close(ignored);
        return ec;
    }
    // 记录实际监听端点（端口 0 时由系统分配），endpoint() 与测试用它。
    // 此处网络线程尚未启动，local_endpoint 查询无并发；之后 endpoint() 只读
    // actual_endpoint，不再触碰 acceptor（local_endpoint 与网络线程的
    // async_accept 并发不在 asio 共享对象线程安全保证内）
    {
        asio::error_code ep_ec;
        actual_endpoint = acceptor.local_endpoint(ep_ec);
        if (ep_ec) {
            running = false;
            asio::error_code ignored;
            acceptor.close(ignored);
            return ep_ec;
        }
    }
    // 把入参更新为实际监听端点：端口 0 启动时，调用方持有的 ep 在这里变成
    // 系统分配的实际端口，可直接用于连接；固定端口启动时 ep 不变
    ep = actual_endpoint;
    spdlog::info("Listening on {}:{}", actual_endpoint.address().to_string(), actual_endpoint.port());

    // 复用 io_context：上一次 stop() 的 run() 返回后它处于停止状态，必须
    // restart 才能再次运行（否则本次 run() 立即返回，协程不会执行）
    asio_io_context.restart();

    if (before_thread_create_callback) {
        before_thread_create_callback(ThreadPurpose::NetworkIO);
    }
    // 网络线程：调度接受协程并运行 io_context（连接事件的完成处理器与协程
    // 恢复都在本线程执行）。协程被 stop() 的 cancel 取消后无待办工作，
    // run() 返回，线程退出
    try {
        network_io_thread = std::thread([this] {
            asio::co_spawn(asio_io_context, accept_loop(), asio::detached);
            asio_io_context.run();
        });
    } catch (...) {
        // 线程创建失败（如系统资源不足）：恢复未运行状态并关闭 acceptor，
        // 避免服务卡在"已标记运行但无网络线程"的中间状态
        running = false;
        asio::error_code ignored;
        acceptor.close(ignored);
        return std::make_error_code(std::errc::resource_unavailable_try_again);
    }
    if (after_thread_create_callback) {
        after_thread_create_callback(ThreadPurpose::NetworkIO, network_io_thread);
    }
    return {};
}

asio::ip::tcp::endpoint usbipdcpp::Server::endpoint() const {
    // 直接返回 start() 时记录的端点：运行期不触碰 acceptor，避免与网络
    // 线程的 async_accept 并发（共享对象并发不在 asio 保证内）。stop()
    // 后已置空（未监听语义）
    return actual_endpoint;
}

void usbipdcpp::Server::stop() {
    // 原子置停止标志：与 start()/accept_loop 的原子读写互不阻塞。
    if (!running.exchange(false)) {
        // 防御：即便从未运行，也确保不残留可 join 的线程。
        if (network_io_thread.joinable()) {
            network_io_thread.join();
        }
        return;
    }

    // 在锁下取消挂起的异步 accept（async_accept 是 asio 支持跨线程取消的
    // 异步操作，cancel() 是其官方取消接口）：完成处理器收到 operation_aborted
    // 后协程检查 running 退出。协程可能在"accept 完成与重新注册之间"的
    // 窗口错过本次取消，此时由下方 io_context_.stop() 兜底。
    {
        std::lock_guard lock(acceptor_mutex);
        asio::error_code ignored;
        acceptor.cancel(ignored);
    }

    // 兜底：即使 cancel 落在协程的注册窗口（没有挂起的 accept 可取消），
    // 也能让网络线程的 run() 立即返回，join 不会永久等待。
    // 残留的协程在下一次 start()（restart 后 run）或 io_context 析构时清理。
    asio_io_context.stop();

    if (network_io_thread.joinable()) {
        network_io_thread.join();
    }

    // 网络线程已退出，此处关闭 acceptor 无并发，安全。
    asio::error_code ignore_ec;
    acceptor.close(ignore_ec);
    actual_endpoint = {};  // 端口已失效，endpoint() 返回空（未监听语义）

    // 让残留的接受协程在 stop() 内结束：close 会取消挂起的异步 accept 并投递
    // 完成事件，而前面的 io_context_.stop() 已让 run() 返回、事件滞留队列。
    // 若放任不管，协程帧会一直持有其创建的 Session（shared_ptr）直到下一次
    // start() 或 Server 析构时 io_context 销毁才释放——届时会话析构回调访问的
    // Server 可能已析构（use-after-free）。restart 后 poll 处理这些完成事件，
    // 协程恢复后检查 running 为 false 即 co_return，帧销毁、会话引用释放。
    // poll() 一次即足够（源码依据）：asio 各后端取消/关闭挂起操作时都是
    // 同步投递完成事件——win_iocp 的 cancel/close 把完成包投递进 IOCP 队列
    // （即投递即就绪）；Linux epoll 的 cancel_ops / deregister_descriptor
    // （epoll_reactor.ipp）通过 scheduler_.post_deferred_completions 把
    // operation_aborted 的完成事件同步放入调度队列，不依赖 epoll_wait 唤醒。
    // 因此 close 之后 poll() 必然处理到全部取消完成事件。恢复的协程收到
    // operation_aborted 必然 co_return，不会与下一次 start() 的新协程并存。
    asio_io_context.restart();
    asio_io_context.poll();

    // 快照当前存活会话：weak_ptr 锁定成功说明会话仍存活，需要统一停止
    std::vector<std::shared_ptr<Session>> conns;
    {
        std::lock_guard lock(session_list_mutex);
        conns.reserve(sessions.size());
        for (auto &[session_id, weak_session]: sessions) {
            if (auto session = weak_session.lock()) {
                conns.push_back(std::move(session));
            }
        }
    }

    // 请求所有存活会话优雅停止（shutdown+cancel 打断阻塞读）
    for (auto &session: conns) {
        session->immediately_stop();
    }
    spdlog::info("等待所有session关闭");
    // 释放引用：session 线程被打断后退出，最后一个引用释放即自析构
    conns.clear();

    // 等待存活会话计数归零：accept 已停止，不再有新会话创建，计数只会单调
    // 递减到 0，语义是"所有已创建会话的析构体均已执行完"（析构体末尾回调
    // 递减，详见 Server.h 中 active_sessions 的注释）。等待与 sessions 快照
    // 解耦：会话在"remove_session 后、析构完成前"的窗口不会被漏掉。
    {
        std::unique_lock lock(session_list_mutex);
        if (!reap_cv.wait_for(lock, std::chrono::seconds(10), [&] {
                return active_sessions.load() == 0;
            })) {
            // 诊断：10 秒内未归零说明有会话引用泄漏或线程阻塞未退出，打印
            // 现场后继续等待——高负载下会话回收可能超过 10 秒，硬终止会误杀
            // 正常慢速路径，这里只留痕不中断（配合崩溃处理器与日志定位）
            SPDLOG_ERROR("stop() 等待会话析构超时（剩余 {} 个活跃会话），继续等待",
                         active_sessions.load());
            reap_cv.wait(lock, [&] { return active_sessions.load() == 0; });
        }
    }

    // 清空会话记录（已自析构的会话其记录已在析构前被 remove_session 移除）
    {
        std::lock_guard lock(session_list_mutex);
        sessions.clear();
    }
    spdlog::info("All sessions were successfully closed");
}

std::shared_ptr<usbipdcpp::UsbDevice> usbipdcpp::Server::add_device(std::shared_ptr<UsbDevice> &&device) {
    std::lock_guard lock(devices_mutex);
    available_devices.emplace_back(std::move(device));
    return available_devices.back();
}


bool usbipdcpp::Server::has_bound_device(const std::string &busid) {
    std::shared_lock lock(devices_mutex);
    //只要存了这个设备就是有设备，不管是在可用设备还是正在使用的设备
    for (auto &device: available_devices) {
        if (device->busid == busid) {
            return true;
        }
    }
    return using_devices.contains(busid);
}

size_t usbipdcpp::Server::get_session_count() {
    // 只统计仍存活的会话：已断开的会话在收尾时已被 remove_session 从表中
    // erase（失效的 weak_ptr 不残留），语义是"当前可用的连接数"
    std::lock_guard lock(session_list_mutex);
    return sessions.size();
}

void usbipdcpp::Server::print_bound_devices() {
    std::shared_lock lock(devices_mutex);

    std::size_t device_index = 1;
    std::cout << "available devices:" << std::endl;
    for (auto &device: available_devices) {
        std::cout << std::format("\tNo.{} device {}\n", device_index, device->busid);
        ++device_index;
    }
    std::cout << '\n';
    device_index = 1;
    std::cout << "using devices:" << std::endl;
    for (auto &device: using_devices) {
        std::cout << std::format("\tNo.{} device {}\n", device_index, device.first);
        ++device_index;
    }
    std::cout << std::endl;
}

void usbipdcpp::Server::register_session_exit_callback(std::function<void()> &&callback) {
    std::lock_guard lock(session_list_mutex);
    session_exit_callbacks.emplace_back(std::move(callback));
}

// bool usbipdcpp::Server::remove_device(const std::string &busid) {
//     std::lock_guard lock(devices_mutex);
//     for (auto it = available_devices.begin(); it != available_devices.end(); ++it) {
//         if ((*it)->busid == busid) {
//             available_devices.erase(it);
//             return true;
//         }
//     }
//     for (auto it = using_devices.begin(); it != using_devices.end(); ++it) {
//         if (it->first == busid) {
//             SPDLOG_ERROR("{} is being used and can't be removed");
//             return false;
//         }
//     }
//     SPDLOG_ERROR("Can't find device {}");
//     return false;
// }

usbipdcpp::Server::~Server() {
    // 析构不自动 stop：Server 生命周期由调用方管理（契约见 Server.h 类注释）。
    // 析构前必须已显式调用 stop()，否则运行中的网络线程仍 joinable 会
    // std::terminate，且存活会话析构时会通过回调访问已析构的 Server
    // （未定义行为）
}


void usbipdcpp::Server::on_session_exit() {
    std::lock_guard lock(session_list_mutex);
    for (auto &callback: session_exit_callbacks) {
        callback();
    }
}

void usbipdcpp::Server::remove_session(std::uint64_t id) {
    // 会话析构前调用（session 线程收尾），移除自身的 weak_ptr 记录
    std::lock_guard lock(session_list_mutex);
    // 幂等：同一 id 只处理一次。accept_loop 对 session->run() 抛异常的
    // 兜底清理与 session 线程正常收尾可能都调用本函数（run 内部
    // detach 后 after_thread_create_callback 若抛异常，两条路径都会到
    // 这里），第二次调用时 erase 返回 0，跳过回调，避免
    // session_exit_callbacks 重复执行
    if (sessions.erase(id) == 0) {
        return;
    }
    // 调用回调
    for (auto &callback: session_exit_callbacks) {
        callback();
    }
}


asio::awaitable<void> usbipdcpp::Server::accept_loop() {
    while (true) {
        if (!running) {
            co_return;
        }

        // 先创建会话（内部自持 io_context 与 socket，见 Session.h），连接异步
        // 接受进会话的 socket。id 原子分配单调递增（永不重复），会话收尾/析构
        // 时直接调 server 的方法移除自身并通知回收完成
        uint64_t id = next_session_id.fetch_add(1);
        auto session = std::make_shared<Session>(*this, id);
        // 创建即计入存活：会话可能从未接受连接（协程被取消），但其析构回调
        // 同样递减，stop() 的"等待归零"语义覆盖所有已创建会话
        active_sessions.fetch_add(1);

        // 注册前检查：缩小 stop() 取消丢失的窗口（最终由 io_context_.stop() 兜底）。
        // 注册发生在 co_await 求值（await_suspend）时，无法持锁进行"检查+注册"。
        if (!running) {
            co_return;
        }

        // 异步 accept：stop() 通过 acceptor_.cancel() 跨线程取消挂起的 accept
        // （cancel 是 asio 官方的异步取消接口）。协程挂起期间 session 由协程帧持有
        asio::error_code ec;
        co_await acceptor.async_accept(session->socket, asio::redirect_error(asio::use_awaitable, ec));
        if (ec) {
            // 瞬态错误（对端在 accept 前重置/中止连接）在连接频繁建立断开的
            // 高并发下很常见，直接退出会让服务端静默停止 accept，必须继续循环。
            // 致命错误（如 EMFILE 资源耗尽、acceptor 被取消/关闭）时同线程
            // close 立即释放监听端口后退出：不关端口会一直占用到 stop()，
            // 无法再次 start()（回归测试验证此行为）；stop() 的 close 是幂等
            // 兜底。acceptor 唯一操作者是本协程（网络线程），同线程 close 安全
            if (ec == asio::error::connection_reset ||
                ec == asio::error::connection_aborted ||
                ec == asio::error::interrupted ||
                // macOS 特有：客户端在连接建立后、accept 执行前就断开
                // （FIN/RST）的连接，XNU 的 accept() 返回 EINVAL；Linux 则
                // 返回已死的 fd，由后续读操作报 ECONNRESET。这是快速连断
                // 场景下的高频瞬态错误，必须继续循环，否则 accept_loop
                // 退出后服务端静默停止接受连接（macOS CI 实测
                // DisconnectRightAfterDevlistRequest 等测试在此处死亡）
                ec == asio::error::invalid_argument) {
                continue;
            }
            std::error_code close_ec;
            acceptor.close(close_ec);
            // 此处不置 running=false：stop() 的完整清理路径（cancel/join/等待
            // 会话析构归零/清空会话表）依赖 running 为 true 才走主流程，若
            // 这里提前置 false，stop() 会短路返回并跳过全部清理，导致存活
            // 会话泄漏、acceptor/端口状态残留，服务器无法干净地再次 start()
            co_return;
        }

        // 可能是 stop() 取消后仍完成的连接（竞态），丢弃并退出
        if (!running) {
            co_return;
        }

        // 设置 socket 选项
        std::error_code socket_opt_ec;
        if (network_config.socket_recv_buffer_size > 0) {
            session->socket.set_option(
                    asio::socket_base::receive_buffer_size(network_config.socket_recv_buffer_size),
                    socket_opt_ec);
            if (socket_opt_ec) {
                SPDLOG_WARN("Failed to set receive buffer size: {}", socket_opt_ec.message());
            }
        }
        if (network_config.socket_send_buffer_size > 0) {
            session->socket.set_option(
                    asio::socket_base::send_buffer_size(network_config.socket_send_buffer_size),
                    socket_opt_ec);
            if (socket_opt_ec) {
                SPDLOG_WARN("Failed to set send buffer size: {}", socket_opt_ec.message());
            }
        }
        if (network_config.tcp_no_delay) {
            session->socket.set_option(asio::ip::tcp::no_delay(true), socket_opt_ec);
            if (socket_opt_ec) {
                SPDLOG_WARN("Failed to set TCP no_delay: {}", socket_opt_ec.message());
            }
        }

        // 用 ec 版获取对端地址：客户端可能在 accept 完成后、协程继续执行
        // 之前就已断开（尤其 RST），此时 remote_endpoint() 会抛异常。detached
        // 协程的未捕获异常会导致 std::terminate，因此这里不能抛异常（用 ec
        // 版），连接已死时直接丢弃 session 继续循环
        asio::error_code remote_ec;
        auto remote_endpoint = session->socket.remote_endpoint(remote_ec);
        if (remote_ec) {
            SPDLOG_DEBUG("连接在 accept 后立即断开：{}", remote_ec.message());
            continue;
        }
        auto remote_endpoint_name = std::format("{}:{}", remote_endpoint.address().to_string(),
                                                remote_endpoint.port());
        spdlog::info("A new connection from {}", remote_endpoint_name);

        try {
            std::lock_guard lock(session_list_mutex);
            // 仅存 weak_ptr：Session 生命周期自管，Server 不持有引用
            sessions.emplace(id, session);
        } catch (...) {
            // 连接表插入失败（如内存不足）：放弃该会话——active_sessions 已
            // 在创建时递增，session 离开作用域即析构（析构回调递减），计数
            // 保持平衡。不能抛给 detached 协程（会终止接受循环），继续 accept
            continue;
        }

        //函数会直接返回，但内部获取了自身的shared_ptr因此不会被析构
        //每个session启动一个线程，防止某些必须阻塞的操作影响其他设备。
        try {
            session->run();
        } catch (...) {
            // 主线程创建失败（如系统资源不足）：会话没有线程、不会自行收尾，
            // 移除其连接记录并放弃该会话（session 离开作用域即析构，析构回调
            // 递减 active_sessions，计数保持平衡）。不能重抛：detached 协程
            // 的未捕获异常会让接受协程终止，服务器静默停止接受连接；
            // 此处继续 accept 循环，资源恢复后即可正常服务
            remove_session(id);
            continue;
        }
    }
}

bool usbipdcpp::Server::is_device_using(const std::string &busid) {
    std::shared_lock lock(devices_mutex);
    return using_devices.contains(busid);
}

void usbipdcpp::Server::try_moving_device_to_available(const std::string &busid) {
    print_devices();
    SPDLOG_DEBUG("尝试将{}转移到可用设备中", busid);
    std::lock_guard lock(devices_mutex);
    // SPDLOG_TRACE("成功获得两个锁");

    auto ret = using_devices.find(busid);
    if (ret != using_devices.end()) {
        SPDLOG_INFO("成功将{}转移到可用设备中", busid);
        auto &dev = ret->second;
        available_devices.emplace_back(std::move(dev));
        using_devices.erase(busid);
    }
    else {
        SPDLOG_WARN("找不到busid为{}的设备", busid);
    }
}

std::shared_ptr<usbipdcpp::UsbDevice> usbipdcpp::Server::try_moving_device_to_using(const std::string &wanted_busid) {
    std::lock_guard lock(devices_mutex);
    //找能用的设备
    for (auto i = available_devices.begin(); i != available_devices.end(); ++i) {
        //找到设备
        if (wanted_busid == (*i)->busid) {
            SPDLOG_INFO("将{}放入正在使用的设备中", wanted_busid);
            //将想要的设备放入正在使用的设备
            auto ret = (using_devices[wanted_busid] = std::move(*i));
            //删掉可用设备中的这个设备
            available_devices.erase(i);
            return ret;
        }
    }
    SPDLOG_WARN("找不到busid为{}的设备", wanted_busid);
    return nullptr;
}

void usbipdcpp::Server::print_devices() {
    std::shared_lock guard(devices_mutex);
    spdlog::info("有{}个可用设备", available_devices.size());
    spdlog::info("有{}个正在使用的设备，分别为", using_devices.size());
    for (auto &dev: using_devices) {
        spdlog::info("{}", dev.first);
    }
}
