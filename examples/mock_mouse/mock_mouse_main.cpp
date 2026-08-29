#include <atomic>
#include <cmath>
#include <iostream>
#include <numbers>
#include <thread>

#include "../example_utils.h"
#include "usbipdcpp/Server.h"
#include "usbipdcpp/virtual_device/SimpleVirtualDeviceHandler.h"
#include "usbipdcpp/virtual_device/devices/RelativeMouseHandler.h"

using namespace usbipdcpp;

int main(int argc, char **argv) {
    auto opts = make_example_options("mock_mouse", "USB/IP virtual mouse device");
    opts.add_options()("circle", "Move cursor in a circle pattern");
    auto result = parse_example_args(opts, argc, argv);
    auto port = result["port"].as<std::uint16_t>();
    auto busid = result["busid"].as<std::string>();
    bool circle_mode = result.count("circle") > 0;

    spdlog::set_level(spdlog::level::trace);

    StringPool string_pool;

    // make_interface 返回已绑定 RelativeMouseHandler 的完整鼠标接口
    auto mock_mouse = UsbDevice::make(busid, 0x1234, 0x5678,
                                      {RelativeMouseHandler::make_interface(string_pool, 0x81)},
                                      1, 1, 0, "/usbipdcpp/mock_mouse", UsbSpeed::Low, 0xabcd);
    mock_mouse->with_handler<SimpleVirtualDeviceHandler>(string_pool)->setup_interface_handlers();

    auto &mouse = *std::dynamic_pointer_cast<RelativeMouseHandler>(mock_mouse->interfaces[0].handler);

    Server server;
    server.add_device(std::move(mock_mouse));

    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), port};

    if (auto ec = server.start(endpoint); ec) {
        SPDLOG_ERROR("服务器启动失败：{}", ec.message());
        return 1;
    }

    SPDLOG_INFO("Mock mouse started on port {}, busid {}", port, busid);
    if (circle_mode)
        SPDLOG_INFO("Mode: circle — cursor will trace a circle");
    else
        SPDLOG_INFO("Mode: toggle left button");
    SPDLOG_INFO("Connect with: usbip attach -r <host> -b {}", busid);
    SPDLOG_INFO("Press Enter to exit...");

    std::atomic_bool running{true};
    std::thread mouse_thread([&]() {
        if (circle_mode) {
            const double step = 5.0;
            const int steps_per_circle = 60;
            int i = 0;
            while (running) {
                double angle = 2.0 * std::numbers::pi * i / steps_per_circle;
                auto dx = static_cast<std::int8_t>(-step * std::sin(angle));
                auto dy = static_cast<std::int8_t>(step * std::cos(angle));
                if (dx != 0 || dy != 0)
                    mouse.move(dx, dy);
                i = (i + 1) % steps_per_circle;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        else {
            bool pressed = false;
            while (running) {
                pressed = !pressed;
                mouse.set_left_button(pressed);
                SPDLOG_INFO("Left button: {}", pressed ? "PRESSED" : "RELEASED");
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    });

    std::cin.get();

    running = false;
    mouse_thread.join();
    server.stop();

    return 0;
}
