#pragma once

#include <memory>
#include <string>

#include "usbipdcpp/Device.h"

/**
 * @brief 设备工厂类，用于创建虚拟USB设备
 */
class DeviceFactory {
public:
    /**
     * @brief 创建一个简单的虚拟HID设备
     * @param index 设备索引 (1-10)
     * @return 创建的设备共享指针
     */
    static std::shared_ptr<usbipdcpp::UsbDevice> create_simple_device(int index);

    /**
     * @brief 创建多个虚拟设备
     * @param count 设备数量
     * @return 设备列表
     */
    static std::vector<std::shared_ptr<usbipdcpp::UsbDevice>> create_devices(int count);

private:
    /**
     * @brief 生成设备busid
     * @param index 设备索引
     * @return busid字符串，格式如 "1-1", "1-2" 等
     */
    static std::string generate_busid(int index);

    /**
     * @brief 生成设备路径
     * @param index 设备索引
     * @return 设备路径
     */
    static std::string generate_path(int index);

    /**
     * @brief 生成vendor ID
     * @param index 设备索引
     * @return vendor ID
     */
    static std::uint16_t generate_vendor_id(int index);

    /**
     * @brief 生成product ID
     * @param index 设备索引
     * @return product ID
     */
    static std::uint16_t generate_product_id(int index);
};
