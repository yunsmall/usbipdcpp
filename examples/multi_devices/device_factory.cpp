#include "device_factory.h"
#include "simple_device.h"

#include <format>
#include <spdlog/spdlog.h>

#include "endpoint.h"
#include "constant.h"

std::shared_ptr<usbipdcpp::UsbDevice> DeviceFactory::create_simple_device(int index, usbipdcpp::StringPool &string_pool) {
    // 创建接口端点
    std::vector<usbipdcpp::UsbInterface> interfaces = {
        usbipdcpp::UsbInterface{
            .interface_class = static_cast<std::uint8_t>(usbipdcpp::ClassCode::HID),
            .interface_subclass = 0x00,
            .interface_protocol = 0x00,
            .endpoints = {
                usbipdcpp::UsbEndpoint{
                    .address = 0x81,  // IN endpoint
                    .attributes = 0x03, // Interrupt
                    .max_packet_size = 8,
                    .interval = 10
                }
            }
        }
    };

    // 为接口设置处理器
    interfaces[0].with_handler<SimpleHidInterfaceHandler>(string_pool);

    // 创建设备
    auto device = std::make_shared<usbipdcpp::UsbDevice>(usbipdcpp::UsbDevice{
        .path = generate_path(index),
        .busid = generate_busid(index),
        .bus_num = 1,
        .dev_num = static_cast<std::uint32_t>(index),
        .speed = static_cast<std::uint32_t>(usbipdcpp::UsbSpeed::Full),
        .vendor_id = generate_vendor_id(index),
        .product_id = generate_product_id(index),
        .device_bcd = 0x0100,
        .device_class = 0x00,
        .device_subclass = 0x00,
        .device_protocol = 0x00,
        .configuration_value = 1,
        .num_configurations = 1,
        .interfaces = interfaces,
        .ep0_in = usbipdcpp::UsbEndpoint::get_default_ep0_in(),
        .ep0_out = usbipdcpp::UsbEndpoint::get_default_ep0_out(),
    });

    // 为设备设置处理器
    device->with_handler<SimpleDeviceHandler>(string_pool);

    SPDLOG_INFO("Created device {}: VID={:04x} PID={:04x} busid={}",
                index, generate_vendor_id(index), generate_product_id(index), generate_busid(index));

    return device;
}

std::vector<std::shared_ptr<usbipdcpp::UsbDevice>> DeviceFactory::create_devices(int count,
                                                                                  usbipdcpp::StringPool &string_pool) {
    std::vector<std::shared_ptr<usbipdcpp::UsbDevice>> devices;
    devices.reserve(count);

    for (int i = 1; i <= count; ++i) {
        devices.push_back(create_simple_device(i, string_pool));
    }

    SPDLOG_INFO("Created {} virtual devices", count);
    return devices;
}

std::string DeviceFactory::generate_busid(int index) {
    return std::format("1-{}", index);
}

std::string DeviceFactory::generate_path(int index) {
    return std::format("/usbipdcpp/simple_device_{}", index);
}

std::uint16_t DeviceFactory::generate_vendor_id(int index) {
    // 使用基础VID + index，保证每个设备有不同的VID
    return static_cast<std::uint16_t>(0x1234 + index - 1);
}

std::uint16_t DeviceFactory::generate_product_id(int index) {
    // 使用基础PID + index，保证每个设备有不同的PID
    return static_cast<std::uint16_t>(0x5678 + index - 1);
}
