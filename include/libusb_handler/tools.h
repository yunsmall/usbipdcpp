#pragma once

#include <string>
#include <format>

#include <spdlog/spdlog.h>
#include <libusb-1.0/libusb.h>
#include <constant.h>


namespace usbipdcpp {
    inline std::string get_device_busid(libusb_device *device) {
        // return std::format("{}-{}-{}", libusb_get_bus_number(device),
        //                    libusb_get_device_address(device),
        //                    libusb_get_port_number(device));
        return std::format("{}-{}", libusb_get_bus_number(device),
                           libusb_get_port_number(device));
    }

    UsbSpeed libusb_speed_to_usb_speed(int speed);
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
