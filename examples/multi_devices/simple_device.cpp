#include "simple_device.h"

#include <spdlog/spdlog.h>

// HID 报告描述符：标准 3 键相对鼠标。原先只声明一个 Pointer 按钮、没有
// X/Y 字段，Linux 能枚举但 Windows 的 hidclass 会以 CM_PROB_FAILED_START
// 拒绝启动（导出设备在 Windows 不可用）
const usbipdcpp::data_type SimpleHidInterfaceHandler::report_descriptor_ = {
        0x05, 0x01, // Usage Page (Generic Desktop)
        0x09, 0x02, // Usage (Mouse)
        0xA1, 0x01, // Collection (Application)
        0x09, 0x01, //   Usage (Pointer)
        0xA1, 0x00, //   Collection (Physical)
        0x05, 0x09, //     Usage Page (Button)
        0x19, 0x01, //     Usage Minimum (1)
        0x29, 0x03, //     Usage Maximum (3)
        0x15, 0x00, //     Logical Minimum (0)
        0x25, 0x01, //     Logical Maximum (1)
        0x95, 0x03, //     Report Count (3)
        0x75, 0x01, //     Report Size (1)
        0x81, 0x02, //     Input (Data,Var,Abs) — 三个按键位
        0x95, 0x01, //     Report Count (1)
        0x75, 0x05, //     Report Size (5)
        0x81, 0x01, //     Input (Const) — 补齐一个字节
        0x05, 0x01, //     Usage Page (Generic Desktop)
        0x09, 0x30, //     Usage (X)
        0x09, 0x31, //     Usage (Y)
        0x15, 0x81, //     Logical Minimum (-127)
        0x25, 0x7F, //     Logical Maximum (127)
        0x75, 0x08, //     Report Size (8)
        0x95, 0x02, //     Report Count (2)
        0x81, 0x06, //     Input (Data,Var,Rel) — 相对位移
        0xC0, //         End Collection (Physical)
        0xC0, //       End Collection (Application)
};

SimpleHidInterfaceHandler::SimpleHidInterfaceHandler(usbipdcpp::UsbInterface &handle_interface,
                                                     usbipdcpp::StringPool &string_pool) :
    HidVirtualInterfaceHandler(handle_interface, string_pool) {
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
