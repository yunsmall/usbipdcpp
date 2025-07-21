#include "DeviceHandler.h"

#include <variant>
#include <variant>
#include <spdlog/spdlog.h>

#include "interface.h"
#include "constant.h"
#include "device.h"
#include "protocol.h"
#include "type.h"
#include "VirtualDeviceHandler.h"
#include "InterfaceHandler.h"

using namespace usbipcpp;

AbstDeviceHandler::AbstDeviceHandler(AbstDeviceHandler &&other) noexcept :
    handle_device(other.handle_device) {
}

void AbstDeviceHandler::dispatch_urb(Session &session,
                                     const UsbIpCommand::UsbIpCmdSubmit &cmd,
                                     std::uint32_t seqnum,
                                     const UsbEndpoint &ep,
                                     std::optional<UsbInterface> &interface,
                                     std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
                                     const SetupPacket &setup_packet, const std::vector<std::uint8_t> &out_data,
                                     const std::vector<UsbIpIsoPacketDescriptor> &iso_packet_descriptors,
                                     usbipcpp::error_code &ec) {
    std::lock_guard lock(self_mutex);
    if (ep.attributes == static_cast<std::uint8_t>(EndpointAttributes::Control)) {
        SPDLOG_DEBUG("处理控制传输");
        handle_control_urb(session, seqnum, ep, transfer_flags, transfer_buffer_length, setup_packet, out_data,
                           ec);
    }
    else if (interface.has_value()) {
        auto &intf = interface.value();
        if (ep.attributes == static_cast<std::uint8_t>(EndpointAttributes::Bulk)) {
            SPDLOG_DEBUG("处理块传输");
            handle_bulk_transfer(session, seqnum, ep, intf, transfer_flags, transfer_buffer_length, out_data, ec);
        }
        else if (ep.attributes == static_cast<std::uint8_t>(EndpointAttributes::Interrupt)) {
            SPDLOG_DEBUG("处理中断传输");
            handle_interrupt_transfer(session, seqnum, ep, intf, transfer_flags, transfer_buffer_length, out_data, ec);
        }
        else if (ep.attributes == static_cast<std::uint8_t>(EndpointAttributes::Isochronous)) {
            SPDLOG_DEBUG("处理等时传输");
            handle_isochronous_transfer(session, seqnum, ep, intf, transfer_flags, transfer_buffer_length,
                                        out_data, iso_packet_descriptors, ec);
        }
        else {
            SPDLOG_DEBUG("端口{:02x}的未知传输类型：{}", ep.address, ep.attributes);
            ec = make_error_code(ErrorType::INVALID_ARG);
        }
    }
    else {
        SPDLOG_ERROR("非控制传输却不存在目标接口");
        ec = make_error_code(ErrorType::INTERNAL_ERROR);
    }


}

void AbstDeviceHandler::handle_unlink_seqnum(std::uint32_t seqnum) {
}
