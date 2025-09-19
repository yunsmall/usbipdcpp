#include "Server.h"

#include <thread>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/redirect_error.hpp>
#include <spdlog/spdlog.h>

#include "utils.h"
#include "protocol.h"
#include "type.h"
#include "Session.h"

usbipdcpp::Server::Server(std::vector<UsbDevice> &&devices) {
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
    //虽然采取等待的策略好丑，但是代码好写啊
    while (true) {
        size_t alive_session_count = 0;
        {
            std::shared_lock lock(session_list_mutex);
            if (sessions.empty()) {
                break;
            }
            alive_session_count = sessions.size();
        }
        auto wait_duration = std::chrono::seconds(1);
        spdlog::info("There are still {} sessions that have not been closed, wait for {} seconds.",
                     alive_session_count, wait_duration.count());
        std::this_thread::sleep_for(wait_duration);
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


asio::awaitable<void> usbipdcpp::Server::do_accept(asio::ip::tcp::acceptor &acceptor) {
    while (true) {
        spdlog::info("Waiting for a new connection...");

        //先创建一个Session，同时内部创建一个自己的socket
        auto session = std::make_shared<Session>(*this);

        asio::error_code ec;
        //服务器io_context接收到socket后将其转移到session内部专有的io_context
        co_await acceptor.async_accept(session->socket, asio::redirect_error(asio::use_awaitable, ec));

        {
            std::lock_guard lock(session_list_mutex);
            sessions.emplace_back(session);
        }

        if (!ec) {
            auto remote_endpoint = session->socket.remote_endpoint();
            auto remote_endpoint_name = std::format("{}:{}", remote_endpoint.address().to_string(),
                                                    remote_endpoint.port());
            spdlog::info("A new connection from {}", remote_endpoint_name);

            //函数会直接返回，但内部获取了自身的shared_ptr因此不会被析构
            //每个session启动一个线程，防止某些必须阻塞的操作影响其他设备
            session->run();
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
    spdlog::debug("有{}个可用设备", available_devices.size());
    spdlog::debug("有{}个正在使用的设备，分别为", using_devices.size());
    for (auto &dev: using_devices) {
        spdlog::debug("{}", dev.first);
    }
}
