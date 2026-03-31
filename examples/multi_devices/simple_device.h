#pragma once

#include <cstdint>

#include "DeviceHandler/SimpleVirtualDeviceHandler.h"
#include "InterfaceHandler/HidVirtualInterfaceHandler.h"
#include "Server.h"
#include "Session.h"

/**
 * @brief 简单的虚拟HID设备接口处理器
 * 用于创建基本的虚拟输入设备
 */
class SimpleHidInterfaceHandler : public usbipdcpp::HidVirtualInterfaceHandler {
public:
    explicit SimpleHidInterfaceHandler(usbipdcpp::UsbInterface &handle_interface, usbipdcpp::StringPool &string_pool);

    void handle_interrupt_transfer(std::uint32_t seqnum, const usbipdcpp::UsbEndpoint &ep,
                                   std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
                                   const usbipdcpp::data_type &out_data,
                                   std::error_code &ec) override;
    void on_new_connection(usbipdcpp::Session &current_session, usbipdcpp::error_code &ec) override;
    void on_disconnection(usbipdcpp::error_code &ec) override;
    void request_clear_feature(std::uint16_t feature_selector, std::uint32_t *p_status) override;

    void request_endpoint_clear_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                        std::uint32_t *p_status) override;

    std::uint8_t request_get_interface(std::uint32_t *p_status) override;

    void request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) override;

    std::uint16_t request_get_status(std::uint32_t *p_status) override;

    std::uint16_t request_endpoint_get_status(std::uint8_t ep_address, std::uint32_t *p_status) override;

    void request_set_feature(std::uint16_t feature_selector, std::uint32_t *p_status) override;

    void request_endpoint_set_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                      std::uint32_t *p_status) override;

    std::uint16_t get_report_descriptor_size() override;

    usbipdcpp::data_type get_report_descriptor() override;

    void handle_non_hid_request_type_control_urb(std::uint32_t seqnum, const usbipdcpp::UsbEndpoint &ep,
                                                 std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
                                                 const usbipdcpp::SetupPacket &setup_packet,
                                                 const usbipdcpp::data_type &out_data, std::error_code &ec) override;
    usbipdcpp::data_type request_get_report(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                                            std::uint32_t *p_status) override;
    void request_set_report(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                            const usbipdcpp::data_type &data,
                            std::uint32_t *p_status) override;
    usbipdcpp::data_type request_get_idle(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                                          std::uint32_t *p_status) override;
    void request_set_idle(std::uint8_t speed, std::uint32_t *p_status) override;

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
