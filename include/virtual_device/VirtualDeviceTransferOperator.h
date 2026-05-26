#pragma once

#include <cstdint>
#include <unordered_map>

#include "DeviceHandler/TransferOperator.h"

namespace usbipdcpp {

/**
 * @brief 虚拟设备层传输操作器，按端点路由到接口级 TransferOperator
 *
 * 维护端点→操作器的注册表 + handle→操作器的映射表。
 * alloc_transfer_handle 按 header.ep 路由；get_transfer_buffer 等通过
 * handle→operator 映射表找到创建者并转发。
 */
class USBIPDCPP_API VirtualDeviceTransferOperator : public TransferOperator {
public:
    void register_endpoint_operator(std::uint8_t ep, TransferOperator* op) {
        ep_operators_[ep] = op;
    }

    // ========== TransferOperator 接口 ==========

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

    bool is_custom_io(void* handle) const override;

private:
    GenericTransferOperator generic_op_;
    std::unordered_map<std::uint8_t, TransferOperator*> ep_operators_;
    std::unordered_map<void*, TransferOperator*> handle_operators_;

    TransferOperator* find_op(void* handle) const;
};

} // namespace usbipdcpp
