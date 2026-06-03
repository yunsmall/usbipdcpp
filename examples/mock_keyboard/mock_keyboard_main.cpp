#include <iostream>
#include <thread>

#include "../example_utils.h"
#include "Server.h"
#include "virtual_device/SimpleVirtualDeviceHandler.h"
#include "virtual_device/devices/KeyboardHandler.h"

using namespace usbipdcpp;

int main(int argc, char **argv) {
    auto opts = make_example_options("mock_keyboard", "USB/IP virtual keyboard device");
    auto result = parse_example_args(opts, argc, argv);
    auto port = result["port"].as<std::uint16_t>();
    auto busid = result["busid"].as<std::string>();

    spdlog::set_level(spdlog::level::trace);

    StringPool string_pool;

    std::vector<UsbInterface> interfaces = {
            UsbInterface{
                    .interface_class = static_cast<std::uint8_t>(ClassCode::HID),
                    .interface_subclass = 0x01, // Boot Interface Subclass
                    .interface_protocol = 0x01, // Keyboard
                    .endpoints = {{
                            UsbEndpoint{
                                    .address = 0x81, // IN
                                    .attributes = 0x03,
                                    .max_packet_size = 16,
                                    .interval = 10,
                            },
                    }},
            },
    };
    interfaces[0].with_handler<KeyboardHandler>(string_pool);

    auto mock_keyboard = std::make_shared<UsbDevice>(UsbDevice{
            .path = "/usbipdcpp/mock_keyboard",
            .busid = busid,
            .bus_num = 1,
            .dev_num = 1,
            .speed = static_cast<std::uint32_t>(UsbSpeed::Full),
            .vendor_id = 0x1234,
            .product_id = 0x5679,
            .device_bcd = 0xABCD,
            .device_class = 0x00,
            .device_subclass = 0x00,
            .device_protocol = 0x00,
            .configuration_value = 1,
            .num_configurations = 1,
            .interfaces = interfaces,
            .ep0_in = UsbEndpoint::get_ep0_in(UsbSpeed::Full),
            .ep0_out = UsbEndpoint::get_ep0_out(UsbSpeed::Full),
    });
    auto device_handler = mock_keyboard->with_handler<SimpleVirtualDeviceHandler>(string_pool);
    device_handler->setup_interface_handlers();

    auto &kb = dynamic_cast<KeyboardHandler &>(*mock_keyboard->interfaces[0].handler);

    Server server;
    server.add_device(std::move(mock_keyboard));

    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), port};
    server.start(endpoint);

    SPDLOG_INFO("Mock keyboard started on port {}, busid {}", port, busid);
    SPDLOG_INFO("Connect with: usbip attach -r <host> -b {}", busid);
    SPDLOG_INFO("Press Enter to exit...");

    // 每隔一秒按下/释放 A 键
    std::atomic<bool> running{true};
    std::thread key_thread([&]() {
        while (running) {
            kb.press_key(HIDKey::A);
            SPDLOG_INFO("Key A pressed");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));

            kb.release_key(HIDKey::A);
            SPDLOG_INFO("Key A released");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    });

    std::cin.get();
    running = false;
    key_thread.join();
    server.stop();

    return 0;
}
