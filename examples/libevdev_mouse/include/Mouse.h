#pragma once

#include <poll.h>

#include <iostream>
#include <filesystem>
#include <string>
#include <vector>

#include <libevdev/libevdev.h>

namespace usbipdcpp {
    namespace umouse {

        struct MouseDescript {
            std::string name;
            std::filesystem::path path;
        };

        struct MouseDevice {
            int fd;
            libevdev *dev;
            std::filesystem::path path;
        };

        std::vector<MouseDescript> findAllMouses();

        MouseDevice openMouse(const std::filesystem::path &path);

        void closeMouse(MouseDevice &device);

        // 主事件循环，写成模板函数主要是为了运行时性能考虑，不要用std::function
        // template<mouse_event_handle_t H>
        // void mouse_event_loop(MouseDevice &mouse, H&& handle);
    }


    // template<umouse::mouse_event_handle_t HANDLE>
    // void umouse::mouse_event_loop(MouseDevice &mouse, HANDLE&& handle) {
    //     struct input_event ev;
    //     int rc;
    //
    //     std::println(std::cout, "Reading mouse events. Press Ctrl+C to exit...");
    //     std::println(std::cout, "-------- Start of Event Group --------");
    //
    //     // 使用 poll 监控文件描述符
    //     struct pollfd fds[1];
    //     fds[0].fd = mouse.fd;
    //     fds[0].events = POLLIN;
    //
    //     while (1) {
    //         // 等待事件（1000毫秒超时）
    //         int ret = poll(fds, 1, 1000);
    //
    //         if (ret < 0) {
    //             throw std::system_error(errno, std::generic_category(), "poll出错");
    //             break;
    //         }
    //
    //         if (ret == 0) {
    //             // 超时 - 无事件
    //             continue;
    //         }
    //
    //         // 读取所有可用事件
    //         while ((rc = libevdev_next_event(mouse.dev, LIBEVDEV_READ_FLAG_NORMAL, &ev)) == 0) {
    //             handle(ev);
    //         }
    //
    //         if (rc != -EAGAIN) {
    //             throw std::system_error(errno, std::generic_category(), "读取event时出错");
    //             break;
    //         }
    //     }
    // }
}