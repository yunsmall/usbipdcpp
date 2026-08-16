#include "usbipdcpp/Server.h"

#include <thread>
#include <iostream>

#include <asio/co_spawn.hpp>
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

void usbipdcpp::Server::start(asio::ip::tcp::endpoint &ep) {
    if (asio_io_context.stopped()) {
        asio_io_context.restart();
    }

    // acceptor 作为成员保存，stop() 时关闭以释放端口，保证可以再次 start()。
    // 在 start() 所在线程同步构造：保证 stop() 时 acceptor 要么已就绪可关闭，
    // 要么尚未构造。若放在网络线程里构造，stop() 可能恰好在 open/bind 到一半
    // 时调用——close 之后网络线程继续 listen 会失败并 exit(1)；或者 stop()
    // 检查 optional 与网络线程的 emplace 并发访问，构成数据竞态。
    // bind 失败直接抛给调用者处理，而不是在网络线程里静默退出整个进程。
    acceptor.emplace(asio_io_context);
    acceptor->open(ep.protocol());
    acceptor->set_option(asio::ip::tcp::acceptor::reuse_address(true));
    acceptor->bind(ep);
    acceptor->listen();
    spdlog::info("Listening on {}:{}", ep.address().to_string(), ep.port());
    asio::co_spawn(asio_io_context, do_accept(), asio::detached);

    if (before_thread_create_callback) {
        before_thread_create_callback(ThreadPurpose::NetworkIO);
    }
    // 网络线程只负责跑 io_context 事件循环
    network_io_thread = std::thread([this]() {
        try {
            asio_io_context.run();
        } catch (const std::exception &e) {
            SPDLOG_ERROR("An unexpected exception occurs in network thread: {}", e.what());
            std::exit(1);
        }
    });
    if (after_thread_create_callback) {
        after_thread_create_callback(ThreadPurpose::NetworkIO, network_io_thread);
    }
}

void usbipdcpp::Server::stop() {
    // 先关闭 acceptor 并退出网络线程，之后再做会话清理。join 之后 do_accept
    // 协程已结束，不会再产生新的 session。
    //
    // 关闭 acceptor 释放监听端口。不关闭的话 acceptor 一直活在 do_accept
    // 协程帧里，再次 start() 时 bind 同一端口会 EADDRINUSE 失败。
    // close 后挂起的 async_accept 会以 operation_aborted 或 bad_descriptor
    // 完成（取决于平台），协程 break 退出，io_context 没有剩余工作后
    // 网络线程自然结束。
    // 这里不能调用 io_context.stop()：那会丢弃 operation_aborted 的完成回调，
    // 旧协程残留到下一次 start()，与新协程并发操作同一个 acceptor
    // （表现为 Bad file descriptor 甚至崩溃）
    if (acceptor) {
        std::error_code ignore_ec;
        acceptor->close(ignore_ec);
    }
    // joinable 检查：stop() 可能在 start() 之前被调用（线程尚未创建），
    // 此时 join 会抛 std::system_error
    if (network_io_thread.joinable()) {
        network_io_thread.join();
    }

    // 网络线程已退出，acceptor 可安全销毁（join 前销毁会与协程并发解引用竞态）
    acceptor.reset();

    // 取快照：join 期间 session 线程会调用 remove_session 修改列表，
    // 不能在持有锁或遍历列表的同时 join
    std::vector<std::shared_ptr<Session>> snapshot;
    {
        std::shared_lock lock(session_list_mutex);
        for (auto &session: sessions) {
            if (auto shared_session = session.lock()) {
                snapshot.emplace_back(std::move(shared_session));
            }
        }
    }

    // 先对所有 session 发出停止请求（让它们并行退出），再逐个 join 等死透。
    // join 本身就是"线程已结束"的证明，不需要任何计数观察
    for (auto &session: snapshot) {
        session->immediately_stop();
    }
    spdlog::info("等待所有session关闭");
    for (auto &session: snapshot) {
        session->join();
    }

    // 清理列表：线程们已在退出路径移除自己，这里清掉剩余的失效 weak_ptr
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
    std::shared_lock lock(session_list_mutex);
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
    // 停止兜底：调用方忘记调 stop() 时，这里保证所有线程在 Server 成员
    // 析构之前结束。不调 stop()（避免多余的 info 日志），只做与 stop()
    // 相同的收尾。注意 immediately_stop 内部会打 info 日志：兜底路径
    // 通常在显式 stop() 之后（sessions 已空，快照为空不触发）；若真在
    // 进程退出阶段还有活跃 session 走到这里，日志可能访问已析构的 spdlog，
    // 但此时本来就有未停干净的线程问题，可接受
    if (acceptor) {
        std::error_code ignore_ec;
        acceptor->close(ignore_ec);
    }
    if (network_io_thread.joinable()) {
        network_io_thread.join();
    }
    acceptor.reset();

    std::vector<std::shared_ptr<Session>> snapshot;
    {
        std::shared_lock lock(session_list_mutex);
        for (auto &session: sessions) {
            if (auto shared_session = session.lock()) {
                snapshot.emplace_back(std::move(shared_session));
            }
        }
    }
    for (auto &session: snapshot) {
        session->immediately_stop();
    }
    for (auto &session: snapshot) {
        session->join();
    }

    {
        std::lock_guard lock(devices_mutex);
        available_devices.clear();
        using_devices.clear();
    }
}


void usbipdcpp::Server::on_session_exit() {
    std::lock_guard lock(session_list_mutex);
    for (auto &callback: session_exit_callbacks) {
        callback();
    }
}

void usbipdcpp::Server::remove_session(Session *session) {
    std::lock_guard lock(session_list_mutex);
    // 从 sessions 列表中移除
    for (auto it = sessions.begin(); it != sessions.end();) {
        if (auto s = it->lock()) {
            if (s.get() == session) {
                it = sessions.erase(it);
                break;
            }
            else {
                ++it;
            }
        }
        else {
            // 清除已失效的 weak_ptr
            it = sessions.erase(it);
        }
    }
    // 调用回调
    for (auto &callback: session_exit_callbacks) {
        callback();
    }
}


asio::awaitable<void> usbipdcpp::Server::do_accept() {
    while (true) {
        spdlog::info("Waiting for a new connection...");

        //先创建一个Session，同时内部创建一个自己的socket
        auto session = std::make_shared<Session>(*this);

        asio::error_code ec;
        //服务器io_context接收到socket后将其转移到session内部专有的io_context
        co_await acceptor->async_accept(session->socket, asio::redirect_error(asio::use_awaitable, ec));

        if (!ec) {
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

            // 用 ec 版获取对端地址：客户端可能在 accept 完成后、本协程继续执行
            // 之前就已断开（尤其 RST），此时 remote_endpoint() 会抛异常。
            // 本协程是 detached 的，异常被 asio 吞掉后 accept 循环就此终止，
            // 后续连接无人 accept；且 session 已入列表但线程未启动，永远
            // 不会 remove_session。因此这里不能抛异常（用 ec 版），连接已死
            // 时直接丢弃 session 继续循环
            asio::error_code remote_ec;
            auto remote_endpoint = session->socket.remote_endpoint(remote_ec);
            if (remote_ec) {
                SPDLOG_DEBUG("连接在 accept 后立即断开：{}", remote_ec.message());
                continue;
            }
            auto remote_endpoint_name = std::format("{}:{}", remote_endpoint.address().to_string(),
                                                    remote_endpoint.port());
            spdlog::info("A new connection from {}", remote_endpoint_name);

            {
                std::lock_guard lock(session_list_mutex);
                sessions.emplace_back(session);
            }

            //函数会直接返回，但内部获取了自身的shared_ptr因此不会被析构
            //每个session启动一个线程，防止某些必须阻塞的操作影响其他设备。
            //run() 只在创建线程失败（资源耗尽）时抛异常，异常会传播出本协程
            //使 io_context.run() 重新抛出，由网络线程的兜底 catch 处理并退出进程
            session->run();
        }
        else if (ec == asio::error::operation_aborted || ec == asio::error::bad_descriptor) {
            // stop() 关闭 acceptor 后，挂起的 async_accept 会以 operation_aborted
            // 或 bad_descriptor 完成（取决于平台），这是正常的停止流程
            SPDLOG_INFO("Accept loop stopped");
            break;
        }
        else {
            SPDLOG_ERROR("Connection error：{}", ec.message());
            // acceptor 已无法继续接收连接，继续循环只会反复出错
            break;
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
