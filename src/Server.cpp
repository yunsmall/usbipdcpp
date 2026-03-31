#include "Server.h"

#include <thread>
#include <iostream>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/redirect_error.hpp>
#include <spdlog/spdlog.h>

#include "../include/utils/utils.h"
#include "protocol.h"
#include "type.h"
#include "Session.h"

usbipdcpp::Server::Server(std::vector<UsbDevice> &&devices, ServerNetworkConfig network_config)
    : network_config(std::move(network_config)) {
    available_devices.reserve(devices.size());
    for (auto &device: devices) {
        available_devices.emplace_back(std::make_shared<UsbDevice>(std::move(device)));
    }
}

void usbipdcpp::Server::start(asio::ip::tcp::endpoint &ep) {
    network_io_thread = std::thread([&,this]() {
        try {
            asio::ip::tcp::acceptor acceptor(asio_io_context);
            acceptor.open(ep.protocol());
            acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));

            acceptor.bind(ep);
            acceptor.listen();
            spdlog::info("Listening on {}:{}", ep.address().to_string(), ep.port());
            asio::co_spawn(
                    asio_io_context,
                    do_accept(acceptor),
                    if_has_value_than_rethrow);
            asio_io_context.run();
        } catch (const std::exception &e) {
            SPDLOG_ERROR("An unexpected exception occurs in network thread: {}", e.what());
            std::exit(1);
        }
    });
}

void usbipdcpp::Server::stop() {
    {
        std::shared_lock lock(session_list_mutex);
        for (auto &session: sessions) {
            if (auto shared_session = session.lock()) {
                shared_session->immediately_stop();
            }
        }
    }
    {
        std::unique_lock lock(session_list_mutex);
        all_sessions_closed_cv.wait(lock, [this] { return sessions.empty(); });
    }
    spdlog::info("All sessions were successfully closed");

    // spdlog::info("Successfully shut down transmissions for all devices");

    asio_io_context.stop();
    SPDLOG_TRACE("Successfully stop io_context");
    should_stop = true;
    network_io_thread.join();
}

void usbipdcpp::Server::add_device(std::shared_ptr<UsbDevice> &&device) {
    std::lock_guard lock(devices_mutex);
    available_devices.emplace_back(device);
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
    if (sessions.empty()) {
        all_sessions_closed_cv.notify_one();
    }
    // 调用回调
    for (auto &callback: session_exit_callbacks) {
        callback();
    }
}


asio::awaitable<void> usbipdcpp::Server::do_accept(asio::ip::tcp::acceptor &acceptor) {
    while (true) {
        spdlog::info("Waiting for a new connection...");

        //先创建一个Session，同时内部创建一个自己的socket
        auto session = std::make_shared<Session>(*this);

        asio::error_code ec;
        //服务器io_context接收到socket后将其转移到session内部专有的io_context
        co_await acceptor.async_accept(session->socket, asio::redirect_error(asio::use_awaitable, ec));

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

            {
                std::lock_guard lock(session_list_mutex);
                sessions.emplace_back(session);
            }
            auto remote_endpoint = session->socket.remote_endpoint();
            auto remote_endpoint_name = std::format("{}:{}", remote_endpoint.address().to_string(),
                                                    remote_endpoint.port());
            spdlog::info("A new connection from {}", remote_endpoint_name);

            //函数会直接返回，但内部获取了自身的shared_ptr因此不会被析构
            //每个session启动一个线程，防止某些必须阻塞的操作影响其他设备
#ifdef USBIPDCPP_USE_COROUTINE
            session->run_co();
#else
            session->run();
#endif
        }
        else if (ec == asio::error::operation_aborted) {
            SPDLOG_ERROR("Operation aborted：{}", ec.message());
            break;
        }
        else {
            SPDLOG_ERROR("Connection error：{}", ec.message());
        }
    }
}

bool usbipdcpp::Server::is_device_using(const std::string &busid) {
    std::shared_lock lock(devices_mutex);
    return using_devices.contains(busid);
}

void usbipdcpp::Server::try_moving_device_to_available(const std::string &busid) {
    print_devices();
    SPDLOG_INFO("尝试将{}转移到可用设备中", busid);
    std::lock_guard lock(devices_mutex);
    // SPDLOG_TRACE("成功获得两个锁");

    auto ret = using_devices.find(busid);
    if (ret != using_devices.end()) {
        SPDLOG_INFO("成功将{}转移到可用设备中", busid);
        auto& dev = ret->second;
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
