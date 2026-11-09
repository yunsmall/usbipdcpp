#pragma once

#include <cstdint>

#include "usbipdcpp/virtual_device/SimpleVirtualDeviceHandler.h"
#include "usbipdcpp/virtual_device/HidVirtualInterfaceHandler.h"
#include "usbipdcpp/Server.h"
#include "usbipdcpp/Session.h"
#include "usbipdcpp/protocol.h"

/**
 * @brief 简单的虚拟HID设备接口处理器
 * 用于创建基本的虚拟输入设备
 */
class SimpleHidInterfaceHandler : public usbipdcpp::HidVirtualInterfaceHandler {
public:
    explicit SimpleHidInterfaceHandler(usbipdcpp::UsbInterface &handle_interface);

    std::uint16_t get_report_descriptor_size() override;
    usbipdcpp::data_type get_report_descriptor() override;

private:
    // 简单的HID报告描述符 - 一个按钮
    static const usbipdcpp::data_type report_descriptor_;
};

/**
 * @brief 创建简单虚拟设备的设备处理器
 */
class SimpleDeviceHandler : public usbipdcpp::SimpleVirtualDeviceHandler {
public:
    explicit SimpleDeviceHandler(usbipdcpp::UsbDevice &handle_device);
};
