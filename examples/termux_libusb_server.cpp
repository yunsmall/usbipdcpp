#include <asio.hpp>
#include <iostream>
#include <libusb-1.0/libusb.h>
#include <spdlog/spdlog.h>

#include "libusb_handler/LibusbServer.h"

using namespace usbipdcpp;

int main(int argc, char **argv) {
    spdlog::set_level(spdlog::level::trace);
    int err;
    if (argc <= 1) {
        SPDLOG_ERROR("You must pass an fd argument");
        return -1;
    }
    int fd;
    if (sscanf(argv[1], "%d", &fd) != 1) {
        SPDLOG_ERROR("Parse fd failed");
        return -1;
    }

    libusb_set_option(nullptr, LIBUSB_OPTION_NO_DEVICE_DISCOVERY);
    err = libusb_init(nullptr);
    if (err) {
        SPDLOG_ERROR("libusb_init failed: {}", libusb_strerror(err));
        libusb_exit(nullptr);
        return 1;
    }

    LibusbServer server;
    asio::ip::tcp::endpoint endpoint(asio::ip::tcp::v4(), 3240);


    libusb_device_handle *dev_handle;
    err = libusb_wrap_sys_device(nullptr, (intptr_t) fd, &dev_handle);
    if (err) {
        SPDLOG_ERROR("libusb_wrap_sys_device failed: {}", libusb_strerror(err));
        libusb_exit(nullptr);
        return -1;
    }

    server.bind_host_device(nullptr, true, dev_handle);
    server.start(endpoint);
    spdlog::info("enter any thing to stop the server");

    std::string line;
    std::getline(std::cin, line);

    server.stop();
    libusb_exit(nullptr);
    return 0;
}
