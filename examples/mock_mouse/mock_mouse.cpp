#include "mock_mouse.h"

using namespace usbipdcpp;

void MockMouseInterfaceHandler::on_new_connection(Session &current_session, error_code &ec) {
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
            std::array<std::uint8_t, 4> report{};
            if (current_state.left_pressed) {
                report[0] |= 0b00000001;
            }
            if (current_state.right_pressed) {
                report[0] |= 0b00000010;
            }
            if (current_state.middle_pressed) {
                report[0] |= 0b00000100;
            }
            if (current_state.side_pressed) {
                report[0] |= 0b00001000;
            }
            if (current_state.extra_pressed) {
                report[0] |= 0b00010000;
            }
            report[1] = static_cast<std::uint8_t>(current_state.move_horizontal);
            report[2] = static_cast<std::uint8_t>(current_state.move_vertical);
            report[3] = static_cast<std::uint8_t>(current_state.wheel_vertical);

            // 使用基类的 send_input_report 发送报告
            send_input_report(asio::buffer(report));

            last_state = current_state;
        }
    });
}

void MockMouseInterfaceHandler::on_disconnection(error_code &ec) {
    should_immediately_stop = true;
    state_cv.notify_all();
    send_thread.join();
    HidVirtualInterfaceHandler::on_disconnection(ec);
}

MockMouseInterfaceHandler::MockMouseInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool) :
    HidVirtualInterfaceHandler(handle_interface, string_pool) {
}

std::uint16_t MockMouseInterfaceHandler::get_report_descriptor_size() {
    return report_descriptor.size();
}

data_type MockMouseInterfaceHandler::get_report_descriptor() {
    return report_descriptor;
}

data_type MockMouseInterfaceHandler::request_get_report(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                                                        std::uint32_t *p_status) {
    auto report_type = static_cast<HIDReportType>(type);
    if (report_type == HIDReportType::Input) {
        std::unique_lock lock(state_mutex);
        data_type result;
        switch (report_id) {
            case 0: {
                vector_append_to_net(result, (std::uint8_t) current_state.left_pressed);
                break;
            }
            case 1: {
                vector_append_to_net(result, (std::uint8_t) current_state.right_pressed);
                break;
            }
            case 2: {
                vector_append_to_net(result, (std::uint8_t) current_state.middle_pressed);
                break;
            }
            case 3: {
                vector_append_to_net(result, (std::uint8_t) current_state.side_pressed);
                break;
            }
            case 4: {
                vector_append_to_net(result, (std::uint8_t) current_state.extra_pressed);
                break;
            }
            case 5: {
                vector_append_to_net(result, (std::uint8_t) current_state.wheel_vertical);
                break;
            }
            case 6: {
                vector_append_to_net(result, (std::uint8_t) current_state.wheel_vertical);
                break;
            }
            case 7: {
                vector_append_to_net(result, (std::uint8_t) current_state.move_horizontal);
                break;
            }
            case 8: {
                vector_append_to_net(result, (std::uint8_t) current_state.move_vertical);
                break;
            }
            default: {
                SPDLOG_WARN("unhandled request_get_report");
                *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
            }
        }
        return result;
    }
    SPDLOG_WARN("unhandled request_get_report");
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
    return {};
}

data_type MockMouseInterfaceHandler::request_get_idle(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                                                      std::uint32_t *p_status) {
    data_type result;
    vector_append_to_net(result, (std::uint16_t) idle_speed);
    return result;
}

void MockMouseInterfaceHandler::request_set_idle(std::uint8_t speed, std::uint32_t *p_status) {
    idle_speed = speed;
}
