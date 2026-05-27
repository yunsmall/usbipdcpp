// #define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_DEBUG

#include "virtual_device/storage_backends/StorageTransferOperator.h"

#include <spdlog/spdlog.h>
#include "virtual_device/devices/MscBulkOnlyHandler.h"
#include "virtual_device/storage_backends/StorageIoTransfer.h"

using namespace usbipdcpp;

StorageTransferOperator::StorageTransferOperator(MscBulkOnlyHandler *handler) : handler_(handler) {
}

void *StorageTransferOperator::alloc_transfer_handle(std::size_t buffer_length, int, const UsbIpHeaderBasic &header,
                                                     const SetupPacket &) {
    auto *trx = pool_.alloc();
    if (!trx)
        trx = new StorageIoTransfer{};
    SPDLOG_DEBUG("STO::alloc handle={:p} dir={} len={}", static_cast<const void *>(trx),
                 header.direction == UsbIpDirection::In ? "IN" : "OUT", buffer_length);
    if (header.direction == UsbIpDirection::Out) {
        trx->external_buf = handler_->prepare_out_buffer(buffer_length, trx);
        SPDLOG_DEBUG("STO::alloc OUT external_buf={:p}", static_cast<const void *>(trx->external_buf));
    }
    return trx;
}

void StorageTransferOperator::free_transfer_handle(void *handle) {
    auto *trx = StorageIoTransfer::from_handle(handle);
    SPDLOG_DEBUG("STO::free handle={:p}", static_cast<const void *>(handle));
    trx->reset();
    if (!pool_.free(trx))
        delete trx;
}

void *StorageTransferOperator::get_transfer_buffer(void *handle) {
    auto *trx = StorageIoTransfer::from_handle(handle);
    // IN 时 external_buf 指向 staging 或 fallback_data；OUT 阶段 external_buf 可能为空（CBW 用 fallback）
    return trx->external_buf ? trx->external_buf : trx->fallback_data.data();
}

std::size_t StorageTransferOperator::get_actual_length(void *handle) {
    return StorageIoTransfer::from_handle(handle)->actual_length;
}

std::size_t StorageTransferOperator::get_read_data_offset(void *) {
    // MSC 没有控制传输，偏移始终为 0
    return 0;
}

std::size_t StorageTransferOperator::get_write_data_offset(const UsbIpHeaderBasic &) {
    return 0;
}

UsbIpIsoPacketDescriptor StorageTransferOperator::get_iso_descriptor(void *, int) {
    // MSC 没有等时传输
    return {};
}

void StorageTransferOperator::set_iso_descriptor(void *, int, const UsbIpIsoPacketDescriptor &) {
}

void StorageTransferOperator::send_transfer_data(void *handle, asio::ip::tcp::socket &sock, std::size_t length,
                                                 std::error_code &ec) {
    auto *trx = StorageIoTransfer::from_handle(handle);
    auto *backend = handler_->get_backend();

    SPDLOG_DEBUG("STO::send handle={:p} len={} direct_io={} lba={} offset={} ext_buf={:p}",
                 static_cast<const void *>(handle), length, trx->direct_io, trx->file_lba, trx->file_offset,
                 static_cast<const void *>(trx->external_buf));

    // 仅 mmap READ 走零拷贝 sendfile/TransmitFile，CSW 等 fallback 数据不碰文件
    if (trx->direct_io && backend &&
        backend->send_direct(trx->file_lba, trx->file_offset, length, static_cast<intptr_t>(sock.native_handle()),
                             ec)) {
        SPDLOG_DEBUG("STO::send direct OK");
        return;
    }
    ec.clear();

    // 回退：从 external_buf / fallback_data 发送
    void *buf = trx->external_buf ? trx->external_buf : trx->fallback_data.data();
    SPDLOG_DEBUG("STO::send fallback buf={:p}", static_cast<const void *>(buf));
    asio::write(sock, asio::buffer(static_cast<const char *>(buf), length), ec);
}

void StorageTransferOperator::recv_transfer_data(void *handle, asio::ip::tcp::socket &sock, std::size_t length,
                                                 std::error_code &ec) {
    auto *trx = StorageIoTransfer::from_handle(handle);
    auto *backend = handler_->get_backend();

    SPDLOG_DEBUG("STO::recv handle={:p} len={} direct_io={} lba={} offset={} ext_buf={:p}",
                 static_cast<const void *>(handle), length, trx->direct_io, trx->file_lba, trx->file_offset,
                 static_cast<const void *>(trx->external_buf));

    // 仅 mmap WRITE 走零拷贝 splice，CBW/staging 数据不碰文件
    if (trx->direct_io && trx->external_buf && backend &&
        backend->recv_direct(trx->file_lba, trx->file_offset, length, static_cast<intptr_t>(sock.native_handle()),
                             ec)) {
        SPDLOG_DEBUG("STO::recv direct OK");
        handler_->on_out_data_received(trx, length);
        return;
    }
    ec.clear();

    // 回退：直读到 external_buf 或 fallback_data
    void *buf = trx->external_buf;
    if (buf) {
        SPDLOG_DEBUG("STO::recv fallback ext_buf={:p}", static_cast<const void *>(buf));
        asio::read(sock, asio::buffer(static_cast<std::uint8_t *>(buf), length), ec);
    }
    else {
        SPDLOG_DEBUG("STO::recv fallback resize={}", length);
        trx->fallback_data.resize(length);
        asio::read(sock, asio::buffer(trx->fallback_data), ec);
    }
    if (!ec) {
        handler_->on_out_data_received(trx, length);
    }
}
