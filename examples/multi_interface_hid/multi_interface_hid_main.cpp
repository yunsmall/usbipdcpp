#include <atomic>
#include <iostream>
#include <thread>

#include "../example_utils.h"
#include "Server.h"
#include "virtual_device/SimpleVirtualDeviceHandler.h"
#include "virtual_device/devices/KeyboardHandler.h"
#include "virtual_device/devices/RelativeMouseHandler.h"

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
    std::vector<UsbInterface> interfaces = {
            UsbInterface{.interface_class = static_cast<std::uint8_t>(ClassCode::HID),
                         .interface_subclass = 0x01, // Boot Interface
                         .interface_protocol = 0x02, // Mouse
                         .endpoints = {{UsbEndpoint{.address = 0x81,
                                                    .attributes = 0x03,
                                                    .max_packet_size = 8,
                                                    .interval = 10}}}},
            UsbInterface{.interface_class = static_cast<std::uint8_t>(ClassCode::HID),
                         .interface_subclass = 0x01, // Boot Interface
                         .interface_protocol = 0x01, // Keyboard
                         .endpoints = {{UsbEndpoint{.address = 0x82,
                                                    .attributes = 0x03,
                                                    .max_packet_size = 16,
                                                    .interval = 10}}}},
    };
    interfaces[0].with_handler<RelativeMouseHandler>(string_pool);
    interfaces[1].with_handler<KeyboardHandler>(string_pool);

    auto device = std::make_shared<UsbDevice>(UsbDevice{
            .path = "/usbipdcpp/multi_interface_hid",
            .busid = busid,
            .bus_num = 1,
            .dev_num = 1,
            .speed = static_cast<std::uint32_t>(UsbSpeed::Low),
            .vendor_id = 0x1234,
            .product_id = 0x5679,
            .device_bcd = 0xabcd,
            .device_class = 0x00,
            .device_subclass = 0x00,
            .device_protocol = 0x00,
            .configuration_value = 1,
            .num_configurations = 1,
            .interfaces = interfaces,
            .ep0_in = UsbEndpoint::get_ep0_in(UsbSpeed::Low),
            .ep0_out = UsbEndpoint::get_ep0_out(UsbSpeed::Low),
    });
    auto device_handler = device->with_handler<SimpleVirtualDeviceHandler>(string_pool);
    device_handler->setup_interface_handlers();

    auto &mouse = *std::dynamic_pointer_cast<RelativeMouseHandler>(device->interfaces[0].handler);
    auto &keyboard = *std::dynamic_pointer_cast<KeyboardHandler>(device->interfaces[1].handler);

    Server server;
    server.add_device(std::move(device));

    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), port};
    server.start(endpoint);

    SPDLOG_INFO("Multi-interface HID started on port {}, busid {}", port, busid);
    SPDLOG_INFO("Interface 0: relative mouse (square pattern)");
    SPDLOG_INFO("Interface 1: keyboard (a-z typing)");
    SPDLOG_INFO("Connect with: usbip attach -r <host> -b {}", busid);
    SPDLOG_INFO("Press Enter to exit...");

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

    std::cin.get();

    running = false;
    mouse_thread.join();
    keyboard_thread.join();
    server.stop();

    return 0;
}
