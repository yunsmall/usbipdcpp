#pragma once

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
    }
}
