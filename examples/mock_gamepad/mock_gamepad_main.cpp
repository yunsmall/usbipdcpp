#include <cmath>
#include <cxxopts.hpp>
#include <iostream>
#include <numbers>
#include <thread>

#include "Server.h"
#include "virtual_device/SimpleVirtualDeviceHandler.h"
#include "virtual_device/devices/GamepadHandler.h"

using namespace usbipdcpp;

int main(int argc, char **argv) {
    cxxopts::Options options("mock_gamepad", "USB/IP virtual gamepad device");
    options.add_options()
        ("p,port", "TCP port", cxxopts::value<std::uint16_t>()->default_value("53240"))
        ("b,busid", "Bus ID", cxxopts::value<std::string>()->default_value("1-1"))
        ("help", "Print help");
    cxxopts::ParseResult result;
    try {
        result = options.parse(argc, argv);
    } catch (const cxxopts::exceptions::exception &e) {
        std::cerr << e.what() << std::endl;
        std::cout << options.help() << std::endl;
        return 1;
    }
    if (result.count("help")) {
        std::cout << options.help() << std::endl;
        return 0;
    }
    auto port = result["port"].as<std::uint16_t>();
    auto busid = result["busid"].as<std::string>();

    spdlog::set_level(spdlog::level::info);

    StringPool string_pool;

    std::vector<UsbInterface> interfaces = {
            UsbInterface{
                    .interface_class = static_cast<std::uint8_t>(ClassCode::HID),
                    .interface_subclass = 0x00,
                    .interface_protocol = 0x00,
                    .endpoints = {{
                            UsbEndpoint{
                                    .address = 0x81, // IN
                                    .attributes = 0x03, // Interrupt
                                    .max_packet_size = 16,
                                    .interval = 8,
                            },
                    }},
            },
    };
    interfaces[0].with_handler<GamepadHandler>(string_pool);

    auto mock_gamepad = std::make_shared<UsbDevice>(UsbDevice{
            .path = "/usbipdcpp/mock_gamepad",
            .busid = busid,
            .bus_num = 1,
            .dev_num = 1,
            .speed = static_cast<std::uint32_t>(UsbSpeed::Full),
            .vendor_id = 0x1234,
            .product_id = 0x5680,
            .device_bcd = 0x0100,
            .device_class = 0x00,
            .device_subclass = 0x00,
            .device_protocol = 0x00,
            .configuration_value = 1,
            .num_configurations = 1,
            .interfaces = interfaces,
            .ep0_in = UsbEndpoint::get_ep0_in(UsbSpeed::Full),
            .ep0_out = UsbEndpoint::get_ep0_out(UsbSpeed::Full),
    });
    auto device_handler = mock_gamepad->with_handler<SimpleVirtualDeviceHandler>(string_pool);
    device_handler->setup_interface_handlers();

    auto &gp = dynamic_cast<GamepadHandler &>(*mock_gamepad->interfaces[0].handler);

    Server server;
    server.add_device(std::move(mock_gamepad));

    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), port};
    server.start(endpoint);

    SPDLOG_INFO("Mock gamepad started on port {}, busid {}", port, busid);
    SPDLOG_INFO("Connect with: usbip attach -r <host> -b {}", busid);
    SPDLOG_INFO("Press Enter to exit...");

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

    std::cin.get();
    running = false;
    demo_thread.join();
    server.stop();

    return 0;
}
