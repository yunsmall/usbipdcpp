#pragma once

#include "DeviceHandler/TransferOperator.h"
#include "utils/ObjectPool.h"

struct libusb_transfer;

namespace usbipdcpp {

namespace detail {
    struct LibusbTransferLM {
        static libusb_transfer *create();
        static void destroy(libusb_transfer *p);
    };
    struct LibusbTransferReset {
        static void reset(libusb_transfer &t);
    };
} // namespace detail

/**
 * @brief libusb 后端的传输操作器，handle 为 libusb_transfer*
 */
class USBIPDCPP_API LibusbTransferOperator : public TransferOperator {
public:
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
    // 非同步传输对象池（num_iso_packets == 0），同步传输直接走 libusb_alloc_transfer
    ObjectPool<libusb_transfer, 64, true, detail::LibusbTransferLM, detail::LibusbTransferReset> transfer_pool_;
};

} // namespace usbipdcpp
