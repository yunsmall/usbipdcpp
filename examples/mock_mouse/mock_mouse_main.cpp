#include <iostream>
#include <thread>
#include <atomic>

#include "mock_mouse.h"

using namespace usbipdcpp;

int main() {
    spdlog::set_level(spdlog::level::trace);

    StringPool string_pool;

    std::vector<UsbInterface> interfaces = {
            UsbInterface{
                    .interface_class = static_cast<std::uint8_t>(
                        ClassCode::HID),
                    .interface_subclass = 0x00,
                    .interface_protocol = 0x00,
                    .endpoints = {
                            UsbEndpoint{
                                    .address = 0x81, // IN
                                    .attributes = 0x03,
                                    // 8 bytes
                                    .max_packet_size = 8,
                                    // Interrupt
                                    .interval = 10
                            }
                    }
            }
    };
    interfaces[0].with_handler<MockMouseInterfaceHandler>(string_pool);


    auto mock_mouse = std::make_shared<UsbDevice>(UsbDevice{
            .path = "/usbipdcpp/mock_mouse",
            .busid = "1-1",
            .bus_num = 1,
            .dev_num = 1,
            .speed = static_cast<std::uint32_t>(UsbSpeed::Low),
            .vendor_id = 0x1234,
            .product_id = 0x5678,
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
    auto device_handler = mock_mouse->with_handler<SimpleVirtualDeviceHandler>(string_pool);
    device_handler->setup_interface_handlers();

    MockMouseInterfaceHandler &mouse_interface_handler = *std::dynamic_pointer_cast<MockMouseInterfaceHandler>(
            mock_mouse->interfaces[0].handler);


    Server server;
    server.add_device(std::move(mock_mouse));

    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), 54324};

    server.start(endpoint);

    SPDLOG_INFO("Mock mouse started");
    SPDLOG_INFO("Port: 54324");
    SPDLOG_INFO("Busid: 1-1");
    SPDLOG_INFO("Connect with: usbip attach -r <host> -b 1-1");
    SPDLOG_INFO("Press Enter to exit...");

    // 后台线程模拟鼠标点击
    std::atomic<bool> running{true};
    std::thread mouse_thread([&]() {
        while (running) {
            {
                std::unique_lock lock(mouse_interface_handler.state_mutex);
                mouse_interface_handler.current_state.left_pressed = !mouse_interface_handler.current_state.left_pressed;
                mouse_interface_handler.state_cv.notify_one();
            }
            SPDLOG_INFO("Toggle left button");
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    });

    std::cin.get();

    running = false;
    mouse_thread.join();
    server.stop();

    return 0;
}
