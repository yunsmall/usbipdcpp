#include "LibusbHandler/LibusbTransferOperator.h"

#include <cstdlib>

#include <libusb.h>

#include "LibusbHandler/LibusbDeviceHandler.h"
#include "constant.h"

using namespace usbipdcpp;

void* LibusbTransferOperator::alloc_transfer_handle(std::size_t buffer_length, int num_iso_packets,
                                                     const UsbIpHeaderBasic& header,
                                                     const SetupPacket& setup_packet) {
    auto* trx = libusb_alloc_transfer(num_iso_packets);
    if (!trx) [[unlikely]] {
        return nullptr;
    }

    std::size_t write_offset = get_write_data_offset(header);
    std::size_t actual_buffer_length = buffer_length + write_offset;

    trx->buffer = static_cast<unsigned char*>(malloc(actual_buffer_length));
    if (!trx->buffer) [[unlikely]] {
        libusb_free_transfer(trx);
        return nullptr;
    }
    trx->length = static_cast<int>(actual_buffer_length);
    return trx;
}

void LibusbTransferOperator::free_transfer_handle(void* handle) {
    auto* trx = static_cast<libusb_transfer*>(handle);
    free(trx->buffer);
    libusb_free_transfer(trx);
}

void* LibusbTransferOperator::get_transfer_buffer(void* handle) {
    auto* trx = static_cast<libusb_transfer*>(handle);
    return trx->buffer;
}

std::size_t LibusbTransferOperator::get_actual_length(void* handle) {
    auto* trx = static_cast<libusb_transfer*>(handle);
    return trx->actual_length;
}

std::size_t LibusbTransferOperator::get_read_data_offset(void* handle) {
    auto* trx = static_cast<libusb_transfer*>(handle);
    if (trx->type == LIBUSB_TRANSFER_TYPE_CONTROL) {
        return LIBUSB_CONTROL_SETUP_SIZE;
    }
    return 0;
}

std::size_t LibusbTransferOperator::get_write_data_offset(const UsbIpHeaderBasic& header) {
    if (header.ep == 0) {
        return LIBUSB_CONTROL_SETUP_SIZE;
    }
    return 0;
}

UsbIpIsoPacketDescriptor LibusbTransferOperator::get_iso_descriptor(void* handle, int index) {
    auto* trx = static_cast<libusb_transfer*>(handle);
    auto& iso = trx->iso_packet_desc[index];
    return UsbIpIsoPacketDescriptor{
        .offset = 0,
        .length = iso.length,
        .actual_length = iso.actual_length,
        .status = static_cast<std::uint32_t>(LibusbDeviceHandler::trxstat2error(iso.status)),
        .length_in_transfer_buffer_only_for_send = iso.length
    };
}

void LibusbTransferOperator::set_iso_descriptor(void* handle, int index, const UsbIpIsoPacketDescriptor& desc) {
    auto* trx = static_cast<libusb_transfer*>(handle);
    auto& iso = trx->iso_packet_desc[index];
    iso.status = LibusbDeviceHandler::error2trxstat(desc.status);
    iso.actual_length = desc.actual_length;
    iso.length = desc.length;
}

void LibusbTransferOperator::send_transfer_data(void* handle, asio::ip::tcp::socket& sock,
                                                 std::size_t length, std::error_code& ec) {
    void* buffer = get_transfer_buffer(handle);
    asio::write(sock, asio::buffer(static_cast<const char*>(buffer), length), ec);
}

void LibusbTransferOperator::recv_transfer_data(void* handle, asio::ip::tcp::socket& sock,
                                                 std::size_t length, std::error_code& ec) {
    void* buffer = get_transfer_buffer(handle);
    asio::read(sock, asio::buffer(static_cast<std::uint8_t*>(buffer), length), ec);
}
