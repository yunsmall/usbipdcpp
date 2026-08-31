#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "usbipdcpp/SetupPacket.h"
#include "usbipdcpp/type.h"
#include "usbipdcpp/virtual_device/InEndpointChannel.h"
#include "usbipdcpp/virtual_device/OutEndpointChannel.h"
#include "usbipdcpp/virtual_device/SimpleVirtualDeviceHandler.h"
#include "usbipdcpp/virtual_device/VirtualInterfaceHandler.h"

namespace usbipdcpp {

class PipeDeviceHandler;

/// read() 返回的一次传输（对齐 VirtualUSBDevice::Xfer / FunctionFS SETUP 事件）
struct PipeXfer {
    std::uint8_t ep = 0;                  // 端点地址（含方向位）；控制请求时为 0
    std::optional<SetupPacket> setup_req; // 仅控制请求（ep==0）时有效
    data_type data;                       // OUT 数据或控制请求数据
};

/// 标准请求行为回调（接口/端点 recipient 的标准控制请求解析后转发到这里，
/// 通过 *p_status 应答（0=成功），需要时可访问 pipe 的公开 API。
/// 不设置的回调使用默认行为：接受并回成功（SET_INTERFACE 只接受接口定义里
/// 存在的 alt，其余回 EPIPE）。回调在 session receiver 线程调用，必须在
/// 连接前设置，连接后不要修改
struct PipeStandardRequestHandler {
    std::function<void(PipeDeviceHandler &pipe, std::uint16_t feature_selector, std::uint32_t *p_status)>
            clear_feature;
    std::function<void(PipeDeviceHandler &pipe, std::uint16_t feature_selector, std::uint8_t ep_address,
                       std::uint32_t *p_status)>
            endpoint_clear_feature;
    std::function<std::uint8_t(PipeDeviceHandler &pipe, std::uint32_t *p_status)> get_interface;
    std::function<void(PipeDeviceHandler &pipe, std::uint16_t alternate_setting, std::uint32_t *p_status)>
            set_interface;
    std::function<std::uint16_t(PipeDeviceHandler &pipe, std::uint32_t *p_status)> get_status;
    std::function<std::uint16_t(PipeDeviceHandler &pipe, std::uint8_t ep_address, std::uint32_t *p_status)>
            endpoint_get_status;
    std::function<void(PipeDeviceHandler &pipe, std::uint16_t feature_selector, std::uint32_t *p_status)>
            set_feature;
    std::function<void(PipeDeviceHandler &pipe, std::uint16_t feature_selector, std::uint8_t ep_address,
                       std::uint32_t *p_status)>
            endpoint_set_feature;
};

/**
 * @brief 通用管道设备 handler：接口内所有端点自动管道化（标准请求自动处理），
 * 数据面提供阻塞 read/write（对齐内核 FunctionFS 的 FIFO 语义）：
 * - read() 阻塞等待一个 OUT 传输或非标准控制请求（class/vendor setup）
 * - write() 把数据写入指定 IN 端点的 FIFO，FIFO 满时阻塞等待宿主取走
 *
 * 本类是为匹配内核 gadget 模式（FunctionFS）专门创建的：业务层像读写文件
 * 一样操作设备，标准请求由框架消化、行为可通过 set_standard_request_handler
 * 回调调整，非标准请求经 read() 透出。但通用管道模式难以覆盖全部自定义
 * 写法（每端点的独立语义、复杂类协议的协商等），需要实现特定 USB 类或
 * 复杂控制协议的设备，建议改用继承 VirtualInterfaceHandler 的方式实现
 * （见 docs/custom-device.md 方法二）。
 *
 * 用法：照 mock_keyboard 方式组 UsbDevice 描述符，with_handler 绑定本类，
 * setup_interface_handlers 后业务线程直接 read/write。
 */
class USBIPDCPP_API PipeDeviceHandler final : public SimpleVirtualDeviceHandler {
public:
    PipeDeviceHandler(UsbDevice &handle_device, StringPool &string_pool);
    // 内部管道接口 handler 的绑定在 setup_interface_handlers 里完成（见其声明）

    // ===== 标准请求行为配置 =====

    /**
     * @brief 设置接口/端点 recipient 的标准请求行为回调
     * @param handler 回调结构体（只设置需要改行为的成员，未设置的保持默认
     * 行为）。回调在 session receiver 线程调用，必须在连接前设置，连接后
     * 不要修改
     */
    void set_standard_request_handler(PipeStandardRequestHandler handler) {
        standard_request_handler = std::move(handler);
    }

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

    /// 接口 handler 需要持有本对象指针，只能在对象完整构造后创建，故在 setup 阶段补建
    void setup_interface_handlers() override;

    void on_new_connection(TransferResponder &current_session, error_code &ec) override;
    void on_disconnection(error_code &ec) override;

private:
    friend class PipeInterfaceHandler;

    // 接口 handler 转发入口（session receiver 线程调用）
    void on_pipe_in_request(std::uint8_t ep_addr, std::uint32_t seqnum, std::uint32_t length,
                            TransferHandle transfer);
    void on_pipe_out_transfer(std::uint8_t ep_addr, std::uint32_t seqnum, TransferHandle transfer);
    void on_pipe_control_request(const SetupPacket &setup, std::uint32_t seqnum,
                                 std::uint32_t transfer_buffer_length, TransferHandle transfer);
    void on_pipe_unlink(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum);

    /// 获取端点 IN 通道（懒创建：首次访问时按 fifo_capacity 建通道并绑定会话）。
    /// 调用者必须已持有 pipe_mutex
    ByteStreamInChannel &get_in_channel(std::uint8_t ep_addr);

    /// 获取端点 OUT 通道（懒创建：首次访问时绑定会话并重置断连状态）。
    /// 调用者必须已持有 pipe_mutex
    OutEndpointChannel &get_out_channel(std::uint8_t ep_addr);

    // 保护 out_channels / in_channels / disconnected
    mutable std::mutex pipe_mutex;
    // 任一 OUT 通道非空或断连（read 的等待条件）；入队/断连时 notify
    std::condition_variable out_cv;
    // 每端点 OUT 通道（NAK 背压），键 = 端点地址（含方向位）；控制请求
    // （ep==0）也走这里。挂起的请求由 read() 取走时应答
    std::map<std::uint8_t, OutEndpointChannel> out_channels;
    // 每端点 IN 通道（字节流模式），键 = 端点地址（含方向位）；控制应答
    // （ep==0）也走这里。挂起-应答/阻塞写在通道内部
    std::unordered_map<std::uint8_t, ByteStreamInChannel> in_channels;
    bool disconnected = true;

    std::size_t fifo_capacity = 64 * 1024;

    // 自定义描述符（连接前设置，运行时只读，无需加锁）
    data_type class_specific_descriptor;
    std::unordered_map<std::uint8_t, data_type> custom_descriptors;

    // 标准请求行为回调（连接前设置，运行时只读，无需加锁）
    PipeStandardRequestHandler standard_request_handler;
};

} // namespace usbipdcpp
