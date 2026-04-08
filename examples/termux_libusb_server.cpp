#include <asio.hpp>
#include <iostream>
#include <libusb-1.0/libusb.h>
#include <spdlog/spdlog.h>

#include "LibusbHandler/LibusbServer.h"

using namespace usbipdcpp;

auto listen_port_env = "USBIPDCPP_LISTEN_PORT";

std::uint16_t parse_listen_port_from_env() {
    std::uint32_t listen_port;
    auto listen_port_str = std::getenv(listen_port_env);
    if (listen_port_str == nullptr) {
        spdlog::info("{} is not defined, use default port 3240", listen_port_env);
        listen_port = 3240;
    }
    else {
        if (sscanf(listen_port_str, "%u", &listen_port) != 1) {
            SPDLOG_WARN("Parse {} as int failed, use default port 3240", listen_port_env);
            listen_port = 3240;
        }
        else {
            spdlog::info("get listen port {} from {}", listen_port, listen_port_env);
        }
    }
    return static_cast<std::uint16_t>(listen_port);
}

int main(int argc, char **argv) {
    spdlog::set_level(spdlog::level::trace);
    int err;
    if (argc <= 1) {
        SPDLOG_ERROR("You must pass an fd argument");
        return -1;
    }
    intptr_t fd;
    if (sscanf(argv[1], "%td", &fd) != 1) {
        SPDLOG_ERROR("Parse fd failed");
        return -1;
    }
    std::uint16_t listen_port = parse_listen_port_from_env();

    libusb_set_option(nullptr, LIBUSB_OPTION_WEAK_AUTHORITY);
    err = libusb_init(nullptr);
    if (err) {
        SPDLOG_ERROR("libusb_init failed: {}", libusb_strerror(err));
        libusb_exit(nullptr);
        return 1;
    }

    LibusbServer server;
    server.set_hotplug_enabled(false);  // Android 无 root 权限不支持热插拔
    asio::ip::tcp::endpoint endpoint(asio::ip::tcp::v4(), listen_port);

    // Android 模式：直接传入 fd，bind 时会临时 wrap 获取设备信息
    // 每次客户端连接时会重新 wrap fd
    auto result = server.bind_host_device_with_wrapped_fd(fd);
    if (result != DeviceOperationResult::Success) {
        SPDLOG_ERROR("bind_host_device_with_wrapped_fd failed");
        libusb_exit(nullptr);
        return -1;
    }

    server.start(endpoint);
    spdlog::info("enter any thing to stop the server");

    std::string line;
    std::getline(std::cin, line);

    server.stop();
    libusb_exit(nullptr);
    return 0;
}
