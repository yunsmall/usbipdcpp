#include "virtual_device/VirtualDeviceTransferOperator.h"

#include "constant.h"

using namespace usbipdcpp;

TransferOperator* VirtualDeviceTransferOperator::find_op(void* handle) const {
    auto it = handle_operators_.find(handle);
    return (it != handle_operators_.end()) ? it->second : nullptr;
}

void* VirtualDeviceTransferOperator::alloc_transfer_handle(std::size_t buffer_length, int num_iso_packets,
                                                            const UsbIpHeaderBasic& header,
                                                            const SetupPacket& setup_packet) {
    auto it = ep_operators_.find(static_cast<std::uint8_t>(header.ep));
    TransferOperator* op = (it != ep_operators_.end()) ? it->second : &generic_op_;
    void* handle = op->alloc_transfer_handle(buffer_length, num_iso_packets, header, setup_packet);
    handle_operators_[handle] = op;
    return handle;
}

void VirtualDeviceTransferOperator::free_transfer_handle(void* handle) {
    auto it = handle_operators_.find(handle);
    if (it != handle_operators_.end()) {
        it->second->free_transfer_handle(handle);
        handle_operators_.erase(it);
    }
}

void* VirtualDeviceTransferOperator::get_transfer_buffer(void* handle) {
    auto* op = find_op(handle);
    return op ? op->get_transfer_buffer(handle) : nullptr;
}

std::size_t VirtualDeviceTransferOperator::get_actual_length(void* handle) {
    auto* op = find_op(handle);
    return op ? op->get_actual_length(handle) : 0;
}

std::size_t VirtualDeviceTransferOperator::get_read_data_offset(void* handle) {
    auto* op = find_op(handle);
    return op ? op->get_read_data_offset(handle) : 0;
}

std::size_t VirtualDeviceTransferOperator::get_write_data_offset(const UsbIpHeaderBasic& header) {
    auto it = ep_operators_.find(static_cast<std::uint8_t>(header.ep));
    return (it != ep_operators_.end()) ? it->second->get_write_data_offset(header) : 0;
}

UsbIpIsoPacketDescriptor VirtualDeviceTransferOperator::get_iso_descriptor(void* handle, int index) {
    auto* op = find_op(handle);
    if (op) return op->get_iso_descriptor(handle, index);
    return {};
}

void VirtualDeviceTransferOperator::set_iso_descriptor(void* handle, int index,
                                                        const UsbIpIsoPacketDescriptor& desc) {
    auto* op = find_op(handle);
    if (op) op->set_iso_descriptor(handle, index, desc);
}

void VirtualDeviceTransferOperator::send_transfer_data(void* handle, asio::ip::tcp::socket& sock,
                                                        std::size_t length, std::error_code& ec) {
    auto* op = find_op(handle);
    if (op)
        op->send_transfer_data(handle, sock, length, ec);
    else
        ec = std::make_error_code(std::errc::invalid_argument);
}

void VirtualDeviceTransferOperator::recv_transfer_data(void* handle, asio::ip::tcp::socket& sock,
                                                        std::size_t length, std::error_code& ec) {
    auto* op = find_op(handle);
    if (op)
        op->recv_transfer_data(handle, sock, length, ec);
    else
        ec = std::make_error_code(std::errc::invalid_argument);
}

bool VirtualDeviceTransferOperator::is_custom_io(void* handle) const {
    auto it = handle_operators_.find(handle);
    return (it != handle_operators_.end()) ? it->second->is_custom_io(handle) : false;
}
