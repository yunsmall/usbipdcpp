#include "InterfaceHandler/VirtualInterfaceHandler.h"

#include "Session.h"

using namespace usbipdcpp;

void VirtualInterfaceHandler::handle_bulk_transfer(Session &session, std::uint32_t seqnum, const UsbEndpoint &ep,
                                                   std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
                                                   const data_type &out_data,
                                                   std::error_code &ec) {
    SPDLOG_WARN("虚拟接口在端口{:04x}默认实现的块传输实现", ep.address);
    session.submit_ret_submit(
            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(
                    seqnum
                    )
            );
}

void VirtualInterfaceHandler::handle_interrupt_transfer(Session &session, std::uint32_t seqnum, const UsbEndpoint &ep,
                                                        std::uint32_t transfer_flags,
                                                        std::uint32_t transfer_buffer_length, const data_type &out_data,
                                                        std::error_code &ec) {
    SPDLOG_WARN("虚拟接口在端口{:04x}默认实现的中断传输实现", ep.address);
    session.submit_ret_submit(
            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(
                    seqnum
                    )
            );
}

void VirtualInterfaceHandler::handle_isochronous_transfer(Session &session, std::uint32_t seqnum, const UsbEndpoint &ep,
                                         std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
                                         const data_type &out_data,
                                         const std::vector<UsbIpIsoPacketDescriptor> &iso_packet_descriptors,
                                         std::error_code &ec) {
    SPDLOG_WARN("虚拟接口在端口{:04x}默认实现的等时传输实现", ep.address);
    session.submit_ret_submit(
            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(
                    seqnum
                    )
            );
}

data_type VirtualInterfaceHandler::request_get_descriptor(std::uint8_t type, std::uint8_t language_id,
                                                          std::uint16_t descriptor_length, std::uint32_t *p_status) {
    *p_status = static_cast<uint32_t>(UrbStatusType::StatusEPIPE);
    return {};
}
