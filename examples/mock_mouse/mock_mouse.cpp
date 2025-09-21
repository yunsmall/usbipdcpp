#include "mock_mouse.h"

using namespace usbipdcpp;

void MockMouseInterfaceHandler::on_new_connection(Session &current_session, error_code &ec) {
    session = &current_session;
    should_immediately_stop = false;
    last_state = State{};
    current_state = State{};
    int_req_queue.clear();
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
            //清空所有发来的中断传输
            while (true) {
                std::uint32_t seqnum{};
                bool has_seqnum = false;
                {
                    std::lock_guard lock(int_req_queue_mutex);
                    auto at_begin = int_req_queue.begin();
                    if (at_begin != int_req_queue.end()) {
                        seqnum = *at_begin;
                        int_req_queue.pop_front();
                        has_seqnum = true;
                    }
                }

                //如果有值，则发送
                if (has_seqnum) {
                    data_type ret(4, 0);
                    {
                        if (current_state.left_pressed) {
                            ret[0] |= 0b00000001;
                        }
                        if (current_state.right_pressed) {
                            ret[0] |= 0b00000010;
                        }
                        if (current_state.middle_pressed) {
                            ret[0] |= 0b00000100;
                        }
                        if (current_state.side_pressed) {
                            ret[0] |= 0b00001000;
                        }
                        if (current_state.extra_pressed) {
                            ret[0] |= 0b00010000;
                        }
                        ret[1] = current_state.move_horizontal;
                        ret[2] = current_state.move_vertical;
                        ret[3] = current_state.wheel_vertical;
                    }

                    if (!should_immediately_stop) {
                        session.load()->submit_ret_submit(
                                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(seqnum, ret)
                                );
                    }
                }
                //没值了就继续等待
                else {
                    break;
                }
            }
            last_state = current_state;
        }
    });
}

void MockMouseInterfaceHandler::on_disconnection(error_code &ec) {
    session = nullptr;
    should_immediately_stop = true;
    state_cv.notify_all();
    send_thread.join();
}

MockMouseInterfaceHandler::MockMouseInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool):
    HidVirtualInterfaceHandler(handle_interface, string_pool) {
}

void MockMouseInterfaceHandler::handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                          std::uint32_t transfer_flags,
                                                          std::uint32_t transfer_buffer_length,
                                                          const data_type &out_data,
                                                          std::error_code &ec) {
    if (ep.is_in()) {
        //往队列里添加东西
        std::lock_guard lock(int_req_queue_mutex);
        int_req_queue.emplace_back(seqnum);
    }
    else {
        {
            std::lock_guard lock(int_req_queue_mutex);
            int_req_queue.clear();
        }
        session.load()->submit_ret_submit(
                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum)
                );
    }

}

void MockMouseInterfaceHandler::request_clear_feature(std::uint16_t feature_selector, std::uint32_t *p_status) {
    SPDLOG_WARN("unhandled request_clear_feature");
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
}

void MockMouseInterfaceHandler::request_endpoint_clear_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                                               std::uint32_t *p_status) {
    SPDLOG_WARN("unhandled request_endpoint_clear_feature");
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
}

std::uint8_t MockMouseInterfaceHandler::request_get_interface(std::uint32_t *p_status) {
    return 0;
}

void MockMouseInterfaceHandler::request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) {
    if (alternate_setting != 0) {
        SPDLOG_WARN("unhandled request_set_interface");
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
    }
}

std::uint16_t MockMouseInterfaceHandler::request_get_status(std::uint32_t *p_status) {
    return 0;
}

std::uint16_t MockMouseInterfaceHandler::request_endpoint_get_status(std::uint8_t ep_address, std::uint32_t *p_status) {
    return 0;
}

void MockMouseInterfaceHandler::request_set_feature(std::uint16_t feature_selector, std::uint32_t *p_status) {
    SPDLOG_WARN("unhandled request_set_feature");
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
}

void MockMouseInterfaceHandler::request_endpoint_set_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                                             std::uint32_t *p_status) {
    SPDLOG_WARN("unhandled request_endpoint_set_feature");
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
}

std::uint16_t MockMouseInterfaceHandler::get_report_descriptor_size() {
    return report_descriptor.size();
}

data_type MockMouseInterfaceHandler::get_report_descriptor() {
    return report_descriptor;

}

void MockMouseInterfaceHandler::handle_non_hid_request_type_control_urb(std::uint32_t seqnum,
                                                                        const UsbEndpoint &ep,
                                                                        std::uint32_t transfer_flags,
                                                                        std::uint32_t transfer_buffer_length,
                                                                        const SetupPacket &setup_packet,
                                                                        const data_type &out_data,
                                                                        std::error_code &ec) {
    session.load()->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_no_iso(seqnum, {}));
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

void MockMouseInterfaceHandler::request_set_report(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                                                   const data_type &data, std::uint32_t *p_status) {
    SPDLOG_WARN("unhandled request_set_report");
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
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
