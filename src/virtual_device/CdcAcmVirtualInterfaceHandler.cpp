#include "virtual_device/CdcAcmVirtualInterfaceHandler.h"

#include "Session.h"

namespace usbipdcpp {

// ==================== CdcAcmCommunicationInterfaceHandler ====================

CdcAcmCommunicationInterfaceHandler::CdcAcmCommunicationInterfaceHandler(
    UsbInterface &handle_interface, StringPool &string_pool) :
    VirtualInterfaceHandler(handle_interface, string_pool) {
}

void CdcAcmCommunicationInterfaceHandler::handle_non_standard_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep,
        std::uint32_t transfer_flags,
        std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet,
        const data_type &out_data, std::error_code &ec) {

    auto type = static_cast<RequestType>(setup_packet.calc_request_type());
    std::uint32_t status = static_cast<std::uint32_t>(UrbStatusType::StatusOK);

    if (type == RequestType::Class) {
        auto request = static_cast<CdcAcmRequest>(setup_packet.request);

        if (!setup_packet.is_out()) {
            // IN 请求
            data_type result;
            switch (request) {
                case CdcAcmRequest::GetLineCoding: {
                    result = line_coding_.to_bytes();
                    if (setup_packet.length < result.size()) {
                        result.resize(setup_packet.length);
                    }
                    break;
                }
                default: {
                    SPDLOG_ERROR("Unknown CDC ACM IN request 0x{:x}", setup_packet.request);
                    status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
                }
            }
            session->submit_ret_submit(
                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                    seqnum, status, std::move(result)));
        }
        else {
            // OUT 请求
            switch (request) {
                case CdcAcmRequest::SetLineCoding: {
                    auto new_coding = LineCoding::from_bytes(out_data);
                    on_set_line_coding(new_coding);
                    line_coding_ = new_coding;
                    SPDLOG_DEBUG("SET_LINE_CODING: baud={}, data_bits={}, stop_bits={}, parity={}",
                                 line_coding_.dwDTERate, line_coding_.bDataBits,
                                 line_coding_.bCharFormat, line_coding_.bParityType);
                    break;
                }
                case CdcAcmRequest::SetControlLineState: {
                    auto state = ControlSignalState::from_uint16(setup_packet.value);
                    on_set_control_line_state(state);
                    control_signal_state_ = state;
                    SPDLOG_DEBUG("SET_CONTROL_LINE_STATE: DTR={}, RTS={}",
                                 state.dtr, state.rts);
                    break;
                }
                case CdcAcmRequest::SendBreak: {
                    auto duration = setup_packet.value;
                    on_send_break(duration);
                    SPDLOG_DEBUG("SEND_BREAK: duration={}", duration);
                    break;
                }
                default: {
                    SPDLOG_ERROR("Unknown CDC ACM OUT request 0x{:x}", setup_packet.request);
                    status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
                }
            }
            session->submit_ret_submit(
                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                    seqnum, status, {}));
        }
    }
    else {
        // 非 CDC 类请求，交给子类处理
        handle_non_cdc_request_type_control_urb(seqnum, ep, transfer_flags, transfer_buffer_length,
                                                 setup_packet, out_data, ec);
    }
}

void CdcAcmCommunicationInterfaceHandler::handle_interrupt_transfer(
        std::uint32_t seqnum, const UsbEndpoint &ep,
        std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        data_type &&out_data, std::error_code &ec) {

    if (ep.is_in()) {
        // 中断 IN：主机请求状态通知
        std::lock_guard lock(notification_mutex_);

        if (!pending_notification_.empty()) {
            // 有待发送的通知
            auto data = std::move(pending_notification_);
            pending_notification_.clear();
            session->submit_ret_submit(
                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(seqnum, std::move(data)));
        }
        else {
            // 没有待发送的通知，加入队列等待
            std::lock_guard queue_lock(interrupt_req_queue_mutex_);
            interrupt_req_queue_.push_back(seqnum);
        }
    }
    else {
        // 中断 OUT：CDC ACM 通常不使用
        SPDLOG_WARN("CDC ACM communication interface received unexpected interrupt OUT");
        session->submit_ret_submit(
            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum));
    }
}

data_type CdcAcmCommunicationInterfaceHandler::get_class_specific_descriptor() {
    // CDC ACM 类特定描述符
    // Header Functional Descriptor
    data_type descriptor = {
        0x05,       // bLength
        0x24,       // bDescriptorType: CS_INTERFACE
        0x00,       // bDescriptorSubtype: Header
        0x10, 0x01  // bcdCDC: 1.10
    };

    // Call Management Functional Descriptor
    descriptor.insert(descriptor.end(), {
        0x05,       // bLength
        0x24,       // bDescriptorType: CS_INTERFACE
        0x01,       // bDescriptorSubtype: Call Management
        0x00,       // bmCapabilities
        0x01        // bDataInterface: Interface 1
    });

    // ACM Functional Descriptor
    descriptor.insert(descriptor.end(), {
        0x04,       // bLength
        0x24,       // bDescriptorType: CS_INTERFACE
        0x02,       // bDescriptorSubtype: ACM
        0x02        // bmCapabilities: support Set_Line_Coding, Set_Control_Line_State, Send_Break
    });

    // Union Functional Descriptor
    descriptor.insert(descriptor.end(), {
        0x05,       // bLength
        0x24,       // bDescriptorType: CS_INTERFACE
        0x06,       // bDescriptorSubtype: Union
        0x00,       // bMasterInterface: Interface 0
        0x01        // bSlaveInterface0: Interface 1
    });

    return descriptor;
}

void CdcAcmCommunicationInterfaceHandler::on_set_line_coding(const LineCoding &coding) {
    // 默认空实现，子类可重写
}

void CdcAcmCommunicationInterfaceHandler::on_set_control_line_state(const ControlSignalState &state) {
    // 默认空实现，子类可重写
}

void CdcAcmCommunicationInterfaceHandler::on_send_break(std::uint16_t duration) {
    // 默认空实现，子类可重写
}

void CdcAcmCommunicationInterfaceHandler::handle_non_cdc_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep,
        std::uint32_t transfer_flags,
        std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet,
        const data_type &out_data, std::error_code &ec) {
    // 默认返回错误，子类可重写以处理非 CDC 请求
    SPDLOG_WARN("Unhandled request type 0x{:x} in CDC ACM communication interface",
                setup_packet.calc_request_type());
    session->submit_ret_submit(
        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
            seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE), {}));
}

// ==================== CdcAcmCommunicationInterfaceHandler 默认实现 ====================

void CdcAcmCommunicationInterfaceHandler::request_clear_feature(
        std::uint16_t feature_selector, std::uint32_t *p_status) {
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
}

void CdcAcmCommunicationInterfaceHandler::request_endpoint_clear_feature(
        std::uint16_t feature_selector, std::uint8_t ep_address, std::uint32_t *p_status) {
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
}

std::uint8_t CdcAcmCommunicationInterfaceHandler::request_get_interface(std::uint32_t *p_status) {
    return 0;
}

void CdcAcmCommunicationInterfaceHandler::request_set_interface(
        std::uint16_t alternate_setting, std::uint32_t *p_status) {
    if (alternate_setting != 0) {
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
    }
}

std::uint16_t CdcAcmCommunicationInterfaceHandler::request_get_status(std::uint32_t *p_status) {
    return 0;
}

std::uint16_t CdcAcmCommunicationInterfaceHandler::request_endpoint_get_status(
        std::uint8_t ep_address, std::uint32_t *p_status) {
    return 0;
}

void CdcAcmCommunicationInterfaceHandler::request_set_feature(
        std::uint16_t feature_selector, std::uint32_t *p_status) {
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
}

void CdcAcmCommunicationInterfaceHandler::request_endpoint_set_feature(
        std::uint16_t feature_selector, std::uint8_t ep_address, std::uint32_t *p_status) {
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
}

void CdcAcmCommunicationInterfaceHandler::send_serial_state_notification(std::uint16_t state_bits) {
    SerialStateNotification notification;
    notification.data = state_bits;

    std::lock_guard lock(notification_mutex_);
    pending_notification_ = notification.to_bytes();

    // 如果有等待的中断请求，立即响应
    std::uint32_t seqnum = 0;
    bool has_pending = false;
    {
        std::lock_guard queue_lock(interrupt_req_queue_mutex_);
        if (!interrupt_req_queue_.empty()) {
            seqnum = interrupt_req_queue_.front();
            interrupt_req_queue_.pop_front();
            has_pending = true;
        }
    }

    if (has_pending) {
        auto data = std::move(pending_notification_);
        pending_notification_.clear();
        session->submit_ret_submit(
            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(seqnum, std::move(data)));
    }
}

// ==================== CdcAcmDataInterfaceHandler ====================

CdcAcmDataInterfaceHandler::CdcAcmDataInterfaceHandler(
    UsbInterface &handle_interface, StringPool &string_pool) :
    VirtualInterfaceHandler(handle_interface, string_pool) {
}

void CdcAcmDataInterfaceHandler::handle_bulk_transfer(
        std::uint32_t seqnum, const UsbEndpoint &ep,
        std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        data_type &&out_data, std::error_code &ec) {

    if (ep.is_in()) {
        // Bulk IN：主机请求数据
        std::lock_guard lock(tx_data_mutex_);

        SPDLOG_INFO("bulk in transfer_buffer_length:{}",transfer_buffer_length);

        if (!pending_tx_data_.empty()) {
            // 有待发送的数据，支持部分发送避免截断丢失
            auto &pending = pending_tx_data_.front();
            std::uint32_t send_len = std::min(pending.remaining(), transfer_buffer_length);

            // 如果剩余数据全部发送，可以直接移动避免拷贝
            if (send_len == pending.remaining()) {
                session->submit_ret_submit(
                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
                        seqnum, std::move(pending.data)));
                pending_tx_data_.pop_front();
            }
            else {
                // 部分发送，需要拷贝
                data_type to_send(pending.data.begin() + pending.offset,
                                  pending.data.begin() + pending.offset + send_len);
                pending.offset += send_len;
                session->submit_ret_submit(
                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(seqnum, std::move(to_send)));
            }
        }
        else {
            // 尝试从回调获取数据
            auto data = on_data_requested(transfer_buffer_length);
            if (!data.empty()) {
                // 回调返回的数据可能也需要部分发送
                if (data.size() <= transfer_buffer_length) {
                    session->submit_ret_submit(
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(seqnum, std::move(data)));
                }
                else {
                    // 数据大于请求长度，部分发送后剩余入队列
                    data_type to_send(data.begin(), data.begin() + transfer_buffer_length);
                    PendingData remaining;
                    remaining.data = data_type(data.begin() + transfer_buffer_length, data.end());
                    remaining.offset = 0;
                    session->submit_ret_submit(
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(seqnum, std::move(to_send)));
                    pending_tx_data_.push_front(std::move(remaining));
                }
            }
            else {
                // 没有数据可发送，加入队列等待
                std::lock_guard queue_lock(bulk_in_req_queue_mutex_);
                bulk_in_req_queue_.push_back({seqnum, transfer_buffer_length});
            }
        }
    }
    else {
        // Bulk OUT：接收主机发来的数据
        if (out_data.size() > transfer_buffer_length) {
            out_data.resize(transfer_buffer_length);
        }

        on_data_received(std::move(out_data));

        session->submit_ret_submit(
            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(seqnum));
    }
}

void CdcAcmDataInterfaceHandler::handle_non_standard_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep,
        std::uint32_t transfer_flags,
        std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet,
        const data_type &out_data, std::error_code &ec) {

    // 数据接口通常不处理类特定控制请求
    SPDLOG_WARN("CDC ACM data interface received unexpected control request");
    session->submit_ret_submit(
        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
            seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE), {}));
}

data_type CdcAcmDataInterfaceHandler::get_class_specific_descriptor() {
    // 数据接口没有类特定描述符
    return {};
}

void CdcAcmDataInterfaceHandler::on_data_received(data_type &&data) {
    // 默认空实现，子类可重写
}

data_type CdcAcmDataInterfaceHandler::on_data_requested(std::uint16_t length) {
    // 默认返回空，子类可重写
    return {};
}

void CdcAcmDataInterfaceHandler::send_data(const data_type &data) {
    std::lock_guard lock(tx_data_mutex_);
    PendingData pending;
    pending.data = data;
    pending.offset = 0;
    pending_tx_data_.push_back(std::move(pending));

    // 如果有等待的 Bulk IN 请求，立即响应
    BulkInRequest request{};
    bool has_pending = false;
    {
        std::lock_guard queue_lock(bulk_in_req_queue_mutex_);
        if (!bulk_in_req_queue_.empty()) {
            request = bulk_in_req_queue_.front();
            bulk_in_req_queue_.pop_front();
            has_pending = true;
        }
    }

    if (has_pending) {
        auto &front = pending_tx_data_.front();
        std::uint32_t send_len = std::min(front.remaining(), request.length);

        // const 引用版本必须拷贝
        data_type to_send(front.data.begin() + front.offset,
                          front.data.begin() + front.offset + send_len);

        front.offset += send_len;
        if (front.offset >= front.data.size()) {
            pending_tx_data_.pop_front();
        }

        session->submit_ret_submit(
            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(request.seqnum, std::move(to_send)));
    }
}

void CdcAcmDataInterfaceHandler::send_data(data_type &&data) {
    std::lock_guard lock(tx_data_mutex_);
    PendingData pending;
    pending.data = std::move(data);
    pending.offset = 0;
    pending_tx_data_.push_back(std::move(pending));

    // 如果有等待的 Bulk IN 请求，立即响应
    BulkInRequest request{};
    bool has_pending = false;
    {
        std::lock_guard queue_lock(bulk_in_req_queue_mutex_);
        if (!bulk_in_req_queue_.empty()) {
            request = bulk_in_req_queue_.front();
            bulk_in_req_queue_.pop_front();
            has_pending = true;
        }
    }

    if (has_pending) {
        auto &front = pending_tx_data_.front();
        std::uint32_t send_len = std::min(front.remaining(), request.length);

        // 如果数据全部发送，直接移动避免拷贝
        if (send_len == front.remaining()) {
            session->submit_ret_submit(
                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
                    request.seqnum, std::move(front.data)));
            pending_tx_data_.pop_front();
        }
        else {
            // 部分发送，需要拷贝
            data_type to_send(front.data.begin() + front.offset,
                              front.data.begin() + front.offset + send_len);
            front.offset += send_len;
            session->submit_ret_submit(
                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(request.seqnum, std::move(to_send)));
        }
    }
}

void CdcAcmDataInterfaceHandler::send_data(std::string_view data) {
    send_data(data_type(data.begin(), data.end()));
}

// ==================== CdcAcmDataInterfaceHandler 默认实现 ====================

void CdcAcmDataInterfaceHandler::request_clear_feature(
        std::uint16_t feature_selector, std::uint32_t *p_status) {
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
}

void CdcAcmDataInterfaceHandler::request_endpoint_clear_feature(
        std::uint16_t feature_selector, std::uint8_t ep_address, std::uint32_t *p_status) {
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
}

std::uint8_t CdcAcmDataInterfaceHandler::request_get_interface(std::uint32_t *p_status) {
    return 0;
}

void CdcAcmDataInterfaceHandler::request_set_interface(
        std::uint16_t alternate_setting, std::uint32_t *p_status) {
    if (alternate_setting != 0) {
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
    }
}

std::uint16_t CdcAcmDataInterfaceHandler::request_get_status(std::uint32_t *p_status) {
    return 0;
}

std::uint16_t CdcAcmDataInterfaceHandler::request_endpoint_get_status(
        std::uint8_t ep_address, std::uint32_t *p_status) {
    return 0;
}

void CdcAcmDataInterfaceHandler::request_set_feature(
        std::uint16_t feature_selector, std::uint32_t *p_status) {
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
}

void CdcAcmDataInterfaceHandler::request_endpoint_set_feature(
        std::uint16_t feature_selector, std::uint8_t ep_address, std::uint32_t *p_status) {
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
}

}
