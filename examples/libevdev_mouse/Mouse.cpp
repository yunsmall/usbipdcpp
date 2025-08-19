#include "Mouse.h"

#include <fcntl.h>
#include <unistd.h>

#include <format>
#include <iostream>

using namespace usbipdcpp::umouse;

std::vector<MouseDescript> usbipdcpp::umouse::findAllMouses() {
    std::vector<MouseDescript> result;

    std::filesystem::path root = "/dev/input";
    for (auto &entry: std::filesystem::directory_iterator(root)) {
        if (!entry.is_directory() && entry.path().string().contains("event")) {
            auto &full_path = entry.path();
            int fd = open(full_path.c_str(), O_RDONLY | O_NONBLOCK);
            if (fd >= 0) {
                libevdev *dev = nullptr;
                if (libevdev_new_from_fd(fd, &dev) == 0) {
                    // 检查设备是否为鼠标
                    if (libevdev_has_event_type(dev, EV_REL) &&
                        (libevdev_has_event_code(dev, EV_KEY, BTN_LEFT) ||
                         libevdev_has_event_code(dev, EV_KEY, BTN_RIGHT))) {
                        auto name = libevdev_get_name(dev);
                        result.emplace_back(name, entry.path());
                    }
                    libevdev_free(dev);
                    close(fd);
                }
                else {
                    throw std::system_error(errno, std::system_category(),
                                            std::format("libevdev无法打开{}", full_path.string()));
                }

            }
            else {
                throw std::system_error(errno, std::system_category(),
                                        std::format("open {} 出错", full_path.string()));
            }
        }
    }
    return result;
}


MouseDevice usbipdcpp::umouse::openMouse(const std::filesystem::path &path) {
    MouseDevice mouse_device;
    // 打开设备文件
    mouse_device.fd = open(path.c_str(), O_RDONLY | O_NONBLOCK);
    if (mouse_device.fd < 0) {
        throw std::system_error(errno, std::system_category(),
                                std::format("open {} 出错", path.string()));
    }

    // 初始化 libevdev 上下文
    int rc = libevdev_new_from_fd(mouse_device.fd, &mouse_device.dev);
    if (rc < 0) {
        close(mouse_device.fd);
        throw std::system_error(errno, std::system_category(),
                                std::format("libevdev无法打开{}", path.string()));
    }

    mouse_device.path = path;

    std::cout << "成功打开鼠标设备:" << std::endl;
    std::cout << std::format("\t名字: {}", libevdev_get_name(mouse_device.dev)) << std::endl;;
    std::cout << std::format("\t路径: {}", path.string()) << std::endl;;
    std::cout << std::format("\t厂家: {:4x}", libevdev_get_id_vendor(mouse_device.dev)) << std::endl;;
    std::cout << std::format("\t产品: {:4x}", libevdev_get_id_product(mouse_device.dev)) << std::endl;;


    return mouse_device;
}

void usbipdcpp::umouse::closeMouse(MouseDevice &device) {
    if (device.dev) {
        libevdev_free(device.dev);
        device.dev = nullptr;
    }
    if (device.fd >= 0) {
        close(device.fd);
        device.fd = -1;
    }
}
