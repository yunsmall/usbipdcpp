#include "virtual_device/HidVirtualInterfaceHandler.h"

#include <algorithm>

#include "constant.h"
#include "Session.h"
#include "protocol.h"

// ========== 中断传输处理 ==========

void usbipdcpp::HidVirtualInterfaceHandler::handle_interrupt_transfer(
        std::uint32_t seqnum, const UsbEndpoint &ep,
        std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        TransferHandle transfer, std::error_code &ec) {

    if (ep.is_in()) {
        // 中断 IN：主机请求输入报告
        std::lock_guard lock(interrupt_mutex_);

        // 尝试从子类获取数据
        auto data = on_input_report_requested(static_cast<std::uint16_t>(transfer_buffer_length));

        if (!data.empty()) {
            // 有数据，立即响应
            auto* trx = GenericTransfer::from_handle(transfer.get());
            trx->data = std::move(data);
            trx->actual_length = std::min(trx->data.size(), static_cast<std::size_t>(transfer_buffer_length));
            trx->data.resize(trx->actual_length);
            session->submit_ret_submit(
                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
                            seqnum, static_cast<std::uint32_t>(trx->actual_length), std::move(transfer)));
        }
        else {
            // 没有数据，挂起请求
            if (pending_interrupt_request_.has_value()) {
                // USB协议错误：同一端点已有挂起请求
                SPDLOG_WARN("Interrupt IN request while another is pending, returning EPIPE");
                session->submit_ret_submit(
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
            }
            else {
                pending_interrupt_request_.emplace(IntRequest{seqnum, transfer_buffer_length, std::move(transfer)});
            }
        }
    }
    else {
        // 中断 OUT：主机发送输出报告
        auto* trx = GenericTransfer::from_handle(transfer.get());
        auto received_size = static_cast<std::uint32_t>(trx->data.size());
        on_output_report_received(asio::buffer(trx->data));

        // transfer 析构时自动释放
        session->submit_ret_submit(
                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(seqnum, received_size));
    }
}

// ========== 发送输入报告 ==========

bool usbipdcpp::HidVirtualInterfaceHandler::send_input_report(asio::const_buffer data) {
    std::lock_guard lock(interrupt_mutex_);

    if (pending_interrupt_request_.has_value()) {
        // 有挂起请求，立即响应
        auto& req = *pending_interrupt_request_;

        auto* trx = GenericTransfer::from_handle(req.transfer.get());
        auto send_len = std::min(data.size(), static_cast<std::size_t>(req.length));
        trx->data.assign(static_cast<const std::uint8_t*>(data.data()),
                         static_cast<const std::uint8_t*>(data.data()) + send_len);
        trx->actual_length = send_len;

        session->submit_ret_submit(
                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
                        req.seqnum, static_cast<std::uint32_t>(send_len), std::move(req.transfer)));

        pending_interrupt_request_.reset();
        return true;
    }
    return false;
}

// ========== 回调默认实现 ==========

usbipdcpp::data_type usbipdcpp::HidVirtualInterfaceHandler::on_input_report_requested(std::uint16_t length) {
    // 默认返回空，子类可重写
    return {};
}

void usbipdcpp::HidVirtualInterfaceHandler::on_output_report_received(asio::const_buffer data) {
    // 默认空实现，子类可重写
}

// ========== 连接生命周期 ==========

void usbipdcpp::HidVirtualInterfaceHandler::on_disconnection(std::error_code &ec) {
    {
        std::lock_guard lock(interrupt_mutex_);
        // TransferHandle 析构时会自动释放
        pending_interrupt_request_.reset();
    }
    VirtualInterfaceHandler::on_disconnection(ec);
}

// ========== 控制请求处理 ==========

void usbipdcpp::HidVirtualInterfaceHandler::handle_non_standard_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet, TransferHandle transfer, std::error_code &ec) {
    auto* trx = GenericTransfer::from_handle(transfer.get());
    auto type = static_cast<RequestType>(setup_packet.calc_request_type());
    switch (type) {
        case RequestType::Class: {
            auto request = static_cast<HIDRequest>(setup_packet.request);
            std::uint32_t status = static_cast<std::uint32_t>(UrbStatusType::StatusOK);
            if (!setup_packet.is_out()) {
                data_type result;
                switch (request) {
                    case HIDRequest::GetIdle: {
                        result = request_get_idle(setup_packet.value >> 8, setup_packet.value,
                                                  setup_packet.length, &status);
                        if (setup_packet.length < result.size()) {
                            result.resize(setup_packet.length);
                        }
                        break;
                    }
                    case HIDRequest::GetProtocol: {
                        auto ret = request_get_protocol(&status);
                        vector_append_to_net(result, ret);
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

                session->submit_ret_submit(
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
                                seqnum, static_cast<std::uint32_t>(trx->actual_length),
                                std::move(transfer)));
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
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(
                                seqnum, status, 0));
            }
            break;
        }
        default: {
            handle_non_hid_request_type_control_urb(seqnum, ep, transfer_flags, transfer_buffer_length,
                                                    setup_packet, std::move(transfer), ec);
        }
    }

}

usbipdcpp::data_type usbipdcpp::HidVirtualInterfaceHandler::request_get_descriptor(std::uint8_t type,
    std::uint8_t language_id, std::uint16_t descriptor_length, std::uint32_t *p_status) {
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
            0x09,                   // bLength
            HidDescriptorType::Hid, // bDescriptorType: HID
            0x11,
            0x01,                      // bcdHID 1.11
            0x00,                      // bCountryCode
            0x01,                      // bNumDescriptors
            HidDescriptorType::Report, // bDescriptorType[0] HID
            static_cast<std::uint8_t>(report_descriptor_size),
            static_cast<std::uint8_t>(report_descriptor_size >> 8), // wDescriptorLength[0]
    };
}

usbipdcpp::data_type usbipdcpp::HidVirtualInterfaceHandler::request_get_idle(std::uint8_t type, std::uint8_t report_id,
                                                                             std::uint16_t length,
                                                                             std::uint32_t *p_status) {
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
    return {};
}

void usbipdcpp::HidVirtualInterfaceHandler::request_set_idle(std::uint8_t speed,
                                                             std::uint32_t *p_status) {
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
}

// ========== 非HID请求默认实现 ==========

void usbipdcpp::HidVirtualInterfaceHandler::handle_non_hid_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep,
        std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet, TransferHandle transfer, std::error_code &ec) {
    // transfer 析构时自动释放
    session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
}

// ========== 报告请求默认实现 ==========

usbipdcpp::data_type usbipdcpp::HidVirtualInterfaceHandler::request_get_report(
        std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
        std::uint32_t *p_status) {
    SPDLOG_WARN("unhandled request_get_report");
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
    return {};
}

void usbipdcpp::HidVirtualInterfaceHandler::request_set_report(
        std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
        const data_type &data, std::uint32_t *p_status) {
    SPDLOG_WARN("unhandled request_set_report");
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
}
