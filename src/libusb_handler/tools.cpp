#include "libusb_handler/tools.h"

#include <spdlog/spdlog.h>

using namespace usbipdcpp;

UsbSpeed usbipdcpp::libusb_speed_to_usb_speed(int speed) {
    switch (speed) {
        case LIBUSB_SPEED_LOW:
            return UsbSpeed::Low;
        case LIBUSB_SPEED_FULL:
            return UsbSpeed::Full;
        case LIBUSB_SPEED_HIGH:
            return UsbSpeed::High;
        case LIBUSB_SPEED_SUPER:
            return UsbSpeed::Super;
        default:
            SPDLOG_DEBUG("unknown speed enum {}", speed);
    }
    return UsbSpeed::Unknown;
}