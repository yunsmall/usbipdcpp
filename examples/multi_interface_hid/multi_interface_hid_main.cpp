#include <atomic>
#include <iostream>
#include <thread>

#include "../example_utils.h"
#include "usbipdcpp/Server.h"
#include "usbipdcpp/virtual_device/SimpleVirtualDeviceHandler.h"
#include "usbipdcpp/virtual_device/devices/KeyboardHandler.h"
#include "usbipdcpp/virtual_device/devices/RelativeMouseHandler.h"

using namespace usbipdcpp;

int main(int argc, char **argv) {
    auto opts = make_example_options("multi_interface_hid", "USB/IP composite device: relative mouse + keyboard");
    auto result = parse_example_args(opts, argc, argv);
    auto port = result["port"].as<std::uint16_t>();
    auto busid = result["busid"].as<std::string>();

    spdlog::set_level(spdlog::level::trace);

    StringPool string_pool;

    // 接口 0: 相对鼠标
    // 接口 1: 键盘
    // 接口 0: 相对鼠标（make_interface 提供非 boot 的 HID 鼠标定义 03/00/00）
    // 接口 1: 键盘
    std::vector<UsbInterface> interfaces = {
            RelativeMouseHandler::make_interface(string_pool, 0x81),
            KeyboardHandler::make_interface(string_pool, 0x82),
    };

    auto device = UsbDevice::make(busid, 0x1234, 0x5679, interfaces, 1, 1, 0, "/usbipdcpp/multi_interface_hid",
                                  UsbSpeed::Low, 0xabcd);
    device->with_handler<SimpleVirtualDeviceHandler>(string_pool)->setup_interface_handlers();

    auto &mouse = *std::dynamic_pointer_cast<RelativeMouseHandler>(device->interfaces[0].handler);
    auto &keyboard = *std::dynamic_pointer_cast<KeyboardHandler>(device->interfaces[1].handler);

    Server server;
    server.add_device(std::move(device));

    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), port};
    if (auto ec = server.start(endpoint); ec) {
        SPDLOG_ERROR("服务器启动失败：{}", ec.message());
        return 1;
    }

    SPDLOG_INFO("Multi-interface HID started on port {}, busid {}", port, busid);
    SPDLOG_INFO("Interface 0: relative mouse (square pattern)");
    SPDLOG_INFO("Interface 1: keyboard (a-z typing)");
    SPDLOG_INFO("Connect with: usbip attach -r <host> -b {}", busid);
    SPDLOG_INFO("Running... (Ctrl+C / SIGTERM / Enter to stop)");

    std::atomic<bool> running{true};

    // 鼠标线程：画正方形（右 → 下 → 左 → 上）
    std::thread mouse_thread([&]() {
        constexpr std::int8_t speed = 5;
        constexpr int side_steps = 40; // 每边 40 步
        int dir = 0; // 0=右, 1=下, 2=左, 3=上
        int step = 0;
        while (running) {
            switch (dir) {
            case 0: mouse.move(speed, 0); break;
            case 1: mouse.move(0, speed); break;
            case 2: mouse.move(-speed, 0); break;
            case 3: mouse.move(0, -speed); break;
            }
            if (++step >= side_steps) {
                step = 0;
                dir = (dir + 1) % 4;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    // 键盘线程：a → z 每秒一个
    std::thread keyboard_thread([&]() {
        constexpr std::uint8_t HID_A = 0x04;
        while (running) {
            for (int i = 0; i < 26 && running; ++i) {
                auto keycode = static_cast<std::uint8_t>(HID_A + i);
                keyboard.press_key(keycode);
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                keyboard.release_key(keycode);
                std::this_thread::sleep_for(std::chrono::milliseconds(950));
            }
        }
    });

    wait_for_exit();

    running = false;
    mouse_thread.join();
    keyboard_thread.join();
    server.stop();

    return 0;
}
