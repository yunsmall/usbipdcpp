#include "usbipdcpp/DeviceHandler/TransferOperator.h"

#include <algorithm>

#include "usbipdcpp/constant.h"
#include "usbipdcpp/utils/SmallVector.h"

using namespace usbipdcpp;

// ========== GenericTransferOperator 实现 ==========

void *GenericTransferOperator::alloc_transfer_handle(std::size_t buffer_length, int num_iso_packets,
                                                     const UsbIpHeaderBasic &header, const SetupPacket &setup_packet) {
    auto *trx = new GenericTransfer{};
    trx->data.resize(buffer_length);
    trx->iso_descriptors.resize(num_iso_packets);
    return trx;
}

void GenericTransferOperator::free_transfer_handle(void *handle) {
    delete GenericTransfer::from_handle(handle);
}

std::size_t GenericTransferOperator::get_actual_length(void *handle) {
    return GenericTransfer::from_handle(handle)->actual_length;
}

std::size_t GenericTransferOperator::set_transfer_data(void *handle, const data_type &data, std::size_t max_length) {
    auto *trx = GenericTransfer::from_handle(handle);
    auto send_len = std::min(data.size(), max_length);
    trx->data.assign(data.begin(), data.begin() + send_len);
    trx->actual_length = send_len;
    return send_len;
}

std::size_t GenericTransferOperator::get_transfer_data(void *handle, data_type &out, bool &supported) {
    // OUT 已接收数据追加到 out 末尾（调用方决定怎么消费）
    auto *trx = GenericTransfer::from_handle(handle);
    supported = true;
    out.insert(out.end(), trx->data.begin(), trx->data.end());
    return trx->data.size();
}

UsbIpIsoPacketDescriptor GenericTransferOperator::get_iso_descriptor(void *handle, int index) {
    return GenericTransfer::from_handle(handle)->iso_descriptors[index];
}

void GenericTransferOperator::set_iso_descriptor(void *handle, int index, const UsbIpIsoPacketDescriptor &desc) {
    GenericTransfer::from_handle(handle)->iso_descriptors[index] = desc;
}

void GenericTransferOperator::send_transfer_data(void *handle, asio::ip::tcp::socket &sock, std::size_t length,
                                                 std::error_code &ec) {
    auto *trx = GenericTransfer::from_handle(handle);
    if (trx->iso_descriptors.empty()) {
        if (length > 0)
            asio::write(sock, asio::buffer(trx->data.data() + trx->data_offset, length), ec);
    }
    else {
        SmallVector<asio::const_buffer, 130> buffers;
        SmallVector<decltype(UsbIpIsoPacketDescriptor{}.to_bytes()), 130> desc_bytes;
        bool need_to_send_buffer = (length > 0);
        for (auto &iso: trx->iso_descriptors) {
            if (need_to_send_buffer)
                buffers.push_back(asio::buffer(trx->data.data() + iso.offset, iso.actual_length));
            // 描述符原样透传，offset/length 保持调用者设置的 buffer 布局值
            desc_bytes.push_back(iso.to_bytes());
        }
        for (auto &bytes: desc_bytes) {
            buffers.push_back(asio::buffer(bytes));
        }
        asio::write(sock, buffers, ec);
    }
}

void GenericTransferOperator::recv_transfer_data(void *handle, asio::ip::tcp::socket &sock, std::size_t length,
                                                 std::error_code &ec) {
    auto *trx = GenericTransfer::from_handle(handle);
    if (length > 0) {
        asio::read(sock, asio::buffer(trx->data.data(), length), ec);
        if (ec)
            return;
    }
    if (!trx->iso_descriptors.empty()) {
        for (auto &iso: trx->iso_descriptors) {
            iso.from_socket(sock);
        }
    }
}
