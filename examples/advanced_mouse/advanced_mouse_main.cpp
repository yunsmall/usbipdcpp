/**
 * @file advanced_mouse_main.cpp
 * @brief 高级鼠标虚拟设备示例
 */

#include <iostream>
#include <thread>
#include <atomic>

#include "Server.h"
#include "virtual_device/SimpleVirtualDeviceHandler.h"
#include "virtual_device/devices/AdvancedMouseHandler.h"

using namespace usbipdcpp;

void print_usage() {
    std::cout << "\n命令列表 (屏幕坐标):\n"
              << "  p            - 打印当前位置\n"
              << "  pos <x y>    - 设置屏幕坐标位置\n"
              << "  1            - 移动到屏幕中心\n"
              << "  2            - 移动到左上角\n"
              << "  3            - 移动到右下角\n"
              << "  4            - 相对移动 (+100, +100)\n"
              << "  5            - 相对移动 (-100, -100)\n"
              << "  6            - 左键点击\n"
              << "  7            - 右键点击\n"
              << "  8            - 双击\n"
              << "  9            - 平滑移动到中心 (耗时1秒)\n"
              << "  w/a/s/d      - 上/左/下/右移动 50像素\n"
              << "  W/A/S/D      - 上/左/下/右移动 200像素\n"
              << "  raw <x y>    - 设置HID原始坐标 (0-32767)\n"
              << "  screen W H   - 设置屏幕尺寸\n"
              << "  bounds <x1 y1 x2 y2> - 设置屏幕边界\n"
              << "  q            - 退出\n"
              << "  h            - 显示帮助\n"
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

    // 创建鼠标处理器，默认屏幕 1920x1080
    interfaces[0].with_handler<AdvancedMouseHandler>(string_pool,
        AdvancedMouseHandler::CoordinateMode::Absolute, 1920, 1080);

    auto mouse_device = std::make_shared<UsbDevice>(UsbDevice{
        .path = "/usbipdcpp/advanced_mouse",
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

    auto mouse = std::dynamic_pointer_cast<AdvancedMouseHandler>(mouse_device->interfaces[0].handler);

    Server server;
    server.add_device(std::move(mouse_device));

    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), 54327};
    server.start(endpoint);

    SPDLOG_INFO("高级虚拟鼠标服务器已启动，端口 54327");
    SPDLOG_INFO("Busid: 1-1");
    SPDLOG_INFO("Connect with: usbip attach -r <host> -b 1-1");

    // 初始位置：屏幕中心
    mouse->set_position(mouse->get_screen_x1() + mouse->get_screen_width() / 2,
                        mouse->get_screen_y1() + mouse->get_screen_height() / 2);

    print_usage();

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;

        // 解析命令
        if (line.size() >= 3 && line.substr(0, 3) == "pos") {
            // pos <x> <y> - 屏幕坐标
            int x, y;
            if (std::sscanf(line.c_str(), "pos %d %d", &x, &y) == 2) {
                mouse->set_position(x, y);
                auto state = mouse->get_current_state();
                std::cout << "移动到屏幕坐标: (" << state.x << ", " << state.y << ")" << std::endl;
            }
            continue;
        }

        if (line.size() >= 3 && line.substr(0, 3) == "raw") {
            // raw <x> <y> - HID原始坐标
            int x, y;
            if (std::sscanf(line.c_str(), "raw %d %d", &x, &y) == 2) {
                mouse->set_position_raw(static_cast<std::int16_t>(x), static_cast<std::int16_t>(y));
                auto state = mouse->get_current_state();
                std::cout << "HID坐标: (" << state.hid_x << ", " << state.hid_y << ") "
                          << "屏幕坐标: (" << state.x << ", " << state.y << ")" << std::endl;
            }
            continue;
        }

        if (line.size() >= 6 && line.substr(0, 6) == "screen") {
            int w, h;
            if (std::sscanf(line.c_str(), "screen %d %d", &w, &h) == 2) {
                mouse->set_screen_size(w, h);
                std::cout << "屏幕尺寸设置为: " << w << "x" << h << std::endl;
            }
            continue;
        }

        if (line.size() >= 6 && line.substr(0, 6) == "bounds") {
            int x1, y1, x2, y2;
            if (std::sscanf(line.c_str(), "bounds %d %d %d %d", &x1, &y1, &x2, &y2) == 4) {
                mouse->set_screen_bounds(x1, y1, x2, y2);
                std::cout << "屏幕边界设置为: (" << x1 << ", " << y1 << ") - (" << x2 << ", " << y2 << ")" << std::endl;
            }
            continue;
        }

        char cmd = line[0];
        switch (cmd) {
            case 'p': {
                auto state = mouse->get_current_state();
                std::cout << "========== 鼠标状态 ==========\n"
                          << "屏幕坐标: (" << state.x << ", " << state.y << ") "
                          << "范围: (" << mouse->get_screen_x1() << ", " << mouse->get_screen_y1() << ") - (" << mouse->get_screen_x2() << ", " << mouse->get_screen_y2() << ") "
                          << "尺寸: " << mouse->get_screen_width() << "x" << mouse->get_screen_height() << "\n"
                          << "HID坐标: (" << state.hid_x << ", " << state.hid_y << ") "
                          << "范围: (0, 0) - (32767, 32767)\n"
                          << "按键状态: 左键=" << (state.left_button ? "按下" : "释放")
                          << " 右键=" << (state.right_button ? "按下" : "释放")
                          << " 中键=" << (state.middle_button ? "按下" : "释放") << "\n"
                          << "滚轮: " << static_cast<int>(state.wheel) << std::endl;
                break;
            }
            case '1': {
                int cx = mouse->get_screen_x1() + mouse->get_screen_width() / 2;
                int cy = mouse->get_screen_y1() + mouse->get_screen_height() / 2;
                std::cout << "移动到屏幕中心 (" << cx << ", " << cy << ")" << std::endl;
                mouse->set_position(cx, cy);
                break;
            }
            case '2': {
                std::cout << "移动到左上角 (" << mouse->get_screen_x1() << ", " << mouse->get_screen_y1() << ")" << std::endl;
                mouse->set_position(mouse->get_screen_x1(), mouse->get_screen_y1());
                break;
            }
            case '3': {
                std::cout << "移动到右下角 (" << mouse->get_screen_x2() - 1 << ", " << mouse->get_screen_y2() - 1 << ")" << std::endl;
                mouse->set_position(mouse->get_screen_x2() - 1, mouse->get_screen_y2() - 1);
                break;
            }
            case '4':
                std::cout << "相对移动 (+100, +100)" << std::endl;
                mouse->move_relative(100, 100);
                break;
            case '5':
                std::cout << "相对移动 (-100, -100)" << std::endl;
                mouse->move_relative(-100, -100);
                break;
            case '6':
                std::cout << "左键点击" << std::endl;
                mouse->left_click();
                break;
            case '7':
                std::cout << "右键点击" << std::endl;
                mouse->right_click();
                break;
            case '8':
                std::cout << "双击" << std::endl;
                mouse->double_click();
                break;
            case '9': {
                int cx = mouse->get_screen_x1() + mouse->get_screen_width() / 2;
                int cy = mouse->get_screen_y1() + mouse->get_screen_height() / 2;
                std::cout << "平滑移动到中心 (" << cx << ", " << cy << ") ..." << std::endl;
                mouse->smooth_move_to(cx, cy, 1000);
                break;
            }
            case 'w': mouse->move_relative(0, -50); break;
            case 'W': mouse->move_relative(0, -200); break;
            case 's': mouse->move_relative(0, 50); break;
            case 'S': mouse->move_relative(0, 200); break;
            case 'a': mouse->move_relative(-50, 0); break;
            case 'A': mouse->move_relative(-200, 0); break;
            case 'd': mouse->move_relative(50, 0); break;
            case 'D': mouse->move_relative(200, 0); break;
            case 'h': print_usage(); break;
            case 'q':
                std::cout << "退出..." << std::endl;
                server.stop();
                return 0;
            default:
                std::cout << "未知命令: " << cmd << std::endl;
                break;
        }
    }

    server.stop();
    return 0;
}