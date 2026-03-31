#include "simple_device.h"

#include <spdlog/spdlog.h>

// HID报告描述符 - 一个简单的按钮
const usbipdcpp::data_type SimpleHidInterfaceHandler::report_descriptor_ = {
        0x05, 0x01, // Usage Page (Generic Desktop)
        0x09, 0x01, // Usage (Pointer)
        0xA1, 0x01, // Collection (Application)
        0x09, 0x01, //   Usage (Pointer)
        0x15, 0x00, //   Logical Minimum (0)
        0x25, 0x01, //   Logical Maximum (1)
        0x75, 0x01, //   Report Size (1)
        0x95, 0x01, //   Report Count (1)
        0x81, 0x02, //   Input (Data,Var,Abs)
        0xC0        // End Collection
};

SimpleHidInterfaceHandler::SimpleHidInterfaceHandler(usbipdcpp::UsbInterface &handle_interface,
                                                     usbipdcpp::StringPool &string_pool) :
    HidVirtualInterfaceHandler(handle_interface, string_pool) {
}

void SimpleHidInterfaceHandler::handle_interrupt_transfer(std::uint32_t seqnum, const usbipdcpp::UsbEndpoint &ep,
                                                          std::uint32_t transfer_flags,
                                                          std::uint32_t transfer_buffer_length,
                                                          const usbipdcpp::data_type &out_data,
                                                          std::error_code &ec) {
    SPDLOG_DEBUG("SimpleHidInterfaceHandler::handle_interrupt_transfer on ep 0x{:02x}", ep.address);

    if (ep.is_in()) {
        // 返回一个简单的数据
        usbipdcpp::data_type data = {0x00};
        session.load()->submit_ret_submit(
                usbipdcpp::UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(seqnum, data));
    }
    else {
        session.load()->submit_ret_submit(
                usbipdcpp::UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(seqnum));
    }
}

void SimpleHidInterfaceHandler::on_new_connection(usbipdcpp::Session &current_session, usbipdcpp::error_code &ec) {
    HidVirtualInterfaceHandler::on_new_connection(current_session, ec);
    SPDLOG_INFO("SimpleHidInterfaceHandler connected");
}

void SimpleHidInterfaceHandler::on_disconnection(usbipdcpp::error_code &ec) {
    HidVirtualInterfaceHandler::on_disconnection(ec);
    SPDLOG_INFO("SimpleHidInterfaceHandler disconnected");
}

void SimpleHidInterfaceHandler::request_clear_feature(std::uint16_t feature_selector, std::uint32_t *p_status) {
    *p_status = 0;
}

void SimpleHidInterfaceHandler::request_endpoint_clear_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                                               std::uint32_t *p_status) {
    *p_status = 0;
}

std::uint8_t SimpleHidInterfaceHandler::request_get_interface(std::uint32_t *p_status) {
    *p_status = 0;
    return 0;
}

void SimpleHidInterfaceHandler::request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) {
    *p_status = 0;
}

std::uint16_t SimpleHidInterfaceHandler::request_get_status(std::uint32_t *p_status) {
    *p_status = 0;
    return 0;
}

std::uint16_t SimpleHidInterfaceHandler::request_endpoint_get_status(std::uint8_t ep_address, std::uint32_t *p_status) {
    *p_status = 0;
    return 0;
}

void SimpleHidInterfaceHandler::request_set_feature(std::uint16_t feature_selector, std::uint32_t *p_status) {
    *p_status = static_cast<std::uint32_t>(usbipdcpp::UrbStatusType::StatusEPIPE);
}

void SimpleHidInterfaceHandler::request_endpoint_set_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                                             std::uint32_t *p_status) {
    *p_status = static_cast<std::uint32_t>(usbipdcpp::UrbStatusType::StatusEPIPE);
}

std::uint16_t SimpleHidInterfaceHandler::get_report_descriptor_size() {
    return report_descriptor_.size();
}

usbipdcpp::data_type SimpleHidInterfaceHandler::get_report_descriptor() {
    return report_descriptor_;
}

void SimpleHidInterfaceHandler::handle_non_hid_request_type_control_urb(
        std::uint32_t seqnum, const usbipdcpp::UsbEndpoint &ep,
        std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const usbipdcpp::SetupPacket &setup_packet,
        const usbipdcpp::data_type &out_data, std::error_code &ec) {
    SPDLOG_DEBUG("SimpleHidInterfaceHandler::handle_non_hid_request_type_control_urb");
    session.load()->submit_ret_submit(
            usbipdcpp::UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_no_iso(seqnum, {}));
}

usbipdcpp::data_type SimpleHidInterfaceHandler::request_get_report(std::uint8_t type, std::uint8_t report_id,
                                                                   std::uint16_t length,
                                                                   std::uint32_t *p_status) {
    *p_status = 0;
    return {0x00};
}

void SimpleHidInterfaceHandler::request_set_report(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                                                   const usbipdcpp::data_type &data,
                                                   std::uint32_t *p_status) {
    *p_status = 0;
}

usbipdcpp::data_type SimpleHidInterfaceHandler::request_get_idle(std::uint8_t type, std::uint8_t report_id,
                                                                 std::uint16_t length,
                                                                 std::uint32_t *p_status) {
    *p_status = 0;
    return {};
}

void SimpleHidInterfaceHandler::request_set_idle(std::uint8_t speed, std::uint32_t *p_status) {
    *p_status = 0;
}

// SimpleDeviceHandler 实现
SimpleDeviceHandler::SimpleDeviceHandler(usbipdcpp::UsbDevice &handle_device, usbipdcpp::StringPool &string_pool) :
    SimpleVirtualDeviceHandler(handle_device, string_pool) {
}
