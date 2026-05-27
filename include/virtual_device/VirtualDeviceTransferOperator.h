#pragma once

#include <cstdint>
#include <unordered_map>

#include "DeviceHandler/TransferOperator.h"

namespace usbipdcpp {

/**
 * @brief 虚拟设备层传输操作器，按端点路由到接口级 TransferOperator
 *
 * 维护端点→操作器的注册表（ep_operators_），alloc_transfer_handle 按 header.ep 路由。
 * 路由后 caller 将 leaf op 存入 TransferHandle，后续 I/O 操作直接调 leaf op，
 * 不再经过本类的 map 查找，因此无需 handle→operator 映射和锁。
 */
class USBIPDCPP_API VirtualDeviceTransferOperator : public TransferOperator {
public:
    /**
     * @brief 注册端点→操作器的映射
     * @param ep 端点地址（如 0x02 表示 OUT, 0x81 表示 IN）
     * @param op 接口级 TransferOperator（如 StorageTransferOperator）
     */
    void register_endpoint_operator(std::uint8_t ep, TransferOperator *op) {
        ep_operators_[ep] = op;
    }

    /**
     * @brief 返回 ep 对应的 leaf TransferOperator
     *
     * from_socket 通过此方法获取 leaf op 后直接在其上操作，
     * 不再需要 handle→operator 映射。
     */
    TransferOperator *get_operator_for_ep(std::uint8_t ep) override;

    // ========== TransferOperator 接口 ==========

    void *alloc_transfer_handle(std::size_t buffer_length, int num_iso_packets, const UsbIpHeaderBasic &header,
                                const SetupPacket &setup_packet) override;
    void free_transfer_handle(void *handle) override;

    void *get_transfer_buffer(void *handle) override;
    std::size_t get_actual_length(void *handle) override;
    std::size_t get_read_data_offset(void *handle) override;
    std::size_t get_write_data_offset(const UsbIpHeaderBasic &header) override;

    UsbIpIsoPacketDescriptor get_iso_descriptor(void *handle, int index) override;
    void set_iso_descriptor(void *handle, int index, const UsbIpIsoPacketDescriptor &desc) override;

    void send_transfer_data(void *handle, asio::ip::tcp::socket &sock, std::size_t length,
                            std::error_code &ec) override;
    void recv_transfer_data(void *handle, asio::ip::tcp::socket &sock, std::size_t length,
                            std::error_code &ec) override;

    bool is_custom_io(void *handle) const override;

private:
    GenericTransferOperator generic_op_;
    std::unordered_map<std::uint8_t, TransferOperator *> ep_operators_;
};

} // namespace usbipdcpp
