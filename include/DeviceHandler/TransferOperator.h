#pragma once

#include <cstddef>
#include <cstdint>
#include <system_error>

#include <asio.hpp>

#include "Export.h"
#include "protocol.h"

namespace usbipdcpp {

/**
 * @brief 传输操作器抽象基类
 *
 * 封装 transfer_handle 的创建、读写、释放，供 AbstDeviceHandler 委托。
 * 不同场景各自实现：GenericTransferOperator（默认）、libusb、零拷贝存储等。
 */
class USBIPDCPP_API TransferOperator {
public:
    virtual ~TransferOperator() = default;

    virtual void* alloc_transfer_handle(std::size_t buffer_length, int num_iso_packets,
                                         const UsbIpHeaderBasic& header,
                                         const SetupPacket& setup_packet) = 0;
    virtual void free_transfer_handle(void* handle) = 0;

    virtual void* get_transfer_buffer(void* handle) = 0;
    virtual std::size_t get_actual_length(void* handle) = 0;
    virtual std::size_t get_read_data_offset(void* handle) = 0;
    virtual std::size_t get_write_data_offset(const UsbIpHeaderBasic& header) = 0;

    virtual UsbIpIsoPacketDescriptor get_iso_descriptor(void* handle, int index) = 0;
    virtual void set_iso_descriptor(void* handle, int index, const UsbIpIsoPacketDescriptor& desc) = 0;

    virtual void send_transfer_data(void* handle, asio::ip::tcp::socket& sock,
                                     std::size_t length, std::error_code& ec) = 0;
    virtual void recv_transfer_data(void* handle, asio::ip::tcp::socket& sock,
                                     std::size_t length, std::error_code& ec) = 0;

    virtual bool is_custom_io(void* handle) const = 0;
};

/**
 * @brief 基于 GenericTransfer 的默认传输操作器
 *
 * 与 AbstDeviceHandler 原有默认实现完全一致：创建 GenericTransfer，
 * 数据存储在 vector 中，send/recv 使用 asio 一次性读写。
 */
class USBIPDCPP_API GenericTransferOperator : public TransferOperator {
public:
    void* alloc_transfer_handle(std::size_t buffer_length, int num_iso_packets,
                                 const UsbIpHeaderBasic& header,
                                 const SetupPacket& setup_packet) override;
    void free_transfer_handle(void* handle) override;

    void* get_transfer_buffer(void* handle) override;
    std::size_t get_actual_length(void* handle) override;
    std::size_t get_read_data_offset(void* handle) override;
    std::size_t get_write_data_offset(const UsbIpHeaderBasic& header) override;

    UsbIpIsoPacketDescriptor get_iso_descriptor(void* handle, int index) override;
    void set_iso_descriptor(void* handle, int index, const UsbIpIsoPacketDescriptor& desc) override;

    void send_transfer_data(void* handle, asio::ip::tcp::socket& sock,
                            std::size_t length, std::error_code& ec) override;
    void recv_transfer_data(void* handle, asio::ip::tcp::socket& sock,
                            std::size_t length, std::error_code& ec) override;

    bool is_custom_io(void* handle) const override { return false; }
};

} // namespace usbipdcpp
