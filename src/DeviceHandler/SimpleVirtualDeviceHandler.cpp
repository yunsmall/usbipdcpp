#include "DeviceHandler/SimpleVirtualDeviceHandler.h"
#include "Session.h"
#include "InterfaceHandler/VirtualInterfaceHandler.h"


using namespace usbipdcpp;

void usbipdcpp::SimpleVirtualDeviceHandler::handle_non_standard_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet, const data_type &out_data, std::error_code &ec) {
    SPDLOG_ERROR("Unimplement non standard control transfer request to simple device");
    session->submit_ret_submit(
            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum)
            );
}


void usbipdcpp::SimpleVirtualDeviceHandler::request_clear_feature(std::uint16_t feature_selector,
                                                                  std::uint32_t *p_status) {
    SPDLOG_WARN("Unimplement request_clear_feature to simple device");
}

std::uint16_t usbipdcpp::SimpleVirtualDeviceHandler::request_get_status(std::uint32_t *p_status) {
    SPDLOG_WARN("Unimplement request_get_status to simple device");
    return 0x00000000u;
}

void usbipdcpp::SimpleVirtualDeviceHandler::request_set_address(std::uint16_t address, std::uint32_t *status) {
    SPDLOG_WARN("Unimplement request_set_address to simple device");
}

void usbipdcpp::SimpleVirtualDeviceHandler::request_set_configuration(std::uint16_t configuration_value,
                                                                      std::uint32_t *p_status) {
    SPDLOG_WARN("Unimplement request_set_configuration to simple device");
}

void usbipdcpp::SimpleVirtualDeviceHandler::request_set_descriptor(std::uint8_t desc_type, std::uint8_t desc_index,
                                                                   std::uint16_t language_id,
                                                                   std::uint16_t descriptor_length,
                                                                   const data_type &descriptor,
                                                                   std::uint32_t *p_status) {
    SPDLOG_WARN("Unimplement request_set_descriptor to simple device");
}

void usbipdcpp::SimpleVirtualDeviceHandler::
request_set_feature(std::uint16_t feature_selector, std::uint32_t *p_status) {
    SPDLOG_WARN("Unimplement request_set_feature to simple device");
}

usbipdcpp::data_type usbipdcpp::SimpleVirtualDeviceHandler::get_other_speed_descriptor(std::uint8_t language_id,
    std::uint16_t descriptor_length, std::uint32_t *p_status) {
    SPDLOG_WARN("Unimplement get_other_speed_descriptor to simple device");
    return {};
}

void usbipdcpp::SimpleVirtualDeviceHandler::set_descriptor(std::uint16_t configuration_value) {
    SPDLOG_WARN("Unimplement set_descriptor to simple device");
}

void SimpleVirtualDeviceHandler::handle_unlink_seqnum(std::uint32_t seqnum) {
    SPDLOG_WARN("Unimplemented handle_unlink_seqnum, seqnum: {}", seqnum);
}
