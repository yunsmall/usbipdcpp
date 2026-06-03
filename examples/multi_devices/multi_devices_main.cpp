#include <iostream>

#include "../example_utils.h"
#include "device_factory.h"
#include "Server.h"
#include "utils/StringPool.h"

int main(int argc, char **argv) {
    auto opts = make_example_options("multi_devices", "USB/IP multi-device server");
    opts.add_options()("n,count", "Number of virtual devices", cxxopts::value<int>()->default_value("10"));
    auto result = parse_example_args(opts, argc, argv);
    auto port = result["port"].as<std::uint16_t>();
    auto count = result["count"].as<int>();

    // 设置日志级别
    spdlog::set_level(spdlog::level::debug);

    SPDLOG_INFO("Starting multi-device USB/IP server");

    // 创建字符串池
    usbipdcpp::StringPool string_pool;

    // 创建虚拟设备
    auto devices = DeviceFactory::create_devices(count, string_pool);

    // 创建服务器
    usbipdcpp::Server server;

    // 将所有设备添加到服务器
    for (auto &device: devices) {
        server.add_device(std::move(device));
    }

    // 设置监听端点
    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), port};

    // 启动服务器
    server.start(endpoint);

    SPDLOG_INFO("Server started on port {} with {} devices", port, devices.size());
    SPDLOG_INFO("Use 'usbip list -r localhost --tcp-port {}' to list devices", port);
    SPDLOG_INFO("Use 'usbip attach -r localhost --tcp-port {} -b 1-X' to attach a device", port);
    SPDLOG_INFO("Press Enter to exit...");

    // 打印所有绑定的设备
    server.print_bound_devices();

    std::cin.get();

    SPDLOG_INFO("Stopping server...");
    server.stop();

    SPDLOG_INFO("Server stopped");
    return 0;
}
