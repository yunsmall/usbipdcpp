#include "mock_keyboard.h"

using namespace usbipdcpp;

void MockKeyboardInterfaceHandler::on_new_connection(Session &current_session, error_code &ec) {
    HidVirtualInterfaceHandler::on_new_connection(current_session, ec);
    should_immediately_stop = false;
    last_state = State{};
    current_state = State{};
    idle_speed = 1;

    send_thread = std::thread([this]() {
        while (!should_immediately_stop) {
            //等待状态变化通知
            std::unique_lock lock(state_mutex);
            state_cv.wait(lock, [this]() {
                return current_state != last_state || should_immediately_stop;
            });
            if (should_immediately_stop)
                break;

            // 构造报告数据
            std::array<std::uint8_t, 8> report{};
            report[0] = current_state.modifier;
            for (size_t i = 0; i < 6; ++i) {
                report[2 + i] = current_state.keys[i];
            }

            // 使用基类的 send_input_report 发送报告
            send_input_report(asio::buffer(report));

            last_state = current_state;
        }
    });
}

void MockKeyboardInterfaceHandler::on_disconnection(error_code &ec) {
    HidVirtualInterfaceHandler::on_disconnection(ec);
    should_immediately_stop = true;
    state_cv.notify_all();
    send_thread.join();
}

MockKeyboardInterfaceHandler::MockKeyboardInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool) :
    HidVirtualInterfaceHandler(handle_interface, string_pool) {
}

// 主机请求输入报告时返回当前状态
data_type MockKeyboardInterfaceHandler::on_input_report_requested(std::uint16_t length) {
    std::lock_guard lock(state_mutex);
    data_type result(8, 0);
    result[0] = current_state.modifier;
    for (size_t i = 0; i < 6; ++i) {
        result[2 + i] = current_state.keys[i];
    }
    if (result.size() > length) {
        result.resize(length);
    }
    return result;
}

std::uint16_t MockKeyboardInterfaceHandler::get_report_descriptor_size() {
    return report_descriptor.size();
}

data_type MockKeyboardInterfaceHandler::get_report_descriptor() {
    return report_descriptor;
}

data_type MockKeyboardInterfaceHandler::request_get_report(std::uint8_t type, std::uint8_t report_id,
                                                           std::uint16_t length,
                                                           std::uint32_t *p_status) {
    auto report_type = static_cast<HIDReportType>(type);
    if (report_type == HIDReportType::Input) {
        std::unique_lock lock(state_mutex);
        data_type result(8, 0);
        result[0] = current_state.modifier;
        for (size_t i = 0; i < 6; ++i) {
            result[2 + i] = current_state.keys[i];
        }
        return result;
    }
    SPDLOG_WARN("unhandled request_get_report");
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
    return {};
}

data_type MockKeyboardInterfaceHandler::request_get_idle(std::uint8_t type, std::uint8_t report_id,
                                                         std::uint16_t length,
                                                         std::uint32_t *p_status) {
    data_type result;
    vector_append_to_net(result, (std::uint16_t) idle_speed);
    return result;
}

void MockKeyboardInterfaceHandler::request_set_idle(std::uint8_t speed, std::uint32_t *p_status) {
    idle_speed = speed;
}
