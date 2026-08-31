#pragma once

#include <array>
#include <string_view>
#include <vector>
#include "usbipdcpp/virtual_device/CdcAcmConstants.h"
#include "usbipdcpp/SetupPacket.h"
#include "usbipdcpp/constant.h"
#include "usbipdcpp/protocol.h"
#include "usbipdcpp/virtual_device/InEndpointChannel.h"
#include "usbipdcpp/virtual_device/OutEndpointChannel.h"
#include "usbipdcpp/virtual_device/VirtualInterfaceHandler.h"

namespace usbipdcpp {
/**
 * @brief CDC ACM 线路编码结构
 */
struct LineCoding {
    std::uint32_t dwDTERate = 115200; // 波特率
    std::uint8_t bCharFormat = 0; // 停止位: 0=1位, 1=1.5位, 2=2位
    std::uint8_t bParityType = 0; // 校验: 0=无, 1=奇, 2=偶, 3=标记, 4=空格
    std::uint8_t bDataBits = 8; // 数据位: 5, 6, 7, 8, 16

    [[nodiscard]] std::array<std::uint8_t, 7> to_bytes() const {
        return {{static_cast<std::uint8_t>(dwDTERate & 0xFF), static_cast<std::uint8_t>((dwDTERate >> 8) & 0xFF),
                 static_cast<std::uint8_t>((dwDTERate >> 16) & 0xFF),
                 static_cast<std::uint8_t>((dwDTERate >> 24) & 0xFF), bCharFormat, bParityType, bDataBits}};
    }

    static LineCoding from_bytes(const std::vector<std::uint8_t> &data) {
        LineCoding coding{};
        if (data.size() >= 7) {
            coding.dwDTERate = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
            coding.bCharFormat = data[4];
            coding.bParityType = data[5];
            coding.bDataBits = data[6];
        }
        return coding;
    }
};

/**
 * @brief CDC ACM 控制信号状态
 */
struct ControlSignalState {
    bool dtr = false; // Data Terminal Ready
    bool rts = false; // Request To Send

    [[nodiscard]] std::uint16_t to_uint16() const {
        std::uint16_t value = 0;
        if (dtr)
            value |= static_cast<std::uint16_t>(CdcAcmControlSignal::DTR);
        if (rts)
            value |= static_cast<std::uint16_t>(CdcAcmControlSignal::RTS);
        return value;
    }

    static ControlSignalState from_uint16(std::uint16_t value) {
        ControlSignalState state;
        state.dtr = (value & static_cast<std::uint16_t>(CdcAcmControlSignal::DTR)) != 0;
        state.rts = (value & static_cast<std::uint16_t>(CdcAcmControlSignal::RTS)) != 0;
        return state;
    }
};

/**
 * @brief CDC ACM 串口状态通知
 */
struct SerialStateNotification {
    std::uint8_t bmRequestType = 0xA1; // 类特定、接口、IN
    std::uint8_t bNotification = 0x20; // SERIAL_STATE
    std::uint16_t wValue = 0;
    std::uint16_t wIndex = 0; // 接口号
    std::uint16_t wLength = 2;
    std::uint16_t data = 0; // 状态位

    [[nodiscard]] std::vector<std::uint8_t> to_bytes() const {
        std::vector<std::uint8_t> result;
        result.push_back(bmRequestType);
        result.push_back(bNotification);
        result.push_back(wValue & 0xFF);
        result.push_back((wValue >> 8) & 0xFF);
        result.push_back(wIndex & 0xFF);
        result.push_back((wIndex >> 8) & 0xFF);
        result.push_back(wLength & 0xFF);
        result.push_back((wLength >> 8) & 0xFF);
        result.push_back(data & 0xFF);
        result.push_back((data >> 8) & 0xFF);
        return result;
    }
};

// 前向声明
class CdcAcmDataInterfaceHandler;

/**
 * @brief CDC ACM 通信接口处理器（处理控制请求和状态通知）
 *
 * 用于处理 CDC ACM 设备的通信接口，响应控制请求并发送状态通知。
 */
class USBIPDCPP_API CdcAcmCommunicationInterfaceHandler : public VirtualInterfaceHandler {
public:
    CdcAcmCommunicationInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool);

    /**
     * @brief 创建 CDC ACM 通信接口（描述符模板，未绑定 handler）
     *
     * 接口定义：CDC 类、ACM 子类、AT 命令协议（02/02/01），一个中断 IN 端点
     * （用于串口状态通知）。
     * @param interrupt_in_ep 中断 IN 端点地址（设备→主机，状态通知）
     * @return 未绑定 handler 的 UsbInterface
     */
    static UsbInterface make_interface(std::uint8_t interrupt_in_ep) {
        UsbInterface i{
                .interface_class = static_cast<std::uint8_t>(ClassCode::CDC),
                .interface_subclass = 0x02, // ACM
                .interface_protocol = 0x01, // AT Commands (v25ter)
                .endpoints = {{UsbEndpoint{.address = interrupt_in_ep,
                                           .attributes = static_cast<std::uint8_t>(EndpointAttributes::Interrupt),
                                           .max_packet_size = 64,
                                           .interval = 16}}},
        };
        return i;
    }

    // ========== 内部实现（子类无需关心） ==========

    void handle_non_standard_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                      std::uint32_t transfer_flags,
                                                      std::uint32_t transfer_buffer_length,
                                                      const SetupPacket &setup_packet, TransferHandle transfer,
                                                      std::error_code &ec) override;

    void handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                                   std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                   std::error_code &ec) override;

    [[nodiscard]] data_type get_class_specific_descriptor() override;

    // ========== 子类可选重写的回调 ==========

    /**
     * @brief 主机设置线路编码时回调
     * @param coding 新的线路编码参数
     */
    virtual void on_set_line_coding(const LineCoding &coding);

    /**
     * @brief 主机设置控制信号状态时回调
     * @param state 控制信号状态（DTR、RTS）
     */
    virtual void on_set_control_line_state(const ControlSignalState &state);

    /**
     * @brief 主机请求发送中断时回调
     * @param duration 中断持续时间
     */
    virtual void on_send_break(std::uint16_t duration);

    /**
     * @brief 处理非 CDC ACM 类请求的控制传输，子类可重写以扩展功能
     */
    virtual void handle_non_cdc_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                         std::uint32_t transfer_flags,
                                                         std::uint32_t transfer_buffer_length,
                                                         const SetupPacket &setup_packet, TransferHandle transfer,
                                                         std::error_code &ec);

    // ========== 状态查询 API ==========

    /**
     * @brief 获取当前线路编码
     */
    [[nodiscard]] const LineCoding &get_line_coding() const {
        return line_coding_;
    }

    /**
     * @brief 获取当前控制信号状态
     */
    [[nodiscard]] const ControlSignalState &get_control_signal_state() const {
        return control_signal_state_;
    }

    // ========== 发送通知 API ==========

    /**
     * @brief 发送串口状态通知到主机
     * @param state_bits 状态位（如 CTS、DSR 等）
     */
    void send_serial_state_notification(std::uint16_t state_bits);

    // ========== 接口关联 API ==========

    /**
     * @brief 关联数据接口处理器
     */
    void set_data_handler(CdcAcmDataInterfaceHandler *handler) {
        data_handler_ = handler;
    }

    /**
     * @brief 获取关联的数据接口处理器
     */
    CdcAcmDataInterfaceHandler *get_data_handler() const {
        return data_handler_;
    }

    // ========== 内部实现（子类无需关心） ==========

    void on_new_connection(TransferResponder &current_session, std::error_code &ec) override;

    void on_disconnection(std::error_code &ec) override;

    void handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) override;

protected:
    LineCoding line_coding_;
    ControlSignalState control_signal_state_;

    /**
     * @brief 关联的数据接口处理器
     */
    CdcAcmDataInterfaceHandler *data_handler_ = nullptr;

    /// 串口状态通知通道（中断 IN 端点）：一条通知 = 一个完整消息。有挂起请求
    /// 直接应答，否则入缓冲（上限 1 条：只保留最新状态，对齐原 pending_notification_
    /// 被新通知覆盖的语义；状态变化低频，丢旧保新即可）
    MessageInChannel notification_channel;
};

/**
 * @brief CDC ACM 数据接口处理器（处理数据传输）
 *
 * 用于处理 CDC ACM 设备的数据接口，处理批量数据传输。
 */
class USBIPDCPP_API CdcAcmDataInterfaceHandler : public VirtualInterfaceHandler {
public:
    CdcAcmDataInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool);

    /**
     * @brief 创建 CDC ACM 数据接口（描述符模板，未绑定 handler）
     *
     * 接口定义：CDC Data 类（0A/00/00），一个 Bulk IN + 一个 Bulk OUT 端点
     * （Full speed，mps=64）。
     * @param in_ep Bulk IN 端点地址（设备→主机，串口数据）
     * @param out_ep Bulk OUT 端点地址（主机→设备，串口数据）
     * @return 未绑定 handler 的 UsbInterface
     */
    static UsbInterface make_interface(std::uint8_t in_ep, std::uint8_t out_ep) {
        UsbInterface i{
                .interface_class = static_cast<std::uint8_t>(ClassCode::CDCData),
                .interface_subclass = 0x00,
                .interface_protocol = 0x00,
                .endpoints = {{UsbEndpoint{.address = in_ep,
                                           .attributes = static_cast<std::uint8_t>(EndpointAttributes::Bulk),
                                           .max_packet_size = 64,
                                           .interval = 0},
                               UsbEndpoint{.address = out_ep,
                                           .attributes = static_cast<std::uint8_t>(EndpointAttributes::Bulk),
                                           .max_packet_size = 64,
                                           .interval = 0}}},
        };
        return i;
    }

    // ========== 内部实现（子类无需关心） ==========

    void handle_bulk_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                              std::uint32_t transfer_buffer_length, TransferHandle transfer,
                              std::error_code &ec) override;

    void handle_non_standard_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                      std::uint32_t transfer_flags,
                                                      std::uint32_t transfer_buffer_length,
                                                      const SetupPacket &setup_packet, TransferHandle transfer,
                                                      std::error_code &ec) override;

    [[nodiscard]] data_type get_class_specific_descriptor() override;

    // ========== 子类可选重写的回调 ==========

    /**
     * @brief 主机 OUT 数据到达时回调，供子类当场根据输入生成数据（通常把生成
     * 的数据塞给 IN 方向，如回显）。在请求挂起（on_out_request）之前调用
     * @param data 接收到的数据（右值引用）。注意契约：返回 false 表示未处理，
     * 请求会挂起，此时必须保证 data 未被移动走（数据仍留在挂起请求里，等子类
     * 之后 take_out() 取出）
     * @return true=已处理这条 OUT（handler 立即应答）；false=未处理，handler 挂起
     * 请求（主机 NAK 背压），子类之后用 take_out() 取出数据并应答
     */
    virtual bool on_data_received(data_type &&data);

    /**
     * @brief 主机请求数据时回调，用于实时按需生成数据
     * @param length 主机请求的数据长度
     * @return 返回要发送的数据，如果返回空则等待 send_data 调用
     * @warning 禁止在里面调用send_data等函数，会死锁。这个函数就是跟你按需生成数据的，
     * 生成数据后当场就给你发走了，没任何必要调用send_data等函数。
     */
    virtual data_type on_data_requested(std::uint16_t length);

    /**
     * @brief 主机 RTS 状态变化时回调
     * @param rts RTS 状态，true=主机愿意接收数据
     */
    virtual void on_rts_changed(bool rts);

    // ========== 发送数据 API ==========

    /**
     * @brief 非阻塞发送数据到主机
     * @param data 数据指针
     * @param size 数据大小
     * @return 实际写入缓冲区的字节数，缓冲区满时可能小于请求值
     */
    std::size_t send_data(const std::uint8_t *data, std::size_t size);
    std::size_t send_data(const data_type &data);
    std::size_t send_data(data_type &&data);
    std::size_t send_data(std::string_view data);

    /**
     * @brief 阻塞发送数据到主机，等待缓冲区有空间
     * @param data 数据指针
     * @param size 数据大小
     * @param timeout_ms 超时时间（毫秒），0 表示无限等待
     * @return 实际写入缓冲区的字节数，超时时可能小于请求值
     */
    std::size_t send_data_blocking(const std::uint8_t *data, std::size_t size, std::uint32_t timeout_ms = 0);
    std::size_t send_data_blocking(const data_type &data, std::uint32_t timeout_ms = 0);
    std::size_t send_data_blocking(data_type &&data, std::uint32_t timeout_ms = 0);
    std::size_t send_data_blocking(std::string_view data, std::uint32_t timeout_ms = 0);

    // ========== 接收数据 API（OUT 方向，挂起模式） ==========

    /**
     * @brief 阻塞取一条主机 OUT 数据。timeout_ms=0 无限等；断连返回 nullopt。
     * OUT 请求先挂起不立即应答（主机 NAK 背压），take_out() 取走时才应答
     */
    std::optional<OutEndpointChannel::Pending> take_out(std::uint32_t timeout_ms = 0);

    /// 非阻塞取一条主机 OUT 数据；无数据返回 nullopt
    std::optional<OutEndpointChannel::Pending> try_take_out();

    // ========== 缓冲区配置 API ==========

    /**
     * @brief 设置 TX 缓冲区容量
     * @param capacity 缓冲区大小（字节）
     */
    void set_tx_buffer_capacity(std::size_t capacity);

    /**
     * @brief 设置 TX 水位线
     * @param high 高水位线，缓冲区超过此值时建议触发流控
     * @param low 低水位线，缓冲区低于此值时建议恢复发送
     */
    void set_tx_watermarks(std::size_t high, std::size_t low);

    /**
     * @brief 获取 TX 缓冲区当前数据量
     * @return 缓冲区中已使用字节数
     */
    [[nodiscard]] std::size_t get_tx_buffer_size() const;

    /**
     * @brief 获取 TX 缓冲区剩余空间
     * @return 缓冲区可用字节数
     */
    [[nodiscard]] std::size_t get_tx_buffer_available() const;

    // ========== 流控状态 API ==========

    /**
     * @brief 设置 CTS 状态通知主机
     * @param cts CTS 状态，true=设备可以接收数据
     */
    void set_cts(bool cts);

    /**
     * @brief 获取当前 RTS 状态（来自主机）
     * @return RTS 状态，true=主机愿意接收数据
     */
    [[nodiscard]] bool get_rts() const;

    /**
     * @brief 关联通信接口处理器
     * @param handler 通信接口处理器指针
     */
    void set_comm_handler(CdcAcmCommunicationInterfaceHandler *handler);

    // ========== 内部实现（子类无需关心） ==========

    void on_new_connection(TransferResponder &current_session, std::error_code &ec) override;
    void on_disconnection(std::error_code &ec) override;
    void handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) override;

protected:
    /**
     * @brief IN 数据通道（字节流模式，发回主机）
     *
     * 封装「挂起-应答」+ 阻塞写：主机 bulk IN 请求先挂起，send_data /
     * send_data_blocking 写入数据时匹配应答；缓冲满时阻塞等待宿主取走。
     * on_data_requested 通过 set_pull_callback 接入（锁内调用，与原来的
     * handle_bulk_transfer 双锁内调用语义一致）
     */
    ByteStreamInChannel in_channel;

    /// OUT 数据挂起通道：主机 bulk OUT 请求先挂起不应答（NAK 背压），
    /// take_out() 取走时读出数据并应答
    OutEndpointChannel out_channel;

    std::size_t in_high_watermark_ = 48 * 1024;
    std::size_t in_low_watermark_ = 16 * 1024;

    /**
     * @brief 关联的通信接口处理器
     */
    CdcAcmCommunicationInterfaceHandler *comm_handler_ = nullptr;
};
} // namespace usbipdcpp
