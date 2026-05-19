#pragma once

#include <string>
#include <format>

#include <spdlog/spdlog.h>
#include <libusb-1.0/libusb.h>
#include <constant.h>
#include "Export.h"


namespace usbipdcpp {
inline std::string get_device_busid(libusb_device *device) {
    uint8_t ports[8];
    int n = libusb_get_port_numbers(device, ports, 8);
    auto busid = std::to_string(libusb_get_bus_number(device));
    for (int i = 0; i < n; i++) {
        busid += (i == 0 ? "-" : ".");
        busid += std::to_string(ports[i]);
    }
    return busid;
}

USBIPDCPP_API UsbSpeed libusb_speed_to_usb_speed(int speed);
}


#define dev_pfmt(dev, fmt) "dev {}: " fmt, ::usbipdcpp::get_device_busid((dev))

#define dev_info(dev,fmt,...) \
SPDLOG_INFO(dev_pfmt((dev),fmt) ,##__VA_ARGS__)
#define dev_dbg(dev,fmt,...) \
SPDLOG_DEBUG(dev_pfmt((dev),fmt) ,##__VA_ARGS__)
#define dev_warn(dev,fmt,...) \
SPDLOG_WARN(dev_pfmt((dev),fmt) ,##__VA_ARGS__)
#define dev_err(dev,fmt,...) \
SPDLOG_ERROR(dev_pfmt((dev),fmt) ,##__VA_ARGS__)
