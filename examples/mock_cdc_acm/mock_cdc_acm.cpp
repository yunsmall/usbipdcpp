#include "mock_cdc_acm.h"

using namespace usbipdcpp;

// ==================== MockCdcAcmCommunicationInterfaceHandler ====================

MockCdcAcmCommunicationInterfaceHandler::MockCdcAcmCommunicationInterfaceHandler(
    UsbInterface &handle_interface, StringPool &string_pool) :
    CdcAcmCommunicationInterfaceHandler(handle_interface, string_pool) {
}

void MockCdcAcmCommunicationInterfaceHandler::on_set_line_coding(const LineCoding &coding) {
    SPDLOG_INFO("Line coding set: baud={}, data_bits={}, stop_bits={}, parity={}",
                coding.dwDTERate, coding.bDataBits, coding.bCharFormat, coding.bParityType);
}

void MockCdcAcmCommunicationInterfaceHandler::on_set_control_line_state(const ControlSignalState &state) {
    SPDLOG_INFO("Control line state: DTR={}, RTS={}", state.dtr, state.rts);

    // 当 DTR 变为高时，发送 DCD 和 DSR 信号
    if (state.dtr && get_data_handler()) {
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

bool MockCdcAcmDataInterfaceHandler::on_data_received(data_type &&data) {
    if (should_immediately_stop) {
        // 停止状态不处理：返回 false 且不移动 data（契约），请求挂起后随断连清掉
        return false;
    }

    // 回显：把主机数据原样塞给 IN 方向，阻塞发送确保不丢（生成的数据经
    // tx_channel 等主机 IN 请求取走）
    SPDLOG_DEBUG("Echo {} bytes", data.size());
    send_data_blocking(std::move(data));
    return true;
}
