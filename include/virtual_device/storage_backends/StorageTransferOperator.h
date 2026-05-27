#pragma once

#include "DeviceHandler/TransferOperator.h"
#include "utils/ObjectPool.h"
#include "virtual_device/storage_backends/StorageIoTransfer.h"

namespace usbipdcpp {

class MscBulkOnlyHandler;

/**
 * @brief MSC 零拷贝传输操作器
 *
 * IN：send_transfer_data 从 external_buf（mmap）直接发送
 * OUT：recv_transfer_data 直读入 CBW/staging 并解析
 */
class StorageTransferOperator : public TransferOperator {
public:
    explicit StorageTransferOperator(MscBulkOnlyHandler *handler);

    void *alloc_transfer_handle(std::size_t buffer_length, int num_iso_packets, const UsbIpHeaderBasic &header,
                                const SetupPacket &setup_packet) override;
    void free_transfer_handle(void *handle) override;

    std::size_t get_actual_length(void *handle) override;

    UsbIpIsoPacketDescriptor get_iso_descriptor(void *handle, int index) override;
    void set_iso_descriptor(void *handle, int index, const UsbIpIsoPacketDescriptor &desc) override;

    void send_transfer_data(void *handle, asio::ip::tcp::socket &sock, std::size_t length,
                            std::error_code &ec) override;
    void recv_transfer_data(void *handle, asio::ip::tcp::socket &sock, std::size_t length,
                            std::error_code &ec) override;

private:
    MscBulkOnlyHandler *handler_;
    /// BOT 最多 2-3 个传输在途，8 个槽足够
    ObjectPool<StorageIoTransfer, 8> pool_;
};

} // namespace usbipdcpp
