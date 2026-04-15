#include "virtual_device/CdcAcmVirtualInterfaceHandler.h"

#include "Session.h"
#include <algorithm>

namespace usbipdcpp {

// ==================== RingBuffer ====================

RingBuffer::RingBuffer(std::size_t capacity)
    : capacity_(capacity) {
    buffer_.resize(capacity_);
}

std::size_t RingBuffer::write(const std::uint8_t *data, std::size_t size) {
    std::size_t available = capacity_ - count_;
    std::size_t to_write = std::min(size, available);

    for (std::size_t i = 0; i < to_write; ++i) {
        buffer_[tail_] = data[i];
        tail_ = (tail_ + 1) % capacity_;
        ++count_;
    }

    return to_write;
}

std::size_t RingBuffer::read(std::uint8_t *data, std::size_t max_size) {
    std::size_t to_read = std::min(max_size, count_);

    for (std::size_t i = 0; i < to_read; ++i) {
        data[i] = buffer_[head_];
        head_ = (head_ + 1) % capacity_;
        --count_;
    }

    return to_read;
}

std::size_t RingBuffer::peek(std::uint8_t *data, std::size_t max_size) const {
    std::size_t to_peek = std::min(max_size, count_);
    std::size_t pos = head_;

    for (std::size_t i = 0; i < to_peek; ++i) {
        data[i] = buffer_[pos];
        pos = (pos + 1) % capacity_;
    }

    return to_peek;
}

std::size_t RingBuffer::size() const {
    return count_;
}

std::size_t RingBuffer::capacity() const {
    return capacity_;
}

std::size_t RingBuffer::available() const {
    return capacity_ - count_;
}

bool RingBuffer::empty() const {
    return count_ == 0;
}

bool RingBuffer::full() const {
    return count_ == capacity_;
}

void RingBuffer::clear() {
    head_ = 0;
    tail_ = 0;
    count_ = 0;
}

void RingBuffer::resize(std::size_t new_capacity) {
    // 读取现有数据
    std::vector<std::uint8_t> old_data(count_);
    read(old_data.data(), count_);

    // 重新分配
    buffer_.resize(new_capacity);
    capacity_ = new_capacity;
    head_ = 0;
    tail_ = 0;
    count_ = 0;

    // 写回数据
    write(old_data.data(), old_data.size());
}

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

void CdcAcmCommunicationInterfaceHandler::on_disconnection(std::error_code &ec) {
    {
        std::lock_guard lock(notification_mutex_);
        pending_notification_.clear();
    }
    {
        std::lock_guard lock(interrupt_req_queue_mutex_);
        interrupt_req_queue_.clear();
    }
    VirtualInterfaceHandler::on_disconnection(ec);
}

// ==================== CdcAcmDataInterfaceHandler ====================

CdcAcmDataInterfaceHandler::CdcAcmDataInterfaceHandler(
    UsbInterface &handle_interface, StringPool &string_pool) :
    VirtualInterfaceHandler(handle_interface, string_pool),
    tx_buffer_(64 * 1024) {
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
        bulk_in_req_queue_.clear();
    }
    tx_cv_.notify_all();
    VirtualInterfaceHandler::on_disconnection(ec);
}

void CdcAcmDataInterfaceHandler::handle_bulk_transfer(
        std::uint32_t seqnum, const UsbEndpoint &ep,
        std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        data_type &&out_data, std::error_code &ec) {

    if (ep.is_in()) {
        // Bulk IN：主机请求数据
        // tx_mutex_ 保护 tx_buffer_ 和 bulk_in_req_queue_
        std::lock_guard lock(tx_mutex_);

        if (send_from_tx_buffer_locked(seqnum, transfer_buffer_length)) {
            // 已从缓冲区发送数据
        }
        else {
            // 缓冲区空，回调子类获取数据
            auto data = on_data_requested(transfer_buffer_length);
            if (!data.empty()) {
                if (data.size() <= transfer_buffer_length) {
                    session->submit_ret_submit(
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(seqnum, std::move(data)));
                }
                else {
                    // 数据大于请求长度，发送部分，剩余写入缓冲区
                    data_type to_send(data.begin(), data.begin() + transfer_buffer_length);
                    session->submit_ret_submit(
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(seqnum, std::move(to_send)));

                    // 剩余数据写入缓冲区
                    tx_buffer_.write(data.data() + transfer_buffer_length,
                                    data.size() - transfer_buffer_length);
                }
            }
            else {
                // 没有数据可发送，加入等待队列
                bulk_in_req_queue_.push_back({seqnum, transfer_buffer_length});
            }
        }
    }
    else {
        // Bulk OUT：接收主机发来的数据，直接回调子类处理
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

void CdcAcmDataInterfaceHandler::on_rts_changed(bool rts) {
    // 默认空实现，子类可重写
}

// ===== 内部函数 =====

bool CdcAcmDataInterfaceHandler::send_from_tx_buffer_locked(std::uint32_t seqnum, std::uint32_t max_length) {
    // 调用者必须已持有 tx_mutex_
    if (tx_buffer_.empty()) {
        return false;
    }

    // 从 TX 缓冲区读取数据发送
    std::size_t send_len = std::min(tx_buffer_.size(), static_cast<std::size_t>(max_length));
    data_type data(send_len);
    tx_buffer_.read(data.data(), send_len);

    // 通知阻塞等待的发送者：缓冲区有空间了
    tx_cv_.notify_one();

    session->submit_ret_submit(
        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(seqnum, std::move(data)));

    // 检查低水位线
    if (tx_buffer_.size() <= tx_low_watermark_) {
        // 可在此通知子类或设置 CTS
    }

    return true;
}

// ===== send_data 实现 =====

std::size_t CdcAcmDataInterfaceHandler::send_data(const std::uint8_t *data, std::size_t size) {
    std::lock_guard lock(tx_mutex_);

    // 写入 TX 缓冲区（非阻塞，满时只写入可用空间）
    std::size_t written = tx_buffer_.write(data, size);

    // 尝试发送等待的数据（已持有 tx_mutex_）
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
    return send_data(reinterpret_cast<const std::uint8_t*>(data.data()), data.size());
}

std::size_t CdcAcmDataInterfaceHandler::send_data_blocking(const std::uint8_t *data, std::size_t size,
                                                          std::uint32_t timeout_ms) {
    std::unique_lock lock(tx_mutex_);
    std::size_t total_written = 0;
    std::size_t offset = 0;

    while (offset < size) {
        // 检查是否已断开连接
        if (disconnected_) {
            return total_written;
        }

        // 等待缓冲区有空间
        if (tx_buffer_.available() == 0) {
            // 没有空间，尝试触发发送
            try_send_pending_locked();

            // 再次检查断开连接
            if (disconnected_) {
                return total_written;
            }

            // 仍然没有空间，等待条件变量
            if (tx_buffer_.available() == 0) {
                if (timeout_ms == 0) {
                    // 无限等待
                    tx_cv_.wait(lock, [this]() {
                        return tx_buffer_.available() > 0 || disconnected_;
                    });
                }
                else {
                    // 带超时等待
                    auto result = tx_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                                  [this]() {
                                                      return tx_buffer_.available() > 0 || disconnected_;
                                                  });
                    if (!result) {
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

        // 尝试发送
        try_send_pending_locked();
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
    return send_data_blocking(reinterpret_cast<const std::uint8_t*>(data.data()), data.size(), timeout_ms);
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
        std::uint16_t state = cts ?
            static_cast<std::uint16_t>(CdcAcmSerialState::CTS) : 0;
        comm_handler_->send_serial_state_notification(state);
    }
}

bool CdcAcmDataInterfaceHandler::get_rts() const {
    if (comm_handler_) {
        return comm_handler_->get_control_signal_state().rts;
    }
    return true;  // 默认允许发送
}

void CdcAcmDataInterfaceHandler::set_comm_handler(CdcAcmCommunicationInterfaceHandler *handler) {
    comm_handler_ = handler;
}

void CdcAcmDataInterfaceHandler::try_send_pending_locked() {
    // 调用者必须已持有 tx_mutex_
    // 此函数操作 tx_buffer_ 和 bulk_in_req_queue_

    // 检查是否有等待的请求
    if (bulk_in_req_queue_.empty() || tx_buffer_.empty()) {
        return;
    }

    // 从队列取出请求
    BulkInRequest request = bulk_in_req_queue_.front();
    bulk_in_req_queue_.pop_front();

    // 从缓冲区读取并发送
    send_from_tx_buffer_locked(request.seqnum, request.length);
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
