#include "DeviceHandler/TransferOperator.h"

#include "constant.h"

using namespace usbipdcpp;

// ========== GenericTransferOperator 实现 ==========

void* GenericTransferOperator::alloc_transfer_handle(std::size_t buffer_length, int num_iso_packets,
                                                      const UsbIpHeaderBasic& header,
                                                      const SetupPacket& setup_packet) {
    auto* trx = new GenericTransfer{};
    trx->data.resize(buffer_length);
    trx->iso_descriptors.resize(num_iso_packets);
    return trx;
}

void GenericTransferOperator::free_transfer_handle(void* handle) {
    delete GenericTransfer::from_handle(handle);
}

void* GenericTransferOperator::get_transfer_buffer(void* handle) {
    return GenericTransfer::from_handle(handle)->data.data();
}

std::size_t GenericTransferOperator::get_actual_length(void* handle) {
    return GenericTransfer::from_handle(handle)->actual_length;
}

std::size_t GenericTransferOperator::get_read_data_offset(void* handle) {
    return GenericTransfer::from_handle(handle)->data_offset;
}

std::size_t GenericTransferOperator::get_write_data_offset(const UsbIpHeaderBasic& header) {
    return 0;
}

UsbIpIsoPacketDescriptor GenericTransferOperator::get_iso_descriptor(void* handle, int index) {
    return GenericTransfer::from_handle(handle)->iso_descriptors[index];
}

void GenericTransferOperator::set_iso_descriptor(void* handle, int index, const UsbIpIsoPacketDescriptor& desc) {
    GenericTransfer::from_handle(handle)->iso_descriptors[index] = desc;
}

void GenericTransferOperator::send_transfer_data(void* handle, asio::ip::tcp::socket& sock,
                                                  std::size_t length, std::error_code& ec) {
    void* buffer = get_transfer_buffer(handle);
    asio::write(sock, asio::buffer(static_cast<const char*>(buffer), length), ec);
}

void GenericTransferOperator::recv_transfer_data(void* handle, asio::ip::tcp::socket& sock,
                                                  std::size_t length, std::error_code& ec) {
    void* buffer = get_transfer_buffer(handle);
    asio::read(sock, asio::buffer(static_cast<std::uint8_t*>(buffer), length), ec);
}
