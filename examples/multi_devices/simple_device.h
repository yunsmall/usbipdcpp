#pragma once

#include <cstdint>

#include "virtual_device/SimpleVirtualDeviceHandler.h"
#include "virtual_device/HidVirtualInterfaceHandler.h"
#include "Server.h"
#include "Session.h"
#include "protocol.h"

/**
 * @brief 简单的虚拟HID设备接口处理器
 * 用于创建基本的虚拟输入设备
 */
class SimpleHidInterfaceHandler : public usbipdcpp::HidVirtualInterfaceHandler {
public:
    explicit SimpleHidInterfaceHandler(usbipdcpp::UsbInterface &handle_interface, usbipdcpp::StringPool &string_pool);

    std::uint16_t get_report_descriptor_size() override;
    usbipdcpp::data_type get_report_descriptor() override;

    // 重写：主机请求输入报告时返回固定数据
    usbipdcpp::data_type on_input_report_requested(std::uint16_t length) override;

private:
    // 简单的HID报告描述符 - 一个按钮
    static const usbipdcpp::data_type report_descriptor_;
};

/**
 * @brief 创建简单虚拟设备的设备处理器
 */
class SimpleDeviceHandler : public usbipdcpp::SimpleVirtualDeviceHandler {
public:
    SimpleDeviceHandler(usbipdcpp::UsbDevice &handle_device, usbipdcpp::StringPool &string_pool);
};
