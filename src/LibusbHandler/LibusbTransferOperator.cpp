#include "LibusbHandler/LibusbTransferOperator.h"

#include <cstdlib>

#include <asio.hpp>
#include <libusb.h>

#include "LibusbHandler/LibusbDeviceHandler.h"
#include "constant.h"
#include "utils/SmallVector.h"

using namespace usbipdcpp;

void *LibusbTransferOperator::alloc_transfer_handle(std::size_t buffer_length, int num_iso_packets,
                                                    const UsbIpHeaderBasic &header, const SetupPacket &setup_packet) {
    auto *trx = libusb_alloc_transfer(num_iso_packets);
    if (!trx) [[unlikely]] {
        return nullptr;
    }
    // libusb_alloc_transfer 不会设置公开的 num_iso_packets 字段（文档 io.c L443 明确说明），
    // 必须用户自行赋值，否则 recv_transfer_data 中描述符读取循环读到垃圾值导致协议错位。
    trx->num_iso_packets = num_iso_packets;

    std::size_t write_offset = (header.ep == 0) ? LIBUSB_CONTROL_SETUP_SIZE : 0;
    std::size_t actual_buffer_length = buffer_length + write_offset;

    trx->buffer = static_cast<unsigned char *>(malloc(actual_buffer_length));
    if (!trx->buffer) [[unlikely]] {
        libusb_free_transfer(trx);
        return nullptr;
    }
    trx->length = static_cast<int>(actual_buffer_length);
    return trx;
}

void LibusbTransferOperator::free_transfer_handle(void *handle) {
    auto *trx = static_cast<libusb_transfer *>(handle);
    free(trx->buffer);
    libusb_free_transfer(trx);
}

std::size_t LibusbTransferOperator::get_actual_length(void *handle) {
    auto *trx = static_cast<libusb_transfer *>(handle);
    return trx->actual_length;
}

UsbIpIsoPacketDescriptor LibusbTransferOperator::get_iso_descriptor(void *handle, int index) {
    auto *trx = static_cast<libusb_transfer *>(handle);
    auto &iso = trx->iso_packet_desc[index];
    // libusb 的 iso 包在 buffer 中连续存放，offset = 前面所有包的 length 累加
    unsigned offset = 0;
    for (int i = 0; i < index; i++) {
        offset += trx->iso_packet_desc[i].length;
    }
    return UsbIpIsoPacketDescriptor{
            .offset = offset,
            .length = iso.length,
            .actual_length = iso.actual_length,
            .status = static_cast<std::uint32_t>(LibusbDeviceHandler::trxstat2error(iso.status)),
    };
}

void LibusbTransferOperator::set_iso_descriptor(void *handle, int index, const UsbIpIsoPacketDescriptor &desc) {
    auto *trx = static_cast<libusb_transfer *>(handle);
    auto &iso = trx->iso_packet_desc[index];
    iso.status = LibusbDeviceHandler::error2trxstat(desc.status);
    iso.actual_length = desc.actual_length;
    iso.length = desc.length;
}

void LibusbTransferOperator::send_transfer_data(void *handle, asio::ip::tcp::socket &sock, std::size_t length,
                                                std::error_code &ec) {
    auto *trx = static_cast<libusb_transfer *>(handle);
    if (trx->type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS && trx->num_iso_packets > 0) {
        SmallVector<asio::const_buffer, 130> buffers;
        SmallVector<decltype(UsbIpIsoPacketDescriptor{}.to_bytes()), 130> desc_bytes;
        // offset: buffer 中的包槽位偏移（pkt.length 步长），同时用于数据读取和描述符 offset 字段。
        //   槽位大小由客户端 CMD_SUBMIT 的描述符 length 决定，必须按 pkt.length 步进而非 actual_length，
        //   否则 vhci 会把包 N 的数据错误地写入包 N-1 的槽位中。
        bool need_to_send_buffer = (length > 0);
        std::uint32_t offset = 0;
        for (int i = 0; i < trx->num_iso_packets; i++) {
            auto &pkt = trx->iso_packet_desc[i];
            if (need_to_send_buffer)
                buffers.push_back(asio::buffer(trx->buffer + offset, pkt.actual_length));
            UsbIpIsoPacketDescriptor desc{
                    .offset = offset,
                    .length = pkt.length,
                    .actual_length = pkt.actual_length,
                    .status = static_cast<std::uint32_t>(LibusbDeviceHandler::trxstat2error(pkt.status)),
            };
            desc_bytes.push_back(desc.to_bytes());
            offset += pkt.length;
        }
        for (auto &bytes: desc_bytes) {
            buffers.push_back(asio::buffer(bytes));
        }
        asio::write(sock, buffers, ec);
    }
    else if (length > 0) {
        auto *buf = trx->buffer + (trx->type == LIBUSB_TRANSFER_TYPE_CONTROL ? LIBUSB_CONTROL_SETUP_SIZE : 0);
        asio::write(sock, asio::buffer(buf, length), ec);
    }
}

void LibusbTransferOperator::recv_transfer_data(void *handle, asio::ip::tcp::socket &sock, std::size_t length,
                                                std::error_code &ec) {
    auto *trx = static_cast<libusb_transfer *>(handle);
    if (length > 0) {
        // 控制传输 buffer 前 8 字节留给 setup 包，由后续 receive_urb 填入；
        // 此处从偏移 8 开始读取数据阶段内容。
        // trx->type 此时尚未设置，不能用来判断传输类型；
        // 改用 trx->length 判断：alloc 时控制传输多加了 LIBUSB_CONTROL_SETUP_SIZE，
        // 因此 trx->length > length 说明 buffer 包含 setup 前缀
        bool is_control = (static_cast<std::size_t>(trx->length) > length);
        auto *buf = trx->buffer + (is_control ? LIBUSB_CONTROL_SETUP_SIZE : 0);
        asio::read(sock, asio::buffer(buf, length), ec);
        if (ec)
            return;
    }

    for (int i = 0; i < trx->num_iso_packets; i++) {
        UsbIpIsoPacketDescriptor iso_desc{};
        iso_desc.from_socket(sock);
        set_iso_descriptor(handle, i, iso_desc);
    }
}
