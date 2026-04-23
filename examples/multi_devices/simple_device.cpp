#include "simple_device.h"

#include <spdlog/spdlog.h>

// HID报告描述符 - 一个简单的按钮
const usbipdcpp::data_type SimpleHidInterfaceHandler::report_descriptor_ = {
        0x05, 0x01, // Usage Page (Generic Desktop)
        0x09, 0x01, // Usage (Pointer)
        0xA1, 0x01, // Collection (Application)
        0x09, 0x01, //   Usage (Pointer)
        0x15, 0x00, //   Logical Minimum (0)
        0x25, 0x01, //   Logical Maximum (1)
        0x75, 0x01, //   Report Size (1)
        0x95, 0x01, //   Report Count (1)
        0x81, 0x02, //   Input (Data,Var,Abs)
        0xC0        // End Collection
};

SimpleHidInterfaceHandler::SimpleHidInterfaceHandler(usbipdcpp::UsbInterface &handle_interface,
                                                     usbipdcpp::StringPool &string_pool) :
    HidVirtualInterfaceHandler(handle_interface, string_pool) {
}

// 主机请求输入报告时返回固定数据
usbipdcpp::data_type SimpleHidInterfaceHandler::on_input_report_requested(std::uint16_t length) {
    return {0x00};
}

std::uint16_t SimpleHidInterfaceHandler::get_report_descriptor_size() {
    return report_descriptor_.size();
}

usbipdcpp::data_type SimpleHidInterfaceHandler::get_report_descriptor() {
    return report_descriptor_;
}

// SimpleDeviceHandler 实现
SimpleDeviceHandler::SimpleDeviceHandler(usbipdcpp::UsbDevice &handle_device, usbipdcpp::StringPool &string_pool) :
    SimpleVirtualDeviceHandler(handle_device, string_pool) {
}
