#include "virtual_device/CdcAcmVirtualInterfaceHandler.h"

#include "DeviceHandler/DeviceHandler.h"

#include "Session.h"
#include <algorithm>

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
        TransferHandle transfer, std::error_code &ec) {

    auto type = static_cast<RequestType>(setup_packet.calc_request_type());
    std::uint32_t status = static_cast<std::uint32_t>(UrbStatusType::StatusOK);

    if (type == RequestType::Class) {
        auto request = static_cast<CdcAcmRequest>(setup_packet.request);

        if (!setup_packet.is_out()) {
            // IN 请求
            auto* trx = GenericTransfer::from_handle(transfer.get());
            switch (request) {
                case CdcAcmRequest::GetLineCoding: {
                    auto bytes = line_coding_.to_bytes();
                    trx->data.assign(bytes.begin(), bytes.end());
                    if (setup_packet.length < trx->data.size()) {
                        trx->data.resize(setup_packet.length);
                    }
                    trx->actual_length = trx->data.size();
                    break;
                }
                default: {
                    SPDLOG_ERROR("Unknown CDC ACM IN request 0x{:x}", setup_packet.request);
                    status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
                    trx->actual_length = 0;
                }
            }
            session->submit_ret_submit(
                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                            seqnum, status, static_cast<std::uint32_t>(trx->actual_length), std::move(transfer)));
        }
        else {
            // OUT 请求
            auto* trx = GenericTransfer::from_handle(transfer.get());
            auto& out_data = trx->data;
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
                    // 通知数据接口 RTS 状态变化
                    if (data_handler_) {
                        data_handler_->on_rts_changed(state.rts);
                    }
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
            // transfer 析构时自动释放
            session->submit_ret_submit(
                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(
                            seqnum, status, 0));
        }
    }
    else {
        // 非 CDC 类请求，交给子类处理
        handle_non_cdc_request_type_control_urb(seqnum, ep, transfer_flags, transfer_buffer_length,
                                                setup_packet, std::move(transfer), ec);
    }
}

void CdcAcmCommunicationInterfaceHandler::handle_interrupt_transfer(
        std::uint32_t seqnum, const UsbEndpoint &ep,
        std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        TransferHandle transfer, std::error_code &ec) {

    if (ep.is_in()) {
        // 同时锁两个 mutex，避免竞态条件
        std::lock(notification_mutex_, endpoint_requests_mutex_);
        std::lock_guard lock1(notification_mutex_, std::adopt_lock);
        std::lock_guard lock2(endpoint_requests_mutex_, std::adopt_lock);

        if (!pending_notification_.empty() && endpoint_requests_.empty(ep.address)) {
            // 有待发送的通知且没有队列中的请求，立即响应
            auto* trx = GenericTransfer::from_handle(transfer.get());
            trx->data = std::move(pending_notification_);
            trx->actual_length = trx->data.size();
            pending_notification_.clear();
            session->submit_ret_submit(
                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
                            seqnum, static_cast<std::uint32_t>(trx->actual_length), std::move(transfer)));
        }
        else {
            // 将请求加入队列，等待处理
            endpoint_requests_.enqueue(ep.address, {seqnum, transfer_buffer_length, std::move(transfer)});
        }
    }
    else {
        // 中断 OUT：CDC ACM 通常不使用
        // transfer 析构时自动释放
        SPDLOG_WARN("CDC ACM communication interface received unexpected interrupt OUT");
        session->submit_ret_submit(
                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
    }
}

data_type CdcAcmCommunicationInterfaceHandler::get_class_specific_descriptor() {
    // CDC ACM 类特定描述符
    // Header Functional Descriptor
    data_type descriptor = {
            0x05,      // bLength
            0x24,      // bDescriptorType: CS_INTERFACE
            0x00,      // bDescriptorSubtype: Header
            0x10, 0x01 // bcdCDC: 1.10
    };

    // Call Management Functional Descriptor
    descriptor.insert(descriptor.end(), {
                              0x05, // bLength
                              0x24, // bDescriptorType: CS_INTERFACE
                              0x01, // bDescriptorSubtype: Call Management
                              0x00, // bmCapabilities
                              0x01  // bDataInterface: Interface 1
                      });

    // ACM Functional Descriptor
    descriptor.insert(descriptor.end(), {
                              0x04, // bLength
                              0x24, // bDescriptorType: CS_INTERFACE
                              0x02, // bDescriptorSubtype: ACM
                              0x02  // bmCapabilities: support Set_Line_Coding, Set_Control_Line_State, Send_Break
                      });

    // Union Functional Descriptor
    descriptor.insert(descriptor.end(), {
                              0x05, // bLength
                              0x24, // bDescriptorType: CS_INTERFACE
                              0x06, // bDescriptorSubtype: Union
                              0x00, // bMasterInterface: Interface 0
                              0x01  // bSlaveInterface0: Interface 1
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
        TransferHandle transfer, std::error_code &ec) {
    // 默认返回错误，子类可重写以处理非 CDC 请求
    SPDLOG_WARN("Unhandled request type 0x{:x} in CDC ACM communication interface",
                setup_packet.calc_request_type());
    // transfer 析构时自动释放
    session->submit_ret_submit(
            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
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

    // 同时锁两个 mutex，避免竞态条件
    std::lock(notification_mutex_, endpoint_requests_mutex_);
    std::lock_guard lock1(notification_mutex_, std::adopt_lock);
    std::lock_guard lock2(endpoint_requests_mutex_, std::adopt_lock);

    pending_notification_ = notification.to_bytes();

    // 如果队列中有中断请求，响应第一个
    auto req_opt = endpoint_requests_.dequeue_any();
    if (req_opt.has_value()) {
        auto& [ep_addr, req] = req_opt.value();

        auto* trx = GenericTransfer::from_handle(req.transfer.get());
        trx->data = std::move(pending_notification_);
        trx->actual_length = trx->data.size();
        pending_notification_.clear();
        session->submit_ret_submit(
                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
                        req.seqnum, static_cast<std::uint32_t>(trx->actual_length), std::move(req.transfer)));
    }
}

void CdcAcmCommunicationInterfaceHandler::on_disconnection(std::error_code &ec) {
    {
        std::lock_guard lock(notification_mutex_);
        pending_notification_.clear();
    }
    {
        std::lock_guard lock(endpoint_requests_mutex_);
        // TransferHandle 析构时会自动释放
        endpoint_requests_.clear();
    }
    VirtualInterfaceHandler::on_disconnection(ec);
}

void CdcAcmCommunicationInterfaceHandler::handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) {
    std::lock_guard lock(endpoint_requests_mutex_);
    endpoint_requests_.cancel_by_seqnum(unlink_seqnum);
    // 不管找没找到都返回成功
    session->submit_ret_unlink(
            UsbIpResponse::UsbIpRetUnlink::create_ret_unlink_success(cmd_seqnum));
}

// ==================== CdcAcmDataInterfaceHandler ====================

CdcAcmDataInterfaceHandler::CdcAcmDataInterfaceHandler(
        UsbInterface &handle_interface, StringPool &string_pool) :
    VirtualInterfaceHandler(handle_interface, string_pool) {
}

void CdcAcmDataInterfaceHandler::on_new_connection(Session &current_session, std::error_code &ec) {
    VirtualInterfaceHandler::on_new_connection(current_session, ec);
    std::lock_guard lock(tx_mutex_);
    disconnected_ = false;
}

void CdcAcmDataInterfaceHandler::on_disconnection(std::error_code &ec) {
    {
        std::lock_guard lock(tx_mutex_);
        disconnected_ = true;
        tx_buffer_.clear();
    }
    {
        std::lock_guard lock(endpoint_requests_mutex_);
        // TransferHandle 析构时会自动释放
        endpoint_requests_.clear();
    }
    tx_cv_.notify_all();
    VirtualInterfaceHandler::on_disconnection(ec);
}

void CdcAcmDataInterfaceHandler::handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) {
    std::lock_guard lock(endpoint_requests_mutex_);
    endpoint_requests_.cancel_by_seqnum(unlink_seqnum);
    // 不管找没找到都返回成功
    session->submit_ret_unlink(
            UsbIpResponse::UsbIpRetUnlink::create_ret_unlink_success(cmd_seqnum));
}

void CdcAcmDataInterfaceHandler::handle_bulk_transfer(
        std::uint32_t seqnum, const UsbEndpoint &ep,
        std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        TransferHandle transfer, std::error_code &ec) {

    if (ep.is_in()) {
        // Bulk IN：主机请求数据
        // 同时锁 tx_mutex_ 和 endpoint_requests_mutex_，避免竞态条件
        std::lock(tx_mutex_, endpoint_requests_mutex_);
        std::lock_guard lock1(tx_mutex_, std::adopt_lock);
        std::lock_guard lock2(endpoint_requests_mutex_, std::adopt_lock);

        if (tx_buffer_.empty() && endpoint_requests_.empty(ep.address)) {
            // 缓冲区空且没有队列中的请求，回调子类获取数据
            auto data = on_data_requested(transfer_buffer_length);
            if (!data.empty()) {
                if (data.size() <= transfer_buffer_length) {
                    auto* trx = GenericTransfer::from_handle(transfer.get());
                    trx->data = std::move(data);
                    trx->actual_length = trx->data.size();
                    session->submit_ret_submit(
                            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
                                    seqnum, static_cast<std::uint32_t>(trx->actual_length), std::move(transfer)));
                }
                else {
                    // 数据大于请求长度，移动整个data，发送部分，剩余写入缓冲区
                    auto* trx = GenericTransfer::from_handle(transfer.get());
                    trx->data = std::move(data);
                    trx->actual_length = transfer_buffer_length;

                    // 剩余数据写入缓冲区
                    tx_buffer_.write(trx->data.data() + transfer_buffer_length,
                                     trx->data.size() - transfer_buffer_length);

                    // 截断到发送长度
                    trx->data.resize(transfer_buffer_length);

                    session->submit_ret_submit(
                            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
                                    seqnum, static_cast<std::uint32_t>(trx->actual_length), std::move(transfer)));
                }
            }
            else {
                // 没有数据可发送，将请求加入队列
                endpoint_requests_.enqueue(ep.address, {seqnum, transfer_buffer_length, std::move(transfer)});
            }
        }
        else {
            // 缓冲区有数据或有队列中的请求，将请求加入队列
            endpoint_requests_.enqueue(ep.address, {seqnum, transfer_buffer_length, std::move(transfer)});
            // 尝试发送
            try_send_pending_locked();
        }
    }
    else {
        // Bulk OUT：接收主机发来的数据，直接回调子类处理
        auto* trx = GenericTransfer::from_handle(transfer.get());
        auto received_size = static_cast<std::uint32_t>(trx->data.size());
        on_data_received(std::move(trx->data));

        // transfer 析构时自动释放
        session->submit_ret_submit(
                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(seqnum, received_size));
    }
}

void CdcAcmDataInterfaceHandler::handle_non_standard_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep,
        std::uint32_t transfer_flags,
        std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet,
        TransferHandle transfer, std::error_code &ec) {

    // 数据接口通常不处理类特定控制请求
    SPDLOG_WARN("CDC ACM data interface received unexpected control request");
    // transfer 析构时自动释放
    session->submit_ret_submit(
            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
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

void CdcAcmDataInterfaceHandler::on_rts_changed(bool rts) {
    // 默认空实现，子类可重写
}

// ===== 内部函数 =====

void CdcAcmDataInterfaceHandler::send_from_tx_buffer_locked(std::uint32_t seqnum, std::uint32_t max_length, TransferHandle transfer) {
    // 调用者必须已持有 tx_mutex_ 且确保 tx_buffer_ 不为空
    // 从 TX 缓冲区读取数据发送
    std::size_t send_len = std::min(tx_buffer_.size(), static_cast<std::size_t>(max_length));
    auto* trx = GenericTransfer::from_handle(transfer.get());
    trx->data.resize(send_len);
    tx_buffer_.read(trx->data.data(), send_len);
    trx->actual_length = send_len;

    // 通知阻塞等待的发送者：缓冲区有空间了
    tx_cv_.notify_one();

    session->submit_ret_submit(
            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
                    seqnum, static_cast<std::uint32_t>(trx->actual_length), std::move(transfer)));

    // 检查低水位线
    if (tx_buffer_.size() <= tx_low_watermark_) {
        // 可在此通知子类或设置 CTS
    }
}

// ===== send_data 实现 =====

std::size_t CdcAcmDataInterfaceHandler::send_data(const std::uint8_t *data, std::size_t size) {
    // 同时锁两个 mutex
    std::lock(tx_mutex_, endpoint_requests_mutex_);
    std::lock_guard lock1(tx_mutex_, std::adopt_lock);
    std::lock_guard lock2(endpoint_requests_mutex_, std::adopt_lock);

    // 写入 TX 缓冲区（非阻塞，满时只写入可用空间）
    std::size_t written = tx_buffer_.write(data, size);

    // 尝试发送等待的数据（已持有两个 mutex）
    try_send_pending_locked();

    return written;
}

std::size_t CdcAcmDataInterfaceHandler::send_data(const data_type &data) {
    return send_data(data.data(), data.size());
}

std::size_t CdcAcmDataInterfaceHandler::send_data(data_type &&data) {
    return send_data(data.data(), data.size());
}

std::size_t CdcAcmDataInterfaceHandler::send_data(std::string_view data) {
    return send_data(reinterpret_cast<const std::uint8_t *>(data.data()), data.size());
}

std::size_t CdcAcmDataInterfaceHandler::send_data_blocking(const std::uint8_t *data, std::size_t size,
                                                           std::uint32_t timeout_ms) {
    std::size_t total_written = 0;
    std::size_t offset = 0;

    while (offset < size) {
        // 阶段1：等待缓冲区有空间
        {
            std::unique_lock tx_lock(tx_mutex_);

            // 检查是否已断开连接
            if (disconnected_) {
                return total_written;
            }

            // 如果缓冲区满，先尝试发送
            if (tx_buffer_.available() == 0) {
                // 锁队列并发送
                {
                    std::lock_guard queue_lock(endpoint_requests_mutex_);
                    try_send_pending_locked();
                }

                // 再次检查断开连接
                if (disconnected_) {
                    return total_written;
                }

                // 如果仍然满，等待条件变量
                while (tx_buffer_.available() == 0 && !disconnected_) {
                    if (timeout_ms == 0) {
                        // 无限等待
                        tx_cv_.wait(tx_lock);
                    }
                    else {
                        // 带超时等待
                        auto result = tx_cv_.wait_for(tx_lock, std::chrono::milliseconds(timeout_ms));
                        if (result == std::cv_status::timeout) {
                            // 超时，返回已写入量
                            return total_written;
                        }
                    }
                }

                // 被唤醒后检查是否断开
                if (disconnected_) {
                    return total_written;
                }
            }

            // 写入数据
            std::size_t written = tx_buffer_.write(data + offset, size - offset);
            total_written += written;
            offset += written;
        }

        // 阶段2：尝试发送（锁两个 mutex）
        {
            std::lock(tx_mutex_, endpoint_requests_mutex_);
            std::lock_guard lock1(tx_mutex_, std::adopt_lock);
            std::lock_guard lock2(endpoint_requests_mutex_, std::adopt_lock);
            try_send_pending_locked();
        }
    }

    return total_written;
}

std::size_t CdcAcmDataInterfaceHandler::send_data_blocking(const data_type &data, std::uint32_t timeout_ms) {
    return send_data_blocking(data.data(), data.size(), timeout_ms);
}

std::size_t CdcAcmDataInterfaceHandler::send_data_blocking(data_type &&data, std::uint32_t timeout_ms) {
    return send_data_blocking(data.data(), data.size(), timeout_ms);
}

std::size_t CdcAcmDataInterfaceHandler::send_data_blocking(std::string_view data, std::uint32_t timeout_ms) {
    return send_data_blocking(reinterpret_cast<const std::uint8_t *>(data.data()), data.size(), timeout_ms);
}

// ===== 缓冲区配置 =====

void CdcAcmDataInterfaceHandler::set_tx_buffer_capacity(std::size_t capacity) {
    std::lock_guard lock(tx_mutex_);
    tx_buffer_.resize(capacity);
    tx_high_watermark_ = capacity * 3 / 4;
    tx_low_watermark_ = capacity / 4;
}

void CdcAcmDataInterfaceHandler::set_tx_watermarks(std::size_t high, std::size_t low) {
    tx_high_watermark_ = high;
    tx_low_watermark_ = low;
}

std::size_t CdcAcmDataInterfaceHandler::get_tx_buffer_size() const {
    std::lock_guard lock(tx_mutex_);
    return tx_buffer_.size();
}

std::size_t CdcAcmDataInterfaceHandler::get_tx_buffer_available() const {
    std::lock_guard lock(tx_mutex_);
    return tx_buffer_.available();
}

// ===== 流控状态 =====

void CdcAcmDataInterfaceHandler::set_cts(bool cts) {
    if (comm_handler_) {
        std::uint16_t state = cts ? static_cast<std::uint16_t>(CdcAcmSerialState::CTS) : 0;
        comm_handler_->send_serial_state_notification(state);
    }
}

bool CdcAcmDataInterfaceHandler::get_rts() const {
    if (comm_handler_) {
        return comm_handler_->get_control_signal_state().rts;
    }
    return true; // 默认允许发送
}

void CdcAcmDataInterfaceHandler::set_comm_handler(CdcAcmCommunicationInterfaceHandler *handler) {
    comm_handler_ = handler;
}

void CdcAcmDataInterfaceHandler::try_send_pending_locked() {
    // 调用者必须已持有 tx_mutex_ 和 endpoint_requests_mutex_

    // 检查是否有队列中的请求且有数据可发
    while (!tx_buffer_.empty()) {
        auto req_opt = endpoint_requests_.dequeue_any();
        if (!req_opt.has_value()) {
            break;
        }

        auto& [ep_addr, req] = req_opt.value();
        // 从缓冲区读取并发送
        send_from_tx_buffer_locked(req.seqnum, req.length, std::move(req.transfer));
    }
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
