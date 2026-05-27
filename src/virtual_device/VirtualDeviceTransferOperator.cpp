// #define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_DEBUG

#include "virtual_device/VirtualDeviceTransferOperator.h"

#include <spdlog/spdlog.h>
#include "constant.h"

using namespace usbipdcpp;

TransferOperator *VirtualDeviceTransferOperator::get_operator_for_ep(std::uint8_t ep) {
    auto it = ep_operators_.find(ep);
    return (it != ep_operators_.end()) ? it->second : &generic_op_;
}

void *VirtualDeviceTransferOperator::alloc_transfer_handle(std::size_t buffer_length, int num_iso_packets,
                                                           const UsbIpHeaderBasic &header,
                                                           const SetupPacket &setup_packet) {
    auto *leaf_op = get_operator_for_ep(static_cast<std::uint8_t>(header.ep));
    return leaf_op->alloc_transfer_handle(buffer_length, num_iso_packets, header, setup_packet);
}

void VirtualDeviceTransferOperator::free_transfer_handle(void *handle) {
    // leaf op 已存入 TransferHandle，正常路径不会走到这里。
    // 如果走了，说明 caller 没有正确使用 TransferHandle::get_operator()。
    SPDLOG_ERROR("VDTO::free_transfer_handle handle={:p} 不应被调用", static_cast<const void *>(handle));
}

void *VirtualDeviceTransferOperator::get_transfer_buffer(void *handle) {
    SPDLOG_WARN("VDTO::get_transfer_buffer 不应被调用，请通过 TransferHandle::get_operator() 直调 leaf op");
    return generic_op_.get_transfer_buffer(handle);
}

std::size_t VirtualDeviceTransferOperator::get_actual_length(void *handle) {
    return generic_op_.get_actual_length(handle);
}

std::size_t VirtualDeviceTransferOperator::get_read_data_offset(void *handle) {
    return generic_op_.get_read_data_offset(handle);
}

std::size_t VirtualDeviceTransferOperator::get_write_data_offset(const UsbIpHeaderBasic &header) {
    auto *leaf_op = get_operator_for_ep(static_cast<std::uint8_t>(header.ep));
    return leaf_op->get_write_data_offset(header);
}

UsbIpIsoPacketDescriptor VirtualDeviceTransferOperator::get_iso_descriptor(void *handle, int index) {
    return generic_op_.get_iso_descriptor(handle, index);
}

void VirtualDeviceTransferOperator::set_iso_descriptor(void *handle, int index, const UsbIpIsoPacketDescriptor &desc) {
    generic_op_.set_iso_descriptor(handle, index, desc);
}

void VirtualDeviceTransferOperator::send_transfer_data(void *handle, asio::ip::tcp::socket &sock, std::size_t length,
                                                       std::error_code &ec) {
    generic_op_.send_transfer_data(handle, sock, length, ec);
}

void VirtualDeviceTransferOperator::recv_transfer_data(void *handle, asio::ip::tcp::socket &sock, std::size_t length,
                                                       std::error_code &ec) {
    generic_op_.recv_transfer_data(handle, sock, length, ec);
}

bool VirtualDeviceTransferOperator::is_custom_io(void *handle) const {
    return false;
}
