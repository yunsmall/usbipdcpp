#pragma once

#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>

#include "virtual_device/SimpleVirtualDeviceHandler.h"
#include "virtual_device/CdcAcmVirtualInterfaceHandler.h"
#include "Server.h"
#include "Session.h"


/**
 * @brief CDC ACM 通信接口处理器实现（回显串口）
 */
class MockCdcAcmCommunicationInterfaceHandler : public usbipdcpp::CdcAcmCommunicationInterfaceHandler {
public:
    MockCdcAcmCommunicationInterfaceHandler(usbipdcpp::UsbInterface &handle_interface,
                                             usbipdcpp::StringPool &string_pool);

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

    void on_set_line_coding(const usbipdcpp::LineCoding &coding) override;
    void on_set_control_line_state(const usbipdcpp::ControlSignalState &state) override;

    // 关联的数据接口处理器（用于数据回显）
    class MockCdcAcmDataInterfaceHandler *data_handler = nullptr;
};


/**
 * @brief CDC ACM 数据接口处理器实现（回显串口）
 */
class MockCdcAcmDataInterfaceHandler : public usbipdcpp::CdcAcmDataInterfaceHandler {
public:
    MockCdcAcmDataInterfaceHandler(usbipdcpp::UsbInterface &handle_interface,
                                    usbipdcpp::StringPool &string_pool);

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

    // 接收到数据时，回显
    void on_data_received(usbipdcpp::data_type &&data) override;

    std::atomic_bool should_immediately_stop = false;
};
