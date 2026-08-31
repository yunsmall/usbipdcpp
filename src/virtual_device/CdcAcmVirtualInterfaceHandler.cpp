#include "usbipdcpp/virtual_device/CdcAcmVirtualInterfaceHandler.h"

#include "usbipdcpp/DeviceHandler/DeviceHandler.h"

#include <algorithm>
#include "usbipdcpp/Session.h"

namespace usbipdcpp {
// ==================== CdcAcmCommunicationInterfaceHandler ====================

CdcAcmCommunicationInterfaceHandler::CdcAcmCommunicationInterfaceHandler(UsbInterface &handle_interface,
                                                                         StringPool &string_pool) :
    VirtualInterfaceHandler(handle_interface, string_pool) {
    // 通知通道待发缓存上限 30 条：串口状态通知低频（状态变化才发一条），
    // 正常使用缓存几乎不会超过个位数；30 条是防主机长期不读时内存堆积的
    // 兜底，同时保留足够余量不丢低频通知（超限丢最旧）
    notification_channel.set_max_pending_messages(30);
}

void CdcAcmCommunicationInterfaceHandler::handle_non_standard_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet, TransferHandle transfer, std::error_code &ec) {
    auto type = static_cast<RequestType>(setup_packet.calc_request_type());
    std::uint32_t status = static_cast<std::uint32_t>(UrbStatusType::StatusOK);

    if (type == RequestType::Class) {
        auto request = static_cast<CdcAcmRequest>(setup_packet.request);

        if (!setup_packet.is_out()) {
            // IN 请求
            auto *trx = GenericTransfer::from_handle(transfer.get());
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
            session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                    seqnum, status, static_cast<std::uint32_t>(trx->actual_length), std::move(transfer)));
        }
        else {
            // OUT 请求
            auto *trx = GenericTransfer::from_handle(transfer.get());
            auto &out_data = trx->data;
            switch (request) {
                case CdcAcmRequest::SetLineCoding: {
                    auto new_coding = LineCoding::from_bytes(out_data);
                    on_set_line_coding(new_coding);
                    line_coding_ = new_coding;
                    SPDLOG_DEBUG("SET_LINE_CODING: baud={}, data_bits={}, stop_bits={}, parity={}",
                                 line_coding_.dwDTERate, line_coding_.bDataBits, line_coding_.bCharFormat,
                                 line_coding_.bParityType);
                    break;
                }
                case CdcAcmRequest::SetControlLineState: {
                    auto state = ControlSignalState::from_uint16(setup_packet.value);
                    on_set_control_line_state(state);
                    control_signal_state_ = state;
                    SPDLOG_DEBUG("SET_CONTROL_LINE_STATE: DTR={}, RTS={}", state.dtr, state.rts);
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
                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(seqnum, status, 0));
        }
    }
    else {
        // 非 CDC 类请求，交给子类处理
        handle_non_cdc_request_type_control_urb(seqnum, ep, transfer_flags, transfer_buffer_length, setup_packet,
                                                std::move(transfer), ec);
    }
}

void CdcAcmCommunicationInterfaceHandler::handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                                    std::uint32_t transfer_flags,
                                                                    std::uint32_t transfer_buffer_length,
                                                                    TransferHandle transfer, std::error_code &ec) {
    if (ep.is_in()) {
        // 通道内部处理：缓冲有通知立即应答，否则挂起请求等待
        // send_serial_state_notification() 推入
        notification_channel.on_in_request(ep.address, seqnum, transfer_buffer_length, std::move(transfer));
    }
    else {
        // 中断 OUT：CDC ACM 通常不使用
        // transfer 析构时自动释放
        SPDLOG_WARN("CDC ACM communication interface received unexpected interrupt OUT");
        session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
    }
}

data_type CdcAcmCommunicationInterfaceHandler::get_class_specific_descriptor() {
    // CDC ACM 类特定描述符
    data_type descriptor;

    // Header Functional Descriptor
    CdcHeaderFunctionalDesc{0x05, CS_INTERFACE, 0x00, 0x0110 /* bcdCDC: 1.10 */}.append_to(descriptor);

    // Call Management Functional Descriptor
    CdcCallManagementDesc{0x05, CS_INTERFACE, 0x01,
                          0x00, // bmCapabilities
                          0x01} // bDataInterface: Interface 1
            .append_to(descriptor);

    // ACM Functional Descriptor
    CdcAcmFunctionalDesc{0x04, CS_INTERFACE, 0x02,
                         0x02} // bmCapabilities: support Set_Line_Coding, Set_Control_Line_State, Send_Break
            .append_to(descriptor);

    // Union Functional Descriptor
    CdcUnionFunctionalDesc{0x05, CS_INTERFACE, 0x06,
                           0x00, // bMasterInterface: Interface 0
                           0x01} // bSlaveInterface0: Interface 1
            .append_to(descriptor);

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
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet, TransferHandle transfer, std::error_code &ec) {
    // 默认返回错误，子类可重写以处理非 CDC 请求
    SPDLOG_WARN("Unhandled request type 0x{:x} in CDC ACM communication interface", setup_packet.calc_request_type());
    // transfer 析构时自动释放
    session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
}

void CdcAcmCommunicationInterfaceHandler::send_serial_state_notification(std::uint16_t state_bits) {
    SerialStateNotification notification;
    notification.data = state_bits;

    // 通道内部处理：有挂起请求直接应答，否则入缓冲（上限 1 条：只保留最新）
    notification_channel.push(notification.to_bytes());
}

void CdcAcmCommunicationInterfaceHandler::on_new_connection(Session &current_session, std::error_code &ec) {
    // 父类先设 session 指针（通道应答请求要用），再绑定通道并重置断连状态
    VirtualInterfaceHandler::on_new_connection(current_session, ec);
    notification_channel.on_new_connection(&current_session);
}

void CdcAcmCommunicationInterfaceHandler::on_disconnection(std::error_code &ec) {
    // 先清通道（缓冲 + 挂起请求，TransferHandle 析构自动释放），再调父类清 session
    notification_channel.on_disconnection();
    VirtualInterfaceHandler::on_disconnection(ec);
}

void CdcAcmCommunicationInterfaceHandler::handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) {
    bool cancelled = notification_channel.cancel_pending(unlink_seqnum);
    // 从队列中真的取消了待处理 URB → 回 -ECONNRESET（URB 被取消，且不再发
    // RET_SUBMIT，请求已从队列移除）；找不到（URB 已完成/不存在）→ 回 0。
    // 与内核 stub_tx.c 及本项目 LibusbDeviceHandler 的 unlink 范本一致
    session->submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(
            cmd_seqnum, cancelled ? static_cast<std::uint32_t>(UrbStatusType::StatusECONNRESET) : 0));
}

// ==================== CdcAcmDataInterfaceHandler ====================

CdcAcmDataInterfaceHandler::CdcAcmDataInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool) :
    VirtualInterfaceHandler(handle_interface, string_pool) {
    // pull 回调：主机请求数据且缓冲/队列都空时调用子类的 on_data_requested
    // 现场生成数据。通道在锁内调用（与原来 handle_bulk_transfer 的双锁内
    // 调用语义一致），故回调里不能调用 send_data 等函数（会死锁）
    in_channel.set_pull_callback([this](std::uint32_t length) {
        return on_data_requested(static_cast<std::uint16_t>(length));
    });
}

void CdcAcmDataInterfaceHandler::on_new_connection(Session &current_session, std::error_code &ec) {
    // 父类先设 session 指针（通道应答请求要用），再绑定通道并重置断连状态
    VirtualInterfaceHandler::on_new_connection(current_session, ec);
    in_channel.on_new_connection(&current_session);
    out_channel.on_new_connection(&current_session);
}

void CdcAcmDataInterfaceHandler::on_disconnection(std::error_code &ec) {
    // 先清通道（缓冲 + 挂起请求，TransferHandle 析构时自动释放；唤醒阻塞
    // 的写者/取者让它们按断连返回），再调父类清 session
    out_channel.on_disconnection();
    in_channel.on_disconnection();
    VirtualInterfaceHandler::on_disconnection(ec);
}

void CdcAcmDataInterfaceHandler::handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) {
    // 挂起请求分散在 IN/OUT 两通道里，逐个尝试取消
    bool cancelled = in_channel.cancel_pending(unlink_seqnum);
    if (!cancelled) {
        cancelled = out_channel.cancel_pending(unlink_seqnum);
    }
    // 从队列中真的取消了待处理 URB → 回 -ECONNRESET（URB 被取消，且不再发
    // RET_SUBMIT，请求已从队列移除）；找不到（URB 已完成/不存在）→ 回 0。
    // 与内核 stub_tx.c 及本项目 LibusbDeviceHandler 的 unlink 范本一致
    session->submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(
            cmd_seqnum, cancelled ? static_cast<std::uint32_t>(UrbStatusType::StatusECONNRESET) : 0));
}

void CdcAcmDataInterfaceHandler::handle_bulk_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                      std::uint32_t transfer_flags,
                                                      std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                                      std::error_code &ec) {
    if (ep.is_in()) {
        // Bulk IN：主机请求数据。通道内部处理：缓冲有数据立即应答，否则先
        // pull（on_data_requested 现场生成），再不行挂起请求等待 send_data
        in_channel.on_in_request(ep.address, seqnum, transfer_buffer_length, std::move(transfer));
    }
    else {
        // Bulk OUT：先给子类当场消费机会（如把数据生成响应塞给 IN 方向）。
        // true=已处理，立即应答；false=未处理，请求挂起入通道（主机 NAK 背压），
        // 子类之后用 take_out() 取出数据并应答。false 时子类必须保证 data 未被
        // 移动（挂起的请求里数据仍在）
        auto *trx = GenericTransfer::from_handle(transfer.get());
        auto received_size = static_cast<std::uint32_t>(trx->data.size());
        if (on_data_received(std::move(trx->data))) {
            session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(
                    seqnum, received_size));
        }
        else {
            out_channel.on_out_request(ep.address, seqnum, std::move(transfer));
        }
    }
}

void CdcAcmDataInterfaceHandler::handle_non_standard_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet, TransferHandle transfer, std::error_code &ec) {
    // 数据接口通常不处理类特定控制请求
    SPDLOG_WARN("CDC ACM data interface received unexpected control request");
    // transfer 析构时自动释放
    session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
}

data_type CdcAcmDataInterfaceHandler::get_class_specific_descriptor() {
    // 数据接口没有类特定描述符
    return {};
}

bool CdcAcmDataInterfaceHandler::on_data_received(data_type &&data) {
    // 默认行为：总是消费并立即应答（未 override 的子类保持原"总是接收"语义）
    return true;
}

data_type CdcAcmDataInterfaceHandler::on_data_requested(std::uint16_t length) {
    // 默认返回空，子类可重写
    return {};
}

void CdcAcmDataInterfaceHandler::on_rts_changed(bool rts) {
    // 默认空实现，子类可重写
}

// ===== send_data 实现 =====

std::size_t CdcAcmDataInterfaceHandler::send_data(const std::uint8_t *data, std::size_t size) {
    // 非阻塞写入通道（满时只写入可用空间，断连返回 0），内部会应答挂起请求
    return in_channel.write_nb(data, size);
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
    // 阻塞写入通道：缓冲满时等待宿主取走（timeout_ms=0 无限等），断连返回已写入量
    return in_channel.write(data, size, timeout_ms);
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

std::optional<OutEndpointChannel::Pending> CdcAcmDataInterfaceHandler::take_out(std::uint32_t timeout_ms) {
    return out_channel.take(timeout_ms);
}

std::optional<OutEndpointChannel::Pending> CdcAcmDataInterfaceHandler::try_take_out() {
    return out_channel.try_take();
}

// ===== 缓冲区配置 =====

void CdcAcmDataInterfaceHandler::set_tx_buffer_capacity(std::size_t capacity) {
    // 通道内部锁保护缓冲；水位线随容量联动（默认 3/4 与 1/4）
    in_channel.set_capacity(capacity);
    in_high_watermark_ = capacity * 3 / 4;
    in_low_watermark_ = capacity / 4;
}

void CdcAcmDataInterfaceHandler::set_tx_watermarks(std::size_t high, std::size_t low) {
    in_high_watermark_ = high;
    in_low_watermark_ = low;
}

std::size_t CdcAcmDataInterfaceHandler::get_tx_buffer_size() const {
    return in_channel.size();
}

std::size_t CdcAcmDataInterfaceHandler::get_tx_buffer_available() const {
    return in_channel.available();
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
} // namespace usbipdcpp
