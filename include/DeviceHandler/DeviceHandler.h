#pragma once

#include <vector>
#include <cstdint>
#include <optional>
#include <system_error>
#include <mutex>
#include <spdlog/spdlog.h>

#include <asio.hpp>

#include "Device.h"
#include "type.h"
#include "protocol.h"


namespace usbipdcpp {
struct UsbEndpoint;
struct UsbInterface;
struct SetupPacket;

class Session;

/**
 * @brief USB 设备处理抽象基类。
 *
 * 每个导入的设备对应一个 DeviceHandler 实例，负责处理协议命令（CMD_SUBMIT、
 * CMD_UNLINK）并生成响应（RET_SUBMIT、RET_UNLINK）。传输数据的读写通过
 * transfer_handle 接口完成，to_socket / from_socket 通过这套接口操作数据。
 *
 * 默认模式下传输数据在内存中连续存放，to_socket / from_socket 一次 scatter-gather
 * 发送/接收。需实现以下 transfer_handle 接口：
 *   - alloc_transfer_handle / free_transfer_handle
 *   - get_transfer_buffer / get_actual_length
 *   - get_read_data_offset / get_write_data_offset
 *   - get_iso_descriptor / set_iso_descriptor
 * 基类提供基于 GenericTransfer 的默认实现，虚拟设备后端可直接复用。
 * libusb 后端重写全套以对接 libusb_transfer。
 *
 * 若设备端内存受限无法分配连续大缓冲区，设 custom_transfer_io = true 并实现：
 *   - alloc_transfer_handle / free_transfer_handle
 *   - send_transfer_data / recv_transfer_data
 * to_socket / from_socket 会调用这两个函数逐块发送/接收，不再走 scatter-gather。
 * 其余 get_* / set_* / iso_* 函数在自定义路径下不会被调用，无需实现。
 */
class USBIPDCPP_API AbstDeviceHandler {
public:
    explicit AbstDeviceHandler(UsbDevice &handle_device) :
        handle_device(handle_device) {
    }

    AbstDeviceHandler(AbstDeviceHandler &&other) noexcept;

    /**
     * @brief 处理 URB 请求的统一入口
     * @param cmd 完整的 CMD_SUBMIT 命令
     * @param ep 端点信息
     * @param interface 可选的接口信息（非控制传输必须有）
     * @param ec 错误码
     */
    virtual void receive_urb(
            UsbIpCommand::UsbIpCmdSubmit cmd,
            UsbEndpoint ep,
            std::optional<UsbInterface> interface,
            usbipdcpp::error_code &ec
            ) = 0;

    /**
     * @brief 新的客户端连接时会调这个函数，可以阻塞。子类实现时请在函数开头调用这个函数
     * @param current_session 请自行储存通信用的session
     * @param ec 发生的ec
     */
    virtual void on_new_connection(Session &current_session, error_code &ec) {
        std::lock_guard lock(session_mutex_);
        session = &current_session;
    }

    /**
     * @brief 当发生错误等情况需要完全终止传输时会调用这个函数。被调用后禁止再提交消息和使用Session对象\n
     * 可以阻塞，处理所有需要处理的事务。子类实现时请在函数末尾调用这个函数
     */
    virtual void on_disconnection(error_code &ec) {
        std::lock_guard lock(session_mutex_);
        session = nullptr;
    }

    /**
     * @brief 检查设备是否已被移除
     * @return true 表示设备已物理拔出
     */
    virtual bool is_device_removed() const {
        return false;  // 默认实现
    }

    /**
     * @brief 设备被物理移除时调用
     */
    virtual void on_device_removed() {}

    /**
     * @brief 线程安全地停止 Session
     */
    void trigger_session_stop();

    /**
     * @brief 处理 USBIP_CMD_UNLINK（客户端要求取消一个未完成的传输）。
     *
     * 协议约定：
     * - 每个 CMD_UNLINK 必须对应一个 RET_UNLINK，status 为 USBIP 错误码。
     * - 如果目标传输已经正常完成，RET_UNLINK 的 status 必须为 0（URB 已完成）。
     * - RET_SUBMIT 必须在 RET_UNLINK 之前发送。客户端先收到 URB 的实际结果，
     *   再收到 unlink 的确认，不能反过来。如果实现中同一个传输可能既发 RET_SUBMIT
     *   又发 RET_UNLINK，必须保证 RET_SUBMIT 先于 RET_UNLINK 到达客户端。
     *
     * @param unlink_seqnum 要取消的 CMD_SUBMIT 的 seqnum
     * @param cmd_seqnum    CMD_UNLINK 自己的 seqnum（用在 RET_UNLINK 里原样返回）
     *
     * 目标传输在 unlink 到达时可能处于以下几种状态，实现需要分别处理：
     *
     * 状态 1 —— 传输尚未开始或仍在进行中：
     *   取消该传输。如果取消成功，传输完成时发送 RET_UNLINK(cmd_seqnum, -ECONNRESET)
     *   （表示 URB 被取消），并且不再发送 RET_SUBMIT。
     *
     * 状态 2 —— 传输已经完成，但 RET_SUBMIT 还未发送：
     *   发送 RET_UNLINK(cmd_seqnum, 实际完成状态码)，如无错误则为 0。
     *   不再发送 RET_SUBMIT。
     *
     * 状态 3 —— RET_SUBMIT 已经发送：
     *   直接发送 RET_UNLINK(cmd_seqnum, 0)。RET_SUBMIT 已先行发送，顺序正确。
     *
     * 状态 4 —— 传输已完全处理完毕（RET_SUBMIT 及可能的 RET_UNLINK 均已发出）：
     *   直接发送 RET_UNLINK(cmd_seqnum, 0)。
     */
    virtual void handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) = 0;

    // ========== transfer_handle 操作接口 ==========

    /**
     * @brief 创建 transfer_handle
     * @param buffer_length 数据缓冲区长度
     * @param num_iso_packets 等时包数量
     * @param header
     * @param setup_packet
     * @return 创建的 transfer_handle
     */
    virtual void* alloc_transfer_handle(std::size_t buffer_length, int num_iso_packets, const UsbIpHeaderBasic& header, const SetupPacket& setup_packet);

    /**
     * @brief 获取 buffer 头指针
     * @param transfer_handle
     * @return buffer 头指针
     */
    virtual void* get_transfer_buffer(void* transfer_handle);

    /**
     * @brief 获取实际传输长度
     * @param transfer_handle
     * @return 实际长度
     */
    virtual std::size_t get_actual_length(void* transfer_handle);

    /**
     * @brief 获取读取数据时的偏移量
     * 用于 RET_SUBMIT 响应，控制传输需要跳过 setup 包（8字节）
     * @param transfer_handle
     * @return 偏移量
     */
    virtual std::size_t get_read_data_offset(void* transfer_handle);

    /**
     * @brief 获取写入数据时的偏移量
     * 用于 alloc_transfer_handle 时计算 buffer 位置，控制传输需要跳过 setup 包
     * @param header 用于判断是否为控制传输
     * @return 偏移量
     */
    virtual std::size_t get_write_data_offset(const UsbIpHeaderBasic& header);

    /**
     * @brief 获取等时包描述符
     * @param transfer_handle
     * @param index 索引
     * @return 描述符
     */
    virtual UsbIpIsoPacketDescriptor get_iso_descriptor(void* transfer_handle, int index);

    /**
     * @brief 设置等时包描述符
     * @param transfer_handle
     * @param index 索引
     * @param desc 描述符
     */
    virtual void set_iso_descriptor(void* transfer_handle, int index, const UsbIpIsoPacketDescriptor& desc);

    /**
     * @brief 释放 transfer_handle
     * @param transfer_handle
     */
    virtual void free_transfer_handle(void* transfer_handle);

    /**
     * @brief 设为 true 表示 handler 需要分块读写 transfer 数据。
     *
     * 适用场景：嵌入式设备内存受限，无法分配与主机 transfer_buffer_length 等长的连续缓冲区。
     * 置 true 后 to_socket / from_socket 会调用 send_transfer_data / recv_transfer_data，
     * 而非 get_transfer_buffer + 一次性 asio::write/read。
     *
     * 默认 false，走原连续内存路径，零虚函数开销。
     */
    bool custom_transfer_io = false;

    /**
     * @brief 自定义发送（custom_transfer_io == true 时由 UsbIpRetSubmit::to_socket 调用）
     *
     * 默认实现将 get_transfer_buffer 指向的 buffer 从头开始发送 length 字节。
     * 若需要跳过 setup 包头等偏移，重写时通过 get_read_data_offset 自行处理。
     *
     * @param handle transfer_handle
     * @param sock 目标 socket
     * @param length 需要发送的总字节数
     * @param ec 错误码
     */
    virtual void send_transfer_data(void* handle, asio::ip::tcp::socket& sock,
                                     std::size_t length, std::error_code& ec);

    /**
     * @brief 自定义接收（custom_transfer_io == true 时由 UsbIpCmdSubmit::from_socket 调用）
     *
     * 默认实现从 socket 读取 length 字节到 get_transfer_buffer 指向的 buffer 开头。
     * 若需要跳过 setup 包头等偏移，重写时通过 get_write_data_offset 自行处理。
     *
     * @param handle transfer_handle
     * @param sock 来源 socket
     * @param length 需要接收的总字节数
     * @param ec 错误码
     */
    virtual void recv_transfer_data(void* handle, asio::ip::tcp::socket& sock,
                                     std::size_t length, std::error_code& ec);

    virtual ~AbstDeviceHandler() = default;

protected:
    UsbDevice &handle_device;
    Session *session = nullptr;
    mutable std::mutex session_mutex_;
};
}
