#include <iostream>
#include <thread>
#include <chrono>

#include "device_factory.h"
#include "Server.h"
#include "utils/StringPool.h"

int main() {
    // 设置日志级别
    spdlog::set_level(spdlog::level::debug);

    SPDLOG_INFO("Starting multi-device USB/IP server");

    // 创建字符串池
    usbipdcpp::StringPool string_pool;

    // 创建10个虚拟设备
    auto devices = DeviceFactory::create_devices(10, string_pool);

    // 创建服务器
    usbipdcpp::Server server;

    // 将所有设备添加到服务器
    for (auto &device : devices) {
        server.add_device(std::move(device));
    }

    // 设置监听端点
    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), 54324};

    // 启动服务器
    server.start(endpoint);

    SPDLOG_INFO("Server started on port 54324 with {} devices", devices.size());
    SPDLOG_INFO("Use 'usbip list -r localhost --tcp-port 54324' to list devices");
    SPDLOG_INFO("Use 'usbip attach -r localhost --tcp-port 54324 -b 1-X' to attach a device");

    // 打印所有绑定的设备
    server.print_bound_devices();

    // 运行服务器30秒
    SPDLOG_INFO("Server will run for 60 seconds...");
    for (int i = 0; i < 60; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (i % 10 == 0 && i > 0) {
            SPDLOG_INFO("Server running... {} seconds elapsed", i);
        }
    }

    SPDLOG_INFO("Stopping server...");
    server.stop();

    SPDLOG_INFO("Server stopped");
    return 0;
}
