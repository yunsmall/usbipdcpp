#include <atomic>
#include <cxxopts.hpp>
#include <iostream>
#include <thread>

#include "mock_mouse.h"

using namespace usbipdcpp;

int main(int argc, char **argv) {
    cxxopts::Options options("mock_mouse", "USB/IP virtual mouse device");
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

    spdlog::set_level(spdlog::level::trace);

    StringPool string_pool;

    std::vector<UsbInterface> interfaces = {UsbInterface{.interface_class = static_cast<std::uint8_t>(ClassCode::HID),
                                                         .interface_subclass = 0x00,
                                                         .interface_protocol = 0x00,
                                                         .endpoints = {{UsbEndpoint{.address = 0x81, // IN
                                                                                    .attributes = 0x03,
                                                                                    // 8 bytes
                                                                                    .max_packet_size = 8,
                                                                                    // Interrupt
                                                                                    .interval = 10}}}}};
    interfaces[0].with_handler<MockMouseInterfaceHandler>(string_pool);


    auto mock_mouse = std::make_shared<UsbDevice>(UsbDevice{
            .path = "/usbipdcpp/mock_mouse",
            .busid = busid,
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
            .ep0_in = UsbEndpoint::get_ep0_in(UsbSpeed::Low),
            .ep0_out = UsbEndpoint::get_ep0_out(UsbSpeed::Low),
    });
    auto device_handler = mock_mouse->with_handler<SimpleVirtualDeviceHandler>(string_pool);
    device_handler->setup_interface_handlers();

    MockMouseInterfaceHandler &mouse_interface_handler =
            *std::dynamic_pointer_cast<MockMouseInterfaceHandler>(mock_mouse->interfaces[0].handler);


    Server server;
    server.add_device(std::move(mock_mouse));

    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), port};

    server.start(endpoint);

    SPDLOG_INFO("Mock mouse started on port {}, busid {}", port, busid);
    SPDLOG_INFO("Connect with: usbip attach -r <host> -b {}", busid);
    SPDLOG_INFO("Press Enter to exit...");

    // 后台线程模拟鼠标点击
    std::atomic<bool> running{true};
    std::thread mouse_thread([&]() {
        while (running) {
            {
                std::unique_lock lock(mouse_interface_handler.state_mutex);
                mouse_interface_handler.current_state.left_pressed =
                        !mouse_interface_handler.current_state.left_pressed;
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
