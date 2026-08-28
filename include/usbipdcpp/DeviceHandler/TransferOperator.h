#pragma once

#include <cstddef>
#include <cstdint>
#include <system_error>

#include <asio.hpp>

#include "usbipdcpp/Export.h"
#include "usbipdcpp/protocol.h"
#include "usbipdcpp/type.h"

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

    virtual void *alloc_transfer_handle(std::size_t buffer_length, int num_iso_packets, const UsbIpHeaderBasic &header,
                                        const SetupPacket &setup_packet) = 0;
    virtual void free_transfer_handle(void *handle) = 0;

    virtual std::size_t get_actual_length(void *handle) = 0;

    virtual UsbIpIsoPacketDescriptor get_iso_descriptor(void *handle, int index) = 0;
    virtual void set_iso_descriptor(void *handle, int index, const UsbIpIsoPacketDescriptor &desc) = 0;

    /**
     * @brief 把数据填入 handle（IN 应答数据准备，如虚拟设备的挂起-应答）
     *
     * 由 InEndpointChannel 等组件的应答路径调用：缓冲里的数据经此方法写入
     * 私有 transfer（按 max_length 截断，实际填入量由 transfer 容量决定，
     * 一般等于 min(data.size(), max_length)），并更新 actual_length。
     * 调用者保证 handle 是本 operator 的 alloc_transfer_handle 创建的。
     *
     * 实现各自处理 transfer 内部结构（GenericTransfer 填 data vector，
     * libusb_transfer 填 buffer），组件不假设字段。
     *
     * 默认空实现返回 0：仅 libusb 等不参与挂起-应答的 op 使用（它们的数据
     * 路径不经过 InEndpointChannel）；需要真实填充的 op（如
     * GenericTransferOperator）必须 override
     *
     * @return 实际填入字节数（默认实现 0）
     */
    virtual std::size_t set_transfer_data(void *handle, const data_type &data, std::size_t max_length) {
        return 0;
    }

    /**
     * @brief 从 handle 读出已接收的数据，追加到 out 末尾（OUT 数据消费）
     *
     * 由 OutEndpointChannel 等组件的消费路径调用：挂起的 OUT 请求在业务侧
     * 取走时经此方法读出数据（追加写入，长度以实际接收为准）。
     * 调用者保证 handle 是本 operator 的 alloc_transfer_handle 创建的。
     *
     * 默认实现置 supported=false（不参与挂起-消费，仅 libusb 等 op 使用）；
     * 需要真实读取的 op（如 GenericTransferOperator）置 supported=true 并追加
     *
     * @param out 输出缓冲，数据追加到末尾
     * @param supported 出参：本 op 是否支持读取
     * @return 追加的字节数
     */
    virtual std::size_t get_transfer_data(void *handle, data_type &out, bool &supported) {
        supported = false;
        return 0;
    }

    /**
     * @brief 发送传输数据（IN 方向：server → client）
     *
     * 由 RET_SUBMIT::to_socket() 在写完 USBIP header 后调用。
     * 调用者保证 handle 是本 operator 的 alloc_transfer_handle 创建的。
     *
     * 实现必须严格按以下步骤操作，不得额外读写 sock，防止协议错位：
     *
     * 1. 若 length > 0，从私有 transfer 中发送 length 字节数据到 sock。
     *    IN 传输 length 为 actual_length，OUT 传输恒为 0，跳过此步。
     *
     * 2. 发送 N = alloc_transfer_handle 时传入的 num_iso_packets 个
     *    UsbIpIsoPacketDescriptor。在 for 循环中对每个描述符调用 to_bytes()
     *    得到网络字节序后写入 sock。
     *    非等时传输 N 为 0 或 0xFFFFFFFF，跳过此步。
     *
     * 所有 iso 数据包在线上紧凑排列，不得在描述符描述的字节区间之间插入空隙。
     *
     * 具体实现参考 GenericTransferOperator 和 LibusbTransferOperator。
     */
    virtual void send_transfer_data(void *handle, asio::ip::tcp::socket &sock, std::size_t length,
                                    std::error_code &ec) = 0;

    /**
     * @brief 接收传输数据（OUT 方向：client → server，含 IN 等时描述符）
     *
     * 由 CMD_SUBMIT::from_socket() 在读完 USBIP header 后调用。
     * 调用者保证 handle 是本 operator 的 alloc_transfer_handle 创建的。
     *
     * 实现必须严格按以下步骤操作，不得额外读写 sock，防止协议错位：
     *
     * 1. 若 length > 0，从 sock 读出 length 字节数据写入私有 transfer。
     *    OUT 传输 length 为 transfer_buffer_length，IN 传输恒为 0，跳过此步。
     *
     * 2. 继续从 sock 读出 N = alloc_transfer_handle 时传入的 num_iso_packets 个
     *    UsbIpIsoPacketDescriptor。在 for 循环中使用
     *    UsbIpIsoPacketDescriptor::from_socket(sock) 读取，
     *    并通过 set_iso_descriptor 写入私有 transfer 对应的 iso 包描述部分。
     *    非等时传输 N 为 0 或 0xFFFFFFFF，跳过此步。
     *
     * 所有 iso 数据包在线上紧凑排列，不得在描述符描述的字节区间之间插入空隙。
     *
     * 具体实现参考 GenericTransferOperator 和 LibusbTransferOperator。
     */
    virtual void recv_transfer_data(void *handle, asio::ip::tcp::socket &sock, std::size_t length,
                                    std::error_code &ec) = 0;

    /**
     * @brief 返回指定端点的 leaf TransferOperator
     *
     * 用于 from_socket 中的端点路由：路由层 op（如 VirtualDeviceTransferOperator）通过此方法
     * 按 ep 返回最终的 leaf op，然后 caller 直接在 leaf op 上 alloc / I/O，不再需要 map 查找。
     * 非路由层的 op 直接返回 this。
     */
    virtual TransferOperator *get_operator_for_ep(std::uint8_t ep) {
        return this;
    }
};

/**
 * @brief 基于 GenericTransfer 的默认传输操作器
 *
 * 与 AbstDeviceHandler 原有默认实现完全一致：创建 GenericTransfer，
 * 数据存储在 vector 中，send/recv 使用 asio 一次性读写。
 */
class USBIPDCPP_API GenericTransferOperator : public TransferOperator {
public:
    void *alloc_transfer_handle(std::size_t buffer_length, int num_iso_packets, const UsbIpHeaderBasic &header,
                                const SetupPacket &setup_packet) override;
    void free_transfer_handle(void *handle) override;

    std::size_t get_actual_length(void *handle) override;

    UsbIpIsoPacketDescriptor get_iso_descriptor(void *handle, int index) override;
    void set_iso_descriptor(void *handle, int index, const UsbIpIsoPacketDescriptor &desc) override;

    std::size_t set_transfer_data(void *handle, const data_type &data, std::size_t max_length) override;
    std::size_t get_transfer_data(void *handle, data_type &out, bool &supported) override;

    void send_transfer_data(void *handle, asio::ip::tcp::socket &sock, std::size_t length,
                            std::error_code &ec) override;
    void recv_transfer_data(void *handle, asio::ip::tcp::socket &sock, std::size_t length,
                            std::error_code &ec) override;
};

} // namespace usbipdcpp
