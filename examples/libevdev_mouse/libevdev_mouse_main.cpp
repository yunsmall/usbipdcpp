#include "libevdev_mouse.h"

#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <poll.h>

#include "Session.h"
#include "Server.h"

#include <libevdev/libevdev.h>

#include "Mouse.h"

using namespace usbipdcpp;
using namespace usbipdcpp::umouse;

int main() {
    auto mouses = findAllMouses();

    if (mouses.size() > 0) {
        std::cout << std::format("当前系统有{}个可用的鼠标设备", mouses.size()) << std::endl;
        for (std::size_t i = 0; i < mouses.size(); i++) {
            std::cout << std::format("第{}个鼠标设备：{}", i, mouses[i].name) << std::endl;
            std::cout << std::format("路径：{}", mouses[i].path.string()) << std::endl;
            std::cout << std::endl;
        }
        std::cout << std::format("请输入想使用的鼠标设备编号：");

        std::size_t index;
        while (std::cin >> index) {
            if (index < mouses.size()) {
                break;
            }
            std::cin.clear();
            std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
            std::cout << std::format("编号不合法，请重新输入") << std::endl;
        }
        MouseDevice opened_mouse;

        try {
            opened_mouse = openMouse(mouses[index].path);
        } catch (const std::exception &e) {
            std::cerr << e.what() << std::endl;
            return 1;
        }

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
        interfaces[0].with_handler<LibevdevMouseInterfaceHandler>(string_pool);


        auto libevdev_mouse = std::make_shared<UsbDevice>(UsbDevice{
                .path = "/usbipdcpp/libevdev_mouse",
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
        auto device_handler = libevdev_mouse->with_handler<SimpleVirtualDeviceHandler>(string_pool);
        device_handler->setup_interface_handlers();

        LibevdevMouseInterfaceHandler &mouse_interface_handler = *std::dynamic_pointer_cast<
            LibevdevMouseInterfaceHandler>(
                libevdev_mouse->interfaces[0].handler);

        Server server;

        asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), 54324};

        server.add_device(std::move(libevdev_mouse));

        server.start(endpoint);

        SPDLOG_INFO("Libevdev mouse started");
        SPDLOG_INFO("Port: 54324");
        SPDLOG_INFO("Busid: 1-1");
        SPDLOG_INFO("Connect with: usbip attach -r <host> -b 1-1");

        struct input_event ev;
        int rc;

        std::cout << "Reading mouse events. Press Ctrl+C to exit..." << std::endl;
        std::cout << "-------- Start of Event Group --------" << std::endl;

        // 使用 poll 监控文件描述符
        struct pollfd fds[1];
        fds[0].fd = opened_mouse.fd;
        fds[0].events = POLLIN;

        while (true) {
            // 等待事件（1000毫秒超时）
            int ret = poll(fds, 1, 1000);

            if (ret < 0) {
                SPDLOG_ERROR("poll error: {}", std::generic_category().message(errno));
                break;
            }

            if (ret == 0) {
                // 超时 - 无事件
                continue;
            }


            // 读取所有可用事件
            while ((rc = libevdev_next_event(opened_mouse.dev, LIBEVDEV_READ_FLAG_NORMAL, &ev)) == 0) {
                std::lock_guard lock(mouse_interface_handler.state_mutex);
                switch (ev.type) {
                    case EV_REL:
                        // 鼠标移动事件
                        switch (ev.code) {
                            case REL_X: {
                                auto result = mouse_interface_handler.current_state.move_horizontal + ev.value;
                                if (result > 127) {
                                    mouse_interface_handler.current_state.move_horizontal = 127;
                                }
                                else if (result < -127) {
                                    mouse_interface_handler.current_state.move_horizontal = -127;
                                }
                                else {
                                    mouse_interface_handler.current_state.move_horizontal = result;
                                }

                                std::cout << std::format("Mouse moved: X={}", ev.value) << std::endl;
                                break;
                            }

                            case REL_Y: {
                                auto result = mouse_interface_handler.current_state.move_vertical + ev.value;
                                if (result > 127) {
                                    mouse_interface_handler.current_state.move_vertical = 127;
                                }
                                else if (result < -127) {
                                    mouse_interface_handler.current_state.move_vertical = -127;
                                }
                                else {
                                    mouse_interface_handler.current_state.move_vertical = result;
                                }

                                std::cout << std::format("Mouse moved: Y={}", ev.value) << std::endl;
                                break;
                            }

                            case REL_WHEEL:
                                // mouse_interface_handler.wheel_vertical = ev.value;
                                std::cout << std::format("Mouse wheel: Vertical Ident={}", ev.value) << std::endl;
                                break;
                            case REL_HWHEEL:
                                std::cout << std::format("Mouse wheel: Horizontal Ident={}", ev.value) << std::endl;
                                break;
                            case REL_WHEEL_HI_RES: {
                                auto resized = ev.value / 120;
                                auto result = mouse_interface_handler.current_state.wheel_vertical + resized;
                                if (result > 127) {
                                    mouse_interface_handler.current_state.wheel_vertical = 127;
                                }
                                else if (result < -127) {
                                    mouse_interface_handler.current_state.wheel_vertical = -127;
                                }
                                else {
                                    mouse_interface_handler.current_state.wheel_vertical = result;
                                }

                                std::cout << std::format("Mouse wheel high resolution: Vertical={}", ev.value) <<
                                        std::endl;
                                break;
                            }
                            case REL_HWHEEL_HI_RES:
                                std::cout << std::format("Mouse wheel high resolution: Horizontal={}", ev.value) <<
                                        std::endl;
                                break;
                            default:
                                std::cout << std::format("Unknown relative event: code={}, value={}", ev.code, ev.value)
                                        << std::endl;
                        }
                        break;

                    case EV_KEY:
                        // 鼠标按键事件
                        switch (ev.code) {
                            case BTN_LEFT:
                                mouse_interface_handler.current_state.left_pressed = ev.value;
                                std::cout << std::format("Left button: {}", ev.value ? "PRESSED" : "RELEASED") <<
                                        std::endl;
                                break;
                            case BTN_RIGHT:
                                mouse_interface_handler.current_state.right_pressed = ev.value;
                                std::cout << std::format("Right button: {}", ev.value ? "PRESSED" : "RELEASED") <<
                                        std::endl;
                                break;
                            case BTN_MIDDLE:
                                mouse_interface_handler.current_state.middle_pressed = ev.value;
                                std::cout << std::format("Middle button: {}", ev.value ? "PRESSED" : "RELEASED") <<
                                        std::endl;
                                break;
                            case BTN_SIDE:
                                mouse_interface_handler.current_state.side_pressed = ev.value;
                                std::cout << std::format("Side button: {}", ev.value ? "PRESSED" : "RELEASED") <<
                                        std::endl;
                                break;
                            case BTN_EXTRA:
                                mouse_interface_handler.current_state.extra_pressed = ev.value;
                                std::cout << std::format("Extra button: {}", ev.value ? "PRESSED" : "RELEASED") <<
                                        std::endl;
                                break;
                            default:
                                std::cout << std::format("Unknown key event: code={}, value={}", ev.code, ev.value) <<
                                        std::endl;
                        }
                        break;

                    case EV_SYN:
                        // 同步事件 - 表示一组事件结束
                        if (ev.code == SYN_REPORT) {
                            std::cout << "-------- End of Event Group --------" << std::endl;
                        }
                        break;

                    default:
                        std::cout << std::format("Unknown event type: {}, code={}, value={}", ev.type, ev.code,
                                                 ev.value) << std::endl;
                }
                mouse_interface_handler.state_cv.notify_all();
            }

            if (rc != -EAGAIN) {
                SPDLOG_ERROR("error when reading event: {}", std::generic_category().message(errno));
                break;
            }
        }

        closeMouse(opened_mouse);

        server.stop();

    }
    else {
        std::cout << "！无可使用的鼠标设备" << std::endl;
    }


    return 0;
}
