#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "usbipdcpp/SetupPacket.h"
#include "usbipdcpp/type.h"
#include "usbipdcpp/utils/RingBuffer.h"
#include "usbipdcpp/virtual_device/SimpleVirtualDeviceHandler.h"
#include "usbipdcpp/virtual_device/VirtualInterfaceHandler.h"

namespace usbipdcpp {

/// read() 返回的一次传输（对齐 VirtualUSBDevice::Xfer / FunctionFS SETUP 事件）
struct PipeXfer {
    std::uint8_t ep = 0;                  // 端点地址（含方向位）；控制请求时为 0
    std::optional<SetupPacket> setup_req; // 仅控制请求（ep==0）时有效
    data_type data;                       // OUT 数据或控制请求数据
};

/**
 * @brief 通用管道设备 handler：接口内所有端点自动管道化（标准请求自动处理），
 * 数据面提供阻塞 read/write（对齐内核 FunctionFS 的 FIFO 语义）：
 * - read() 阻塞等待一个 OUT 传输或非标准控制请求（class/vendor setup）
 * - write() 把数据写入指定 IN 端点的 FIFO，FIFO 满时阻塞等待宿主取走
 *
 * 用法：照 mock_keyboard 方式组 UsbDevice 描述符，with_handler 绑定本类，
 * setup_interface_handlers 后业务线程直接 read/write。
 */
class USBIPDCPP_API PipeDeviceHandler : public SimpleVirtualDeviceHandler {
public:
    PipeDeviceHandler(UsbDevice &handle_device, StringPool &string_pool);
    // 构造时自动给每个接口绑定内部管道接口 handler

    // ===== 数据面 API（阻塞语义对齐内核 FIFO，read/write 对称走 PipeXfer）=====

    /**
     * @brief 阻塞读：等待一个 OUT 传输或非标准控制请求。timeout_ms=0 无限等。
     * @param xfer 出参，有数据时写入（控制请求带 setup_req）
     * @param timeout_ms 超时毫秒，0 表示无限等待
     * @return true 有数据；false 超时或断连
     */
    bool read(PipeXfer &xfer, std::uint32_t timeout_ms = 0);

    /**
     * @brief 阻塞写：把 xfer.data 发到 xfer.ep（IN 端点地址含方向位；ep==0 时作为
     * 控制请求应答）。数据入该端点 FIFO，FIFO 满时等待宿主取走（timeout_ms=0 无限等）。
     * @param xfer 待发送的传输（ep 指定目标端点，data 为负载）
     * @param timeout_ms 超时毫秒，0 表示无限等待
     * @return 实际写入字节数；超时可能小于 data 大小；断连返回 0
     */
    std::size_t write(const PipeXfer &xfer, std::uint32_t timeout_ms = 0);

    /**
     * @brief 设置每个 IN 端点的 FIFO 容量（默认 64KB），必须在连接前调用
     */
    void set_in_fifo_capacity(std::size_t capacity);

    /**
     * @brief 设置所有接口共享的 class-specific 描述符（追加在配置描述符的
     * 接口描述符之后）。通用管道本身没有类描述符，需要特定 USB 类的设备
     * 必须设置（如 HID 设备的 HID 描述符 0x21，否则驱动无法加载）。
     * 必须在连接前调用
     */
    void set_class_specific_descriptor(data_type descriptor);

    /**
     * @brief 设置自定义描述符：GET_DESCRIPTOR 控制请求（recipient=接口、
     * 标准请求）按 type 返回对应数据。HID 设备必须设置 type=0x22 的报告
     * 描述符。必须在连接前调用
     */
    void set_custom_descriptor(std::uint8_t type, data_type descriptor);

    // ========== 连接生命周期 ==========

    void on_new_connection(Session &current_session, error_code &ec) override;
    void on_disconnection(error_code &ec) override;

private:
    friend class PipeInterfaceHandler;

    // 接口 handler 转发入口（session receiver 线程调用）
    void on_pipe_in_request(std::uint8_t ep_addr, std::uint32_t seqnum, std::uint32_t length,
                            TransferHandle transfer);
    void on_pipe_out_transfer(std::uint8_t ep_addr, std::uint32_t seqnum, std::uint32_t received_size,
                              data_type &&data);
    void on_pipe_control_request(const SetupPacket &setup, std::uint32_t seqnum,
                                 std::uint32_t transfer_buffer_length, TransferHandle transfer);
    void on_pipe_unlink(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum);

    // 调用者必须已持有 pipe_mutex 和 requests_mutex
    void try_send_pending_locked();
    void send_from_fifo_locked(RingBuffer &fifo, std::uint32_t seqnum, std::uint32_t max_length,
                               TransferHandle transfer);

    // 保护 out_queue / in_fifos / disconnected
    mutable std::mutex pipe_mutex;
    std::condition_variable read_cv;  // out_queue 非空或断连
    std::condition_variable write_cv; // in_fifo 腾出空间或断连
    std::deque<PipeXfer> out_queue;
    // 每端点 IN FIFO，键 = 端点地址（含方向位）；控制应答（ep==0）也走这里
    std::unordered_map<std::uint8_t, RingBuffer> in_fifos;
    bool disconnected = true;

    // 保护 endpoint_requests
    mutable std::mutex requests_mutex;
    EndpointRequestQueue endpoint_requests; // 挂起的 IN 传输请求（含控制）

    std::size_t fifo_capacity = 64 * 1024;

    // 自定义描述符（连接前设置，运行时只读，无需加锁）
    data_type class_specific_descriptor;
    std::unordered_map<std::uint8_t, data_type> custom_descriptors;
};

} // namespace usbipdcpp
