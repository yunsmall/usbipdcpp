#include <cmath>
#include <iostream>
#include <numbers>
#include <thread>

#include "../example_utils.h"
#include "usbipdcpp/Server.h"
#include "usbipdcpp/virtual_device/SimpleVirtualDeviceHandler.h"
#include "usbipdcpp/virtual_device/devices/GamepadHandler.h"

using namespace usbipdcpp;

int main(int argc, char **argv) {
    auto opts = make_example_options("mock_gamepad", "USB/IP virtual gamepad device");
    auto result = parse_example_args(opts, argc, argv);
    auto port = result["port"].as<std::uint16_t>();
    auto busid = result["busid"].as<std::string>();

    spdlog::set_level(spdlog::level::info);

    StringPool string_pool;

    auto mock_gamepad = UsbDevice::make(busid, 0x1234, 0x5680,
                                        {GamepadHandler::make_interface(0x81)},
                                        1, 1, 0, "/usbipdcpp/mock_gamepad");
    mock_gamepad->interfaces[0].with_handler<GamepadHandler>(string_pool);
    mock_gamepad->with_handler<SimpleVirtualDeviceHandler>(string_pool)->setup_interface_handlers();

    auto &gp = dynamic_cast<GamepadHandler &>(*mock_gamepad->interfaces[0].handler);

    Server server;
    server.add_device(std::move(mock_gamepad));

    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), port};
    if (auto ec = server.start(endpoint); ec) {
        SPDLOG_ERROR("服务器启动失败：{}", ec.message());
        return 1;
    }

    SPDLOG_INFO("Mock gamepad started on port {}, busid {}", port, busid);
    SPDLOG_INFO("Connect with: usbip attach -r <host> -b {}", busid);
    SPDLOG_INFO("Running... (Ctrl+C / SIGTERM / Enter to stop)");

    // 演示线程：D-pad 旋转 + 左摇杆画圆
    std::atomic<bool> running{true};
    std::thread demo_thread([&]() {
        constexpr int steps = 16;
        int step = 0;
        while (running) {
            // D-pad 旋转
            auto hat = static_cast<GamepadHandler::HatDirection>(step % 8);
            gp.set_hat(hat);

            // 左摇杆画圆
            double angle = step * 2.0 * std::numbers::pi / steps;
            int16_t x = static_cast<int16_t>(std::sin(angle) * 16384);
            int16_t y = static_cast<int16_t>(std::cos(angle) * 16384);
            gp.set_axis(0, x);
            gp.set_axis(1, y);

            // 按下按钮 0（A 键）
            gp.set_button(0, step % 2 == 0);

            step = (step + 1) % steps;
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
        gp.release_all_buttons();
        gp.set_hat(GamepadHandler::HatDirection::Center);
        gp.set_axis(0, 0);
        gp.set_axis(1, 0);
    });

    wait_for_exit();
    running = false;
    demo_thread.join();
    server.stop();

    return 0;
}
