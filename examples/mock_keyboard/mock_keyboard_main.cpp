#include <iostream>
#include <thread>

#include "mock_keyboard.h"

using namespace usbipdcpp;

int main() {
    spdlog::set_level(spdlog::level::trace);

    StringPool string_pool;

    std::vector<UsbInterface> interfaces = {
            UsbInterface{
                    .interface_class = static_cast<std::uint8_t>(
                        ClassCode::HID),
                    .interface_subclass = 0x01, // Boot Interface Subclass
                    .interface_protocol = 0x01, // Keyboard
                    .endpoints = {
                            UsbEndpoint{
                                    .address = 0x81, // IN
                                    .attributes = 0x03,
                                    .max_packet_size = 8,
                                    .interval = 10
                            }
                    }
            }
    };
    interfaces[0].with_handler<MockKeyboardInterfaceHandler>(string_pool);


    auto mock_keyboard = std::make_shared<UsbDevice>(UsbDevice{
            .path = "/usbipdcpp/mock_keyboard",
            .busid = "1-2",
            .bus_num = 1,
            .dev_num = 2,
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
            .ep0_in = UsbEndpoint::get_default_ep0_in(),
            .ep0_out = UsbEndpoint::get_default_ep0_out(),
    });
    mock_keyboard->with_handler<SimpleVirtualDeviceHandler>(string_pool);

    MockKeyboardInterfaceHandler &keyboard_interface_handler = *std::dynamic_pointer_cast<MockKeyboardInterfaceHandler>(
            mock_keyboard->interfaces[0].handler);


    Server server;
    server.add_device(std::move(mock_keyboard));

    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), 54325};

    server.start(endpoint);

    SPDLOG_INFO("Mock keyboard started on port 54325");
    SPDLOG_INFO("Simulating key press: A key every second");

    // 模拟按下A键
    // A键的HID usage code是0x04
    constexpr std::uint8_t KEY_A = 0x04;

    std::chrono::seconds run_time{30};
    for (int i = 0; i < std::chrono::duration_cast<std::chrono::seconds>(run_time).count(); i++) {
        // 按下A键
        {
            std::unique_lock lock(keyboard_interface_handler.state_mutex);
            keyboard_interface_handler.current_state.keys[0] = KEY_A;
            keyboard_interface_handler.state_cv.notify_one();
        }
        SPDLOG_INFO("Key A pressed");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        // 释放A键
        {
            std::unique_lock lock(keyboard_interface_handler.state_mutex);
            keyboard_interface_handler.current_state.keys[0] = 0;
            keyboard_interface_handler.state_cv.notify_one();
        }
        SPDLOG_INFO("Key A released");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    server.stop();

    return 0;
}
