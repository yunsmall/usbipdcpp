#pragma once

#include <atomic>

#include "virtual_device/CdcAcmVirtualInterfaceHandler.h"


/**
 * @brief CDC ACM 通信接口处理器实现（回显串口）
 */
class MockCdcAcmCommunicationInterfaceHandler : public usbipdcpp::CdcAcmCommunicationInterfaceHandler {
public:
    MockCdcAcmCommunicationInterfaceHandler(usbipdcpp::UsbInterface &handle_interface,
                                             usbipdcpp::StringPool &string_pool);

    void on_set_line_coding(const usbipdcpp::LineCoding &coding) override;
    void on_set_control_line_state(const usbipdcpp::ControlSignalState &state) override;
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

    void on_data_received(usbipdcpp::data_type &&data) override;

    std::atomic_bool should_immediately_stop = false;
};
