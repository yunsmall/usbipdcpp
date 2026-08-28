#include "usbipdcpp/virtual_device/HidVirtualInterfaceHandler.h"

#include "usbipdcpp/Session.h"
#include "usbipdcpp/constant.h"
#include "usbipdcpp/protocol.h"

// ========== 中断传输处理 ==========

void usbipdcpp::HidVirtualInterfaceHandler::handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                                      std::uint32_t transfer_flags,
                                                                      std::uint32_t transfer_buffer_length,
                                                                      TransferHandle transfer, std::error_code &ec) {
    if (ep.is_in()) {
        // 中断 IN：主机请求输入报告
        // 先让子类现场生成报告（pull 模型），子类可调用 send_input_report()
        // 将数据推入通道，后续走正常 push 流程。
        // 必须在锁外调用：子类实现里可能调用 send_input_report()
        on_input_report_requested(transfer_buffer_length);

        // 通道内部处理：缓冲有报告立即应答，否则挂起请求等待 send_input_report()
        input_channel.on_in_request(ep.address, seqnum, transfer_buffer_length, std::move(transfer));
    }
    else {
        // 中断 OUT：主机发送输出报告
        auto *trx = GenericTransfer::from_handle(transfer.get());
        auto received_size = static_cast<std::uint32_t>(trx->data.size());
        on_output_report_received(asio::buffer(trx->data));

        // transfer 析构时自动释放
        session->submit_ret_submit(
                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(seqnum, received_size));
    }
}

// ========== 发送输入报告 ==========

void usbipdcpp::HidVirtualInterfaceHandler::send_input_report(asio::const_buffer data) {
    // 推入通道：有挂起请求直接应答，否则入缓冲等待（超上限丢最旧，见
    // MAX_PENDING_INPUT_REPORTS）
    input_channel.push(data_type(static_cast<const std::uint8_t *>(data.data()),
                                 static_cast<const std::uint8_t *>(data.data()) + data.size()));
}

// ========== 回调默认实现 ==========

void usbipdcpp::HidVirtualInterfaceHandler::on_input_report_requested(std::uint16_t length) {
    // 默认空实现，子类可重写
}

void usbipdcpp::HidVirtualInterfaceHandler::on_output_report_received(asio::const_buffer data) {
    // 默认空实现，子类可重写
}

// ========== 连接生命周期 ==========

void usbipdcpp::HidVirtualInterfaceHandler::on_new_connection(Session &current_session, error_code &ec) {
    // 父类先设 session 指针（通道应答请求要用），再绑定通道并重置断连状态
    VirtualInterfaceHandler::on_new_connection(current_session, ec);
    input_channel.bind_session(&current_session);
    input_channel.on_new_connection();
}

void usbipdcpp::HidVirtualInterfaceHandler::on_disconnection(std::error_code &ec) {
    // 先清通道（缓冲 + 挂起请求，TransferHandle 析构时自动释放），再调父类清 session
    input_channel.on_disconnection();
    VirtualInterfaceHandler::on_disconnection(ec);
}

// ========== UNLINK 处理 ==========

void usbipdcpp::HidVirtualInterfaceHandler::handle_unlink_seqnum(std::uint32_t unlink_seqnum,
                                                                 std::uint32_t cmd_seqnum) {
    bool cancelled = input_channel.cancel_pending(unlink_seqnum);
    // 从队列中真的取消了待处理 URB → 回 -ECONNRESET（URB 被取消，且不再发
    // RET_SUBMIT，请求已从队列移除）；找不到（URB 已完成/不存在）→ 回 0。
    // 与内核 stub_tx.c（priv->unlinking 时 RET_UNLINK 带 urb->status=-ECONNRESET，
    // 否则 0）及 usbipd-libusb 一致，也是本项目 LibusbDeviceHandler 的 unlink
    // 范本（trxstat2error(CANCELLED)=-ECONNRESET、找不到回 0）
    session->submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(
            cmd_seqnum, cancelled ? static_cast<std::uint32_t>(UrbStatusType::StatusECONNRESET) : 0));
}

// ========== 控制请求处理 ==========

void usbipdcpp::HidVirtualInterfaceHandler::handle_non_standard_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet, TransferHandle transfer, std::error_code &ec) {
    auto *trx = GenericTransfer::from_handle(transfer.get());
    auto type = static_cast<RequestType>(setup_packet.calc_request_type());
    switch (type) {
        case RequestType::Class: {
            auto request = static_cast<HIDRequest>(setup_packet.request);
            std::uint32_t status = static_cast<std::uint32_t>(UrbStatusType::StatusOK);
            if (!setup_packet.is_out()) {
                data_type result;
                switch (request) {
                    case HIDRequest::GetIdle: {
                        result = request_get_idle(setup_packet.value >> 8, setup_packet.value, setup_packet.length,
                                                  &status);
                        if (setup_packet.length < result.size()) {
                            result.resize(setup_packet.length);
                        }
                        break;
                    }
                    case HIDRequest::GetProtocol: {
                        auto ret = request_get_protocol(&status);
                        // 控制传输数据阶段统一小端（GetProtocol 返回 1 字节，
                        // 大小端无区别，与 GetStatus 等保持一致）
                        vector_append_to_le(result, ret);
                        break;
                    }
                    case HIDRequest::GetReport: {
                        result = request_get_report(setup_packet.value >> 8, setup_packet.value, setup_packet.length,
                                                    &status);
                        if (setup_packet.length < result.size()) {
                            result.resize(setup_packet.length);
                        }
                        break;
                    }
                    default: {
                        SPDLOG_ERROR("Unknown HID request 0x{:x}", setup_packet.request);
                        status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
                    }
                }
                // 将数据写入 transfer_handle
                trx->data = std::move(result);
                trx->actual_length = trx->data.size();
                trx->data_offset = 0;

                session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
                        seqnum, static_cast<std::uint32_t>(trx->actual_length), std::move(transfer)));
            }
            else {
                // 从 transfer_handle 获取 OUT 数据
                data_type out_data(trx->data.begin(), trx->data.begin() + transfer_buffer_length);
                switch (request) {
                    case HIDRequest::SetIdle: {
                        request_set_idle(setup_packet.value >> 8, &status);
                        break;
                    }
                    case HIDRequest::SetProtocol: {
                        request_set_protocol(setup_packet.value, &status);
                        break;
                    }
                    case HIDRequest::SetReport: {
                        request_set_report(setup_packet.value >> 8, setup_packet.value, setup_packet.length, out_data,
                                           &status);
                        break;
                    }
                    default: {
                        SPDLOG_ERROR("Unknown HID request 0x{:x}", setup_packet.request);
                        status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
                    }
                }
                // transfer 析构时自动释放
                session->submit_ret_submit(
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(seqnum, status, 0));
            }
            break;
        }
        default: {
            handle_non_hid_request_type_control_urb(seqnum, ep, transfer_flags, transfer_buffer_length, setup_packet,
                                                    std::move(transfer), ec);
        }
    }
}

usbipdcpp::data_type usbipdcpp::HidVirtualInterfaceHandler::request_get_descriptor(std::uint8_t type,
                                                                                   std::uint8_t language_id,
                                                                                   std::uint16_t descriptor_length,
                                                                                   std::uint32_t *p_status) {
    auto hid_type = static_cast<HidDescriptorType>(type);
    switch (hid_type) {
        case HidDescriptorType::Report: {
            return get_report_descriptor();
        }
        default: {
            SPDLOG_ERROR("Unimplement descriptor type: {:x}", static_cast<std::uint32_t>(hid_type));
            *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
            return {};
        }
    }
}

usbipdcpp::data_type usbipdcpp::HidVirtualInterfaceHandler::get_class_specific_descriptor() {
    auto report_descriptor_size = get_report_descriptor_size();
    return {
            0x09, // bLength
            HidDescriptorType::Hid, // bDescriptorType: HID
            0x11,
            0x01, // bcdHID 1.11
            0x00, // bCountryCode
            0x01, // bNumDescriptors
            HidDescriptorType::Report, // bDescriptorType[0] HID
            static_cast<std::uint8_t>(report_descriptor_size),
            static_cast<std::uint8_t>(report_descriptor_size >> 8), // wDescriptorLength[0]
    };
}

// ========== 非HID请求默认实现 ==========

void usbipdcpp::HidVirtualInterfaceHandler::handle_non_hid_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet, TransferHandle transfer, std::error_code &ec) {
    // transfer 析构时自动释放
    session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
}

// ========== 报告请求默认实现 ==========

usbipdcpp::data_type usbipdcpp::HidVirtualInterfaceHandler::request_get_report(std::uint8_t type,
                                                                               std::uint8_t report_id,
                                                                               std::uint16_t length,
                                                                               std::uint32_t *p_status) {
    // 形式响应（对齐内核 f_hid.c hidg_setup：GET_REPORT 返回全 0 空报告，
    // 长度由主机 wLength 决定）。真实报告数据走中断 IN 端点，控制传输
    // GET_REPORT 只是兜底应答；子类需要真实数据时重写
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusOK);
    return data_type(length, 0);
}

void usbipdcpp::HidVirtualInterfaceHandler::request_set_report(std::uint8_t type, std::uint8_t report_id,
                                                               std::uint16_t length, const data_type &data,
                                                               std::uint32_t *p_status) {
    // 对齐内核 f_hid.c ssreport 模式（无 OUT 端点时 SET_REPORT 经 ep0 接收，
    // 数据落 set_report_buf 供用户态读取）。本项目默认没有消费者，接受并
    // 丢弃；子类需要输出报告（如键盘 LED）时重写本函数
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusOK);
}
