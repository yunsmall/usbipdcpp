#include "mock_cdc_acm.h"

using namespace usbipdcpp;

// ==================== MockCdcAcmCommunicationInterfaceHandler ====================

MockCdcAcmCommunicationInterfaceHandler::MockCdcAcmCommunicationInterfaceHandler(
    UsbInterface &handle_interface, StringPool &string_pool) :
    CdcAcmCommunicationInterfaceHandler(handle_interface, string_pool) {
}

void MockCdcAcmCommunicationInterfaceHandler::request_clear_feature(
    std::uint16_t feature_selector, std::uint32_t *p_status) {
    SPDLOG_WARN("unhandled request_clear_feature");
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
}

void MockCdcAcmCommunicationInterfaceHandler::request_endpoint_clear_feature(
    std::uint16_t feature_selector, std::uint8_t ep_address, std::uint32_t *p_status) {
    SPDLOG_WARN("unhandled request_endpoint_clear_feature");
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
}

std::uint8_t MockCdcAcmCommunicationInterfaceHandler::request_get_interface(std::uint32_t *p_status) {
    return 0;
}

void MockCdcAcmCommunicationInterfaceHandler::request_set_interface(
    std::uint16_t alternate_setting, std::uint32_t *p_status) {
    if (alternate_setting != 0) {
        SPDLOG_WARN("unhandled request_set_interface");
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
    }
}

std::uint16_t MockCdcAcmCommunicationInterfaceHandler::request_get_status(std::uint32_t *p_status) {
    return 0;
}

std::uint16_t MockCdcAcmCommunicationInterfaceHandler::request_endpoint_get_status(
    std::uint8_t ep_address, std::uint32_t *p_status) {
    return 0;
}

void MockCdcAcmCommunicationInterfaceHandler::request_set_feature(
    std::uint16_t feature_selector, std::uint32_t *p_status) {
    SPDLOG_WARN("unhandled request_set_feature");
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
}

void MockCdcAcmCommunicationInterfaceHandler::request_endpoint_set_feature(
    std::uint16_t feature_selector, std::uint8_t ep_address, std::uint32_t *p_status) {
    SPDLOG_WARN("unhandled request_endpoint_set_feature");
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
}

void MockCdcAcmCommunicationInterfaceHandler::on_set_line_coding(const LineCoding &coding) {
    SPDLOG_INFO("Line coding set: baud={}, data_bits={}, stop_bits={}, parity={}",
                coding.dwDTERate, coding.bDataBits, coding.bCharFormat, coding.bParityType);
}

void MockCdcAcmCommunicationInterfaceHandler::on_set_control_line_state(const ControlSignalState &state) {
    SPDLOG_INFO("Control line state: DTR={}, RTS={}", state.dtr, state.rts);

    // 当 DTR 变为高时，发送 DCD 和 DSR 信号
    if (state.dtr && data_handler) {
        // 发送状态通知：DCD 和 DSR 有效
        send_serial_state_notification(static_cast<std::uint16_t>(CdcAcmSerialState::DCD) |
                                       static_cast<std::uint16_t>(CdcAcmSerialState::DSR));
    }
}

// ==================== MockCdcAcmDataInterfaceHandler ====================

MockCdcAcmDataInterfaceHandler::MockCdcAcmDataInterfaceHandler(
    UsbInterface &handle_interface, StringPool &string_pool) :
    CdcAcmDataInterfaceHandler(handle_interface, string_pool) {
}

void MockCdcAcmDataInterfaceHandler::on_new_connection(Session &current_session, error_code &ec) {
    CdcAcmDataInterfaceHandler::on_new_connection(current_session, ec);
    should_immediately_stop = false;
}

void MockCdcAcmDataInterfaceHandler::on_disconnection(error_code &ec) {
    should_immediately_stop = true;
    CdcAcmDataInterfaceHandler::on_disconnection(ec);
}

void MockCdcAcmDataInterfaceHandler::request_clear_feature(
    std::uint16_t feature_selector, std::uint32_t *p_status) {
    SPDLOG_WARN("unhandled request_clear_feature");
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
}

void MockCdcAcmDataInterfaceHandler::request_endpoint_clear_feature(
    std::uint16_t feature_selector, std::uint8_t ep_address, std::uint32_t *p_status) {
    SPDLOG_WARN("unhandled request_endpoint_clear_feature");
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
}

std::uint8_t MockCdcAcmDataInterfaceHandler::request_get_interface(std::uint32_t *p_status) {
    return 0;
}

void MockCdcAcmDataInterfaceHandler::request_set_interface(
    std::uint16_t alternate_setting, std::uint32_t *p_status) {
    if (alternate_setting != 0) {
        SPDLOG_WARN("unhandled request_set_interface");
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
    }
}

std::uint16_t MockCdcAcmDataInterfaceHandler::request_get_status(std::uint32_t *p_status) {
    return 0;
}

std::uint16_t MockCdcAcmDataInterfaceHandler::request_endpoint_get_status(
    std::uint8_t ep_address, std::uint32_t *p_status) {
    return 0;
}

void MockCdcAcmDataInterfaceHandler::request_set_feature(
    std::uint16_t feature_selector, std::uint32_t *p_status) {
    SPDLOG_WARN("unhandled request_set_feature");
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
}

void MockCdcAcmDataInterfaceHandler::request_endpoint_set_feature(
    std::uint16_t feature_selector, std::uint8_t ep_address, std::uint32_t *p_status) {
    SPDLOG_WARN("unhandled request_endpoint_set_feature");
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
}

void MockCdcAcmDataInterfaceHandler::on_data_received(const data_type &data) {
    if (should_immediately_stop) {
        return;
    }

    // 回显：将接收到的数据原样发回
    SPDLOG_DEBUG("Echo {} bytes", data.size());
    send_data(data);
}
