/**
 * @file absolute_mouse_main.cpp
 * @brief 绝对坐标鼠标虚拟设备示例
 */

#include <iostream>
#include <thread>
#include <atomic>

#include "Server.h"
#include "virtual_device/SimpleVirtualDeviceHandler.h"
#include "virtual_device/devices/AbsoluteMouseHandler.h"

using namespace usbipdcpp;

void print_usage() {
    std::cout << "\n命令列表 (屏幕坐标):\n"
              << "  p              - 打印当前按钮状态\n"
              << "  pos <x y>      - 设置屏幕坐标位置\n"
              << "  1              - 移动到屏幕中心\n"
              << "  2              - 移动到左上角\n"
              << "  3              - 移动到右下角\n"
              << "  6              - 左键点击\n"
              << "  7              - 右键点击\n"
              << "  8              - 双击\n"
              << "  9              - 平滑移动 (左上角→中心，1秒)\n"
              << "  H              - 人性化移动 (左上角→中心)\n"
              << "  D              - 拖动 (中心→右下角)\n"
              << "  hd x1 y1 x2 y2 - 人性化拖动 (起点→终点)\n"
              << "  raw <x y>      - 设置HID原始坐标 (0-32767)\n"
              << "  screen W H     - 设置屏幕尺寸\n"
              << "  bounds x1 y1 x2 y2 - 设置屏幕边界\n"
              << "  q              - 退出\n"
              << "  h              - 显示帮助\n"
              << std::endl;
}

int main() {
    spdlog::set_level(spdlog::level::debug);

    StringPool string_pool;

    std::vector<UsbInterface> interfaces = {
        UsbInterface{
            .interface_class = static_cast<std::uint8_t>(ClassCode::HID),
            .interface_subclass = 0x01,
            .interface_protocol = 0x02,  // Mouse
            .endpoints = {
                UsbEndpoint{
                    .address = 0x81,
                    .attributes = 0x03,
                    .max_packet_size = 8,
                    .interval = 1
                }
            }
        }
    };

    interfaces[0].with_handler<AbsoluteMouseHandler>(string_pool, 1920, 1080);

    auto mouse_device = std::make_shared<UsbDevice>(UsbDevice{
        .path = "/usbipdcpp/absolute_mouse",
        .busid = "1-1",
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
        .ep0_in = UsbEndpoint::get_default_ep0_in(),
        .ep0_out = UsbEndpoint::get_default_ep0_out(),
    });

    auto device_handler = mouse_device->with_handler<SimpleVirtualDeviceHandler>(string_pool);
    device_handler->setup_interface_handlers();

    auto mouse = std::dynamic_pointer_cast<AbsoluteMouseHandler>(mouse_device->interfaces[0].handler);

    Server server;
    server.add_device(std::move(mouse_device));

    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), 54327};
    server.start(endpoint);

    SPDLOG_INFO("绝对坐标虚拟鼠标服务器已启动，端口 54327");
    SPDLOG_INFO("Busid: 1-1");
    SPDLOG_INFO("Connect with: usbip attach -r <host> -b 1-1");

    // 初始位置：屏幕中心
    int cx = mouse->get_screen_x1() + mouse->get_screen_width() / 2;
    int cy = mouse->get_screen_y1() + mouse->get_screen_height() / 2;
    mouse->set_position(cx, cy);

    print_usage();

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;

        // 按空格分割命令
        std::vector<std::string> parts;
        std::istringstream iss(line);
        std::string part;
        while (iss >> part) {
            parts.push_back(part);
        }

        if (parts.empty()) continue;

        const std::string& cmd = parts[0];
        int cx = mouse->get_screen_x1() + mouse->get_screen_width() / 2;
        int cy = mouse->get_screen_y1() + mouse->get_screen_height() / 2;

        if (cmd == "p") {
            auto state = mouse->get_button_state();
            std::cout << "按钮状态: 左键=" << (state.left_button ? "按下" : "释放")
                      << " 右键=" << (state.right_button ? "按下" : "释放")
                      << " 中键=" << (state.middle_button ? "按下" : "释放")
                      << " 滚轮=" << static_cast<int>(state.wheel) << "\n"
                      << "屏幕范围: (" << mouse->get_screen_x1() << ", " << mouse->get_screen_y1()
                      << ") - (" << mouse->get_screen_x2() << ", " << mouse->get_screen_y2() << ")\n"
                      << "屏幕尺寸: " << mouse->get_screen_width() << "x" << mouse->get_screen_height() << std::endl;
        }
        else if (cmd == "pos" && parts.size() >= 3) {
            int x = std::stoi(parts[1]);
            int y = std::stoi(parts[2]);
            mouse->set_position(x, y);
            std::cout << "移动到屏幕坐标: (" << x << ", " << y << ")" << std::endl;
        }
        else if (cmd == "1") {
            std::cout << "移动到屏幕中心 (" << cx << ", " << cy << ")" << std::endl;
            mouse->set_position(cx, cy);
        }
        else if (cmd == "2") {
            std::cout << "移动到左上角 (" << mouse->get_screen_x1() << ", " << mouse->get_screen_y1() << ")" << std::endl;
            mouse->set_position(mouse->get_screen_x1() + 1, mouse->get_screen_y1() + 1);
        }
        else if (cmd == "3") {
            std::cout << "移动到右下角 (" << mouse->get_screen_x2() - 1 << ", " << mouse->get_screen_y2() - 1 << ")" << std::endl;
            mouse->set_position(mouse->get_screen_x2() - 1, mouse->get_screen_y2() - 1);
        }
        else if (cmd == "6") {
            std::cout << "左键点击 (" << cx << ", " << cy << ")" << std::endl;
            mouse->left_click(cx, cy);
        }
        else if (cmd == "7") {
            std::cout << "右键点击 (" << cx << ", " << cy << ")" << std::endl;
            mouse->right_click(cx, cy);
        }
        else if (cmd == "8") {
            std::cout << "双击 (" << cx << ", " << cy << ")" << std::endl;
            mouse->double_click(cx, cy);
        }
        else if (cmd == "9") {
            int x1 = mouse->get_screen_x1() + 1;
            int y1 = mouse->get_screen_y1() + 1;
            std::cout << "平滑移动 (" << x1 << ", " << y1 << ") → (" << cx << ", " << cy << ") ..." << std::endl;
            mouse->move(x1, y1, cx, cy, 1000);
        }
        else if (cmd == "H") {
            int x1 = mouse->get_screen_x1() + 1;
            int y1 = mouse->get_screen_y1() + 1;
            std::cout << "人性化移动 (" << x1 << ", " << y1 << ") → (" << cx << ", " << cy << ") ..." << std::endl;
            mouse->humanized_move(x1, y1, cx, cy, 1500);
        }
        else if (cmd == "D") {
            int x2 = mouse->get_screen_x2() - 1;
            int y2 = mouse->get_screen_y2() - 1;
            std::cout << "拖动 (" << cx << ", " << cy << ") → (" << x2 << ", " << y2 << ") ..." << std::endl;
            mouse->drag(cx, cy, x2, y2, 1000);
        }
        else if (cmd == "hd" && parts.size() >= 5) {
            int x1 = std::stoi(parts[1]);
            int y1 = std::stoi(parts[2]);
            int x2 = std::stoi(parts[3]);
            int y2 = std::stoi(parts[4]);
            std::cout << "人性化拖动 (" << x1 << ", " << y1 << ") → (" << x2 << ", " << y2 << ") ..." << std::endl;
            mouse->humanized_drag(x1, y1, x2, y2, 1500);
        }
        else if (cmd == "raw" && parts.size() >= 3) {
            int x = std::stoi(parts[1]);
            int y = std::stoi(parts[2]);
            mouse->set_position_raw(static_cast<std::int16_t>(x), static_cast<std::int16_t>(y));
            std::cout << "HID坐标: (" << x << ", " << y << ")" << std::endl;
        }
        else if (cmd == "screen" && parts.size() >= 3) {
            int w = std::stoi(parts[1]);
            int h = std::stoi(parts[2]);
            mouse->set_screen_size(w, h);
            std::cout << "屏幕尺寸设置为: " << w << "x" << h << std::endl;
        }
        else if (cmd == "bounds" && parts.size() >= 5) {
            int x1 = std::stoi(parts[1]);
            int y1 = std::stoi(parts[2]);
            int x2 = std::stoi(parts[3]);
            int y2 = std::stoi(parts[4]);
            mouse->set_screen_bounds(x1, y1, x2, y2);
            std::cout << "屏幕边界设置为: (" << x1 << ", " << y1 << ") - (" << x2 << ", " << y2 << ")" << std::endl;
        }
        else if (cmd == "q") {
            std::cout << "退出..." << std::endl;
            server.stop();
            return 0;
        }
        else if (cmd == "h") {
            print_usage();
        }
        else {
            std::cout << "未知命令: " << cmd << std::endl;
        }
    }

    server.stop();
    return 0;
}