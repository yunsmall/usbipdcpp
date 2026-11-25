#pragma once

#include <cstddef>
#include <span>
#include <system_error>
#include <vector>

#include <asio.hpp>

#include "usbipdcpp/Export.h"
#include "usbipdcpp/protocol.h"

namespace usbipdcpp {

/**
 * @brief 一次 CMD_SUBMIT 的线上字节源
 *
 * 把“这条命令的 payload 和等时描述符”与 socket 流解耦：调用方按自己的节奏
 * 读取（可以一次读满，也可以分块读给设备），框架只保证边界正确。
 *
 * 线上顺序固定为：payload（OUT 方向 transfer_buffer_length 字节，IN 方向 0）
 * 然后是 number_of_packets 个等时描述符。payload 必须全部读完之后才能读描述符；
 * 提前读描述符会把 payload 当成描述符解析，协议错位。
 *
 * 生命周期只在 AbstDeviceHandler::receive_urb 调用期间有效。返回前由 Session
 * 调 finish() 兜底：未读完的字节会被丢弃，保证下一条命令从正确位置开始。
 */
class USBIPDCPP_API TransferSource {
public:
    virtual ~TransferSource() = default;

    /// payload 剩余字节数（不含等时描述符）
    [[nodiscard]] virtual std::size_t remaining() const noexcept = 0;

    /// 还没读的等时描述符数量
    [[nodiscard]] virtual int iso_packets_remaining() const noexcept = 0;

    /**
     * @brief 读取至多 dst.size() 字节 payload（阻塞到读满该长度或读失败）
     * @param dst 目标缓冲
     * @param ec 错误码
     * @return 实际读取的字节数。dst.size() 大于 remaining() 时只读到
     *         remaining()，这不是错误；remaining() 为 0 时返回 0。读失败
     *         返回 0 并置 ec，remaining() 不变
     */
    virtual std::size_t read(std::span<std::byte> dst, std::error_code &ec) = 0;

    /**
     * @brief 读取一部分 payload（底层 read_some，可能少于 dst.size()）
     * @param dst 目标缓冲
     * @param ec 错误码
     * @return 实际读取的字节数；读失败返回 0 并置 ec，已读部分的 remaining()
     *         已同步扣减
     */
    virtual std::size_t read_some(std::span<std::byte> dst, std::error_code &ec) = 0;

    /**
     * @brief 读取等时包描述符
     * @param out 输出缓冲，最多读 out.size() 个
     * @param ec 错误码
     * @note 必须在 payload 全部读完后调用。构造时传入的
     *       num_iso_packets 由调用者保证非负（Session 已按协议校验）
     */
    virtual void read_iso_descriptors(std::span<UsbIpIsoPacketDescriptor> out, std::error_code &ec) = 0;

    /**
     * @brief 丢弃剩余 payload（失败/取消时保持 TCP 流对齐）
     * @param ec 错误码；读失败时剩余量不变
     */
    virtual void skip_remaining(std::error_code &ec) = 0;

    /**
     * @brief 取底层 socket（零拷贝后端用 socket 直接收发后手动记账）
     * @return socket 指针；内存实现返回 nullptr
     */
    [[nodiscard]] virtual asio::ip::tcp::socket *socket() noexcept = 0;

    /**
     * @brief 标记已经通过 socket 直接消费了 n 字节 payload（零拷贝路径用）
     *
     * 调用者负责确保这 n 字节确实已经从 socket() 读走；本函数只做记账，
     * 不会读写 socket。超过剩余量时按剩余量钳位（不会下溢）。只应在
     * socket() 非空的零拷贝实现里使用
     */
    virtual void mark_consumed(std::size_t n) noexcept = 0;

    /**
     * @brief 丢弃未读 payload，并读掉未读的等时描述符
     * @param ec 错误码
     * @note Session 在 receive_urb 返回后调用，保证下一条命令的边界正确
     */
    void drain(std::error_code &ec);
};

/**
 * @brief 基于 asio socket 的 TransferSource（服务端默认实现）
 */
class USBIPDCPP_API SocketTransferSource : public TransferSource {
public:
    /**
     * @param sock 已连接的 socket
     * @param payload_length 本次命令声明的 payload 长度
     * @param num_iso_packets 等时包描述符数量，调用者保证非负
     */
    SocketTransferSource(asio::ip::tcp::socket &sock, std::size_t payload_length, int num_iso_packets);

    [[nodiscard]] std::size_t remaining() const noexcept override { return payload_remaining_; }
    [[nodiscard]] int iso_packets_remaining() const noexcept override { return num_iso_packets_ - iso_read_; }

    std::size_t read(std::span<std::byte> dst, std::error_code &ec) override;
    std::size_t read_some(std::span<std::byte> dst, std::error_code &ec) override;
    void read_iso_descriptors(std::span<UsbIpIsoPacketDescriptor> out, std::error_code &ec) override;
    void skip_remaining(std::error_code &ec) override;
    [[nodiscard]] asio::ip::tcp::socket *socket() noexcept override { return &sock_; }
    void mark_consumed(std::size_t n) noexcept override;

private:
    asio::ip::tcp::socket &sock_;
    std::size_t payload_remaining_;
    int num_iso_packets_;
    int iso_read_ = 0;
};

} // namespace usbipdcpp
