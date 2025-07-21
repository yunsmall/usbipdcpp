#include "Server.h"

#include <thread>

#include <asio/co_spawn.hpp>
#include <asio/detached.hpp>
#include <asio/redirect_error.hpp>
#include <spdlog/spdlog.h>


#include "protocol.h"
#include "type.h"
#include "Session.h"

usbipcpp::Server::Server(std::vector<UsbDevice> &&devices) {
    available_devices.reserve(devices.size());
    for (auto &device: devices) {
        available_devices.emplace_back(std::make_shared<UsbDevice>(std::move(device)));
    }
}

void usbipcpp::Server::start(asio::ip::tcp::endpoint &ep) {
    network_io_thread = std::thread([&,this]() {
        asio::ip::tcp::acceptor acceptor(asio_io_context);
        acceptor.open(ep.protocol());
        acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));

        acceptor.bind(ep);
        acceptor.listen();
        asio::co_spawn(
                asio_io_context,
                do_accept(acceptor),
                asio::detached);
        asio_io_context.run();
    });
}

void usbipcpp::Server::stop() {
    asio_io_context.stop();
    should_stop = true;
    network_io_thread.join();

    {
        std::lock_guard lock(available_devices_mutex);
        std::lock_guard lock2(used_devices_mutex);
        for (auto &available_device: available_devices) {
            available_device->stop_transfer();
        }
        for (auto &val: used_devices | std::views::values) {
            val->stop_transfer();
        }
    }
    spdlog::info("成功关闭所有设备的传输");

    {
        std::shared_lock lock(session_list_mutex);
        for (const auto &session: sessions) {
            session->stop();
        }
    }
    spdlog::info("成功关闭所有会话");
}

void usbipcpp::Server::add_device(std::shared_ptr<UsbDevice> &&device) {
    std::lock_guard lock(available_devices_mutex);
    available_devices.emplace_back(device);
}

bool usbipcpp::Server::remove_device(const std::string &busid) {
    std::lock_guard lock(available_devices_mutex);
    for (auto it = available_devices.begin(); it != available_devices.end(); ++it) {
        if ((*it)->busid == busid) {
            available_devices.erase(it);
            return true;
        }
    }
    std::lock_guard lock2(used_devices_mutex);
    for (auto it = used_devices.begin(); it != used_devices.end(); ++it) {
        if (it->first == busid) {
            SPDLOG_ERROR("{}正在使用，无法移除");
            return false;
        }
    }
    SPDLOG_ERROR("找不到设备{}");
    return false;
}


asio::awaitable<void> usbipcpp::Server::do_accept(asio::ip::tcp::acceptor &acceptor) {
    while (true) {
        asio::error_code ec;
        auto socket = co_await acceptor.async_accept(asio::redirect_error(asio::use_awaitable, ec));

        if (!ec) {
            auto remote_endpoint = socket.remote_endpoint();
            auto remote_endpoint_name = std::format("{}:{}", remote_endpoint.address().to_string(),
                                                    remote_endpoint.port());
            spdlog::info("来自{}的连接", remote_endpoint_name);
            auto session = std::make_shared<Session>(*this, std::move(socket));
            SPDLOG_TRACE("尝试添加会话处理协程");
            asio::co_spawn(asio_io_context, [=]()-> asio::awaitable<void> {
                // 成功建立连接
                std::error_code ec2;
                SPDLOG_TRACE("处理会话");
                co_await session->run(ec2);
                if (ec2) {
                    SPDLOG_ERROR("来自{}的会话处理信息时出错：{}", remote_endpoint_name, ec2.message());
                }
                spdlog::info("来自{}的连接断开", remote_endpoint_name);
                co_return;
            }, asio::detached);
            SPDLOG_TRACE("成功添加会话处理协程");

            {
                std::lock_guard lock(session_list_mutex);
                sessions.emplace_back(std::move(session));
            }
        }
        else if (ec == asio::error::operation_aborted) {
            SPDLOG_ERROR("操作被取消：{}", ec.message());
            break;
        }
        else {
            SPDLOG_ERROR("连接出错：{}", ec.message());
        }
        SPDLOG_INFO("等待新客户端连接");
    }
}

void usbipcpp::Server::move_device_to_available(const std::string &busid) {
    print_devices();
    SPDLOG_INFO("尝试将{}转移到可用设备中", busid);
    std::lock_guard guard(used_devices_mutex);
    std::lock_guard guard2(available_devices_mutex);
    // SPDLOG_TRACE("成功获得两个锁");

    auto ret = used_devices.find(busid);
    if (ret != used_devices.end()) {
        SPDLOG_INFO("成功将{}转移到可用设备中", busid);
        auto &dev = ret->second;
        available_devices.emplace_back(std::move(dev));
        used_devices.erase(busid);
    }
}

void usbipcpp::Server::print_devices() {
    std::shared_lock guard(used_devices_mutex);
    std::shared_lock guard2(available_devices_mutex);
    spdlog::debug("有{}个可用设备", available_devices.size());
    spdlog::debug("有{}个正在使用的设备，分别为", used_devices.size());
    for (auto &dev: used_devices) {
        spdlog::debug("{}", dev.first);
    }
}
