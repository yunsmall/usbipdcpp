#include "DeviceHandler/DeviceHandler.h"

#include <spdlog/spdlog.h>

#include "Interface.h"
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
