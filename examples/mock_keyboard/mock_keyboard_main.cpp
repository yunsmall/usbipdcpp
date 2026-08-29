#include <iostream>
#include <thread>

#include "../example_utils.h"
#include "usbipdcpp/Server.h"
#include "usbipdcpp/virtual_device/SimpleVirtualDeviceHandler.h"
#include "usbipdcpp/virtual_device/devices/KeyboardHandler.h"

using namespace usbipdcpp;

int main(int argc, char **argv) {
    auto opts = make_example_options("mock_keyboard", "USB/IP virtual keyboard device");
    auto result = parse_example_args(opts, argc, argv);
    auto port = result["port"].as<std::uint16_t>();
    auto busid = result["busid"].as<std::string>();

    spdlog::set_level(spdlog::level::trace);

    StringPool string_pool;

    // make_interface 返回已绑定 KeyboardHandler 的完整键盘接口
    auto mock_keyboard = UsbDevice::make(busid, 0x1234, 0x5679,
                                         {KeyboardHandler::make_interface(string_pool, 0x81)},
                                         1, 1, 0, "/usbipdcpp/mock_keyboard", UsbSpeed::Full, 0xABCD);
    mock_keyboard->with_handler<SimpleVirtualDeviceHandler>(string_pool)->setup_interface_handlers();

    auto &kb = dynamic_cast<KeyboardHandler &>(*mock_keyboard->interfaces[0].handler);

    Server server;
    server.add_device(std::move(mock_keyboard));

    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), port};
    if (auto ec = server.start(endpoint); ec) {
        SPDLOG_ERROR("服务器启动失败：{}", ec.message());
        return 1;
    }

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
