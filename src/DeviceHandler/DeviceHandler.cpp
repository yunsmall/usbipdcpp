#include "DeviceHandler/DeviceHandler.h"

#include <spdlog/spdlog.h>

#include "interface.h"
#include "constant.h"
#include "Device.h"
#include "Session.h"
#include "protocol.h"
#include "type.h"
#include "InterfaceHandler/InterfaceHandler.h"

using namespace usbipdcpp;

AbstDeviceHandler::AbstDeviceHandler(AbstDeviceHandler &&other) noexcept :
    handle_device(other.handle_device) {
}

void AbstDeviceHandler::dispatch_urb(
        const UsbIpCommand::UsbIpCmdSubmit &cmd,
        std::uint32_t seqnum,
        const UsbEndpoint &ep,
        std::optional<UsbInterface> &interface,
        std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet,
        usbipdcpp::error_code &ec) {
    // 控制传输较少，Bulk/Interrupt 更常见
    if (ep.attributes == static_cast<std::uint8_t>(EndpointAttributes::Control))[[unlikely]] {
        SPDLOG_DEBUG("处理控制传输，setup包为{}\n{}", get_every_byte(setup_packet.to_bytes()), setup_packet.to_string());
        handle_control_urb(seqnum, ep, transfer_flags, transfer_buffer_length, setup_packet, std::move(cmd.transfer), ec);
    }
    else if (interface.has_value())[[likely]] {
        auto &intf = interface.value();
        // Bulk 和 Interrupt 最常见
        if (ep.attributes == static_cast<std::uint8_t>(EndpointAttributes::Bulk))[[likely]] {
            SPDLOG_DEBUG("处理块传输");
            handle_bulk_transfer(seqnum, ep, intf, transfer_flags, transfer_buffer_length, std::move(cmd.transfer), ec);
        }
        else if (ep.attributes == static_cast<std::uint8_t>(EndpointAttributes::Interrupt)) {
            SPDLOG_DEBUG("处理中断传输");
            handle_interrupt_transfer(seqnum, ep, intf, transfer_flags, transfer_buffer_length, std::move(cmd.transfer), ec);
        }
        else if (ep.attributes == static_cast<std::uint8_t>(EndpointAttributes::Isochronous)) {
            SPDLOG_DEBUG("处理等时传输");
            int num_iso = (cmd.number_of_packets != 0 && cmd.number_of_packets != 0xFFFFFFFF)
                          ? static_cast<int>(cmd.number_of_packets) : 0;
            handle_isochronous_transfer(seqnum, ep, intf, transfer_flags, transfer_buffer_length,
                                        std::move(cmd.transfer), num_iso, ec);
        }
        else [[unlikely]] {
            SPDLOG_DEBUG("端口{:02x}的未知传输类型：{}", ep.address, ep.attributes);
            ec = make_error_code(ErrorType::INVALID_ARG);
        }
    }
    else[[unlikely]] {
        SPDLOG_ERROR("非控制传输却不存在目标接口");
        ec = make_error_code(ErrorType::INTERNAL_ERROR);
    }
}

void AbstDeviceHandler::trigger_session_stop() {
    std::lock_guard lock(session_mutex_);
    if (session)[[likely]] {
        session->immediately_stop();
    }
}

// ========== transfer_handle 操作默认实现（使用 GenericTransfer） ==========

void* AbstDeviceHandler::alloc_transfer_handle(std::size_t buffer_length, int num_iso_packets, const UsbIpHeaderBasic& header, const SetupPacket& setup_packet) {
    auto* trx = new GenericTransfer{};
    trx->data.resize(buffer_length);
    trx->iso_descriptors.resize(num_iso_packets);
    return trx;
}

void* AbstDeviceHandler::get_transfer_buffer(void* transfer_handle) {
    auto* trx = GenericTransfer::from_handle(transfer_handle);
    return trx->data.data();
}

std::size_t AbstDeviceHandler::get_actual_length(void* transfer_handle) {
    auto* trx = GenericTransfer::from_handle(transfer_handle);
    return trx->actual_length;
}

std::size_t AbstDeviceHandler::get_read_data_offset(void* transfer_handle) {
    auto* trx = GenericTransfer::from_handle(transfer_handle);
    return trx->data_offset;
}

std::size_t AbstDeviceHandler::get_write_data_offset(const UsbIpHeaderBasic& header) {
    // 默认实现：不跳过任何字节
    return 0;
}

UsbIpIsoPacketDescriptor AbstDeviceHandler::get_iso_descriptor(void* transfer_handle, int index) {
    auto* trx = GenericTransfer::from_handle(transfer_handle);
    return trx->iso_descriptors[index];
}

void AbstDeviceHandler::set_iso_descriptor(void* transfer_handle, int index, const UsbIpIsoPacketDescriptor& desc) {
    auto* trx = GenericTransfer::from_handle(transfer_handle);
    trx->iso_descriptors[index] = desc;
}

void AbstDeviceHandler::free_transfer_handle(void* transfer_handle) {
    auto* trx = GenericTransfer::from_handle(transfer_handle);
    delete trx;
}
