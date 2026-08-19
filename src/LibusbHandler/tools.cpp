#include "usbipdcpp/LibusbHandler/tools.h"

#include <spdlog/spdlog.h>

using namespace usbipdcpp;

UsbSpeed usbipdcpp::libusb_speed_to_usb_speed(int speed) {
    // libusb 枚举与内核 usb_device_speed 枚举的数值不一致（中间隔了
    // USB_SPEED_WIRELESS），不能直接强转，必须逐个映射：
    // LIBUSB_SPEED_SUPER=4 → USB_SPEED_SUPER=5，LIBUSB_SPEED_SUPER_PLUS=5 → 6
    // UsbSpeed 枚举值对齐内核枚举（constant.h），可直接序列化到协议 speed 字段
    switch (speed) {
        case LIBUSB_SPEED_LOW:
            return UsbSpeed::Low;
        case LIBUSB_SPEED_FULL:
            return UsbSpeed::Full;
        case LIBUSB_SPEED_HIGH:
            return UsbSpeed::High;
        case LIBUSB_SPEED_SUPER:
            return UsbSpeed::Super;
        case LIBUSB_SPEED_SUPER_PLUS:
            return UsbSpeed::SuperPlus;
        default:
            SPDLOG_DEBUG("unknown speed enum {}", speed);
            return UsbSpeed::Unknown;
    }
}
