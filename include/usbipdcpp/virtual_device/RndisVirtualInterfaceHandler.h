#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <optional>

#include "usbipdcpp/virtual_device/InEndpointChannel.h"
#include "usbipdcpp/virtual_device/OutEndpointChannel.h"
#include "usbipdcpp/virtual_device/VirtualInterfaceHandler.h"
#include "usbipdcpp/virtual_device/network_backends/NetworkBackend.h"

namespace usbipdcpp {

class RndisDataInterfaceHandler;

// RNDIS 设备状态机（对齐内核 rndis.c：UNINITIALIZED → INITIALIZED →
// DATA_INITIALIZED（SET filter≠0 后 carrier on），HALT/断连回 UNINITIALIZED）
enum class RndisState : std::uint8_t {
    Uninitialized,
    Initialized,
    DataInitialized,
};

/**
 * @brief RNDIS 通信接口处理器：响应 CDC 封装命令，维护 RNDIS 状态机与响应队列
 *
 * 对应 RNDIS 的控制接口（02/02/0xFF：COMM 类 + ACM 子类 + Vendor 协议，
 * 对齐内核 f_rndis.c——Windows 与 Linux rndis_host 均按此匹配，不用微软
 * 专有 RNDIS 描述符）。RNDIS 控制消息（INIT/QUERY/SET/KEEPALIVE/RESET/HALT）
 * 全部经 ep0 的 SEND_ENCAPSULATED_COMMAND 送入，响应排队由
 * GET_ENCAPSULATED_RESPONSE 取走；每条响应入队都经中断 IN 端点发
 * RESPONSE_AVAILABLE 通知（对齐 rndis.c 的 resp_avail 回调）。
 */
class USBIPDCPP_API RndisCommunicationInterfaceHandler : public VirtualInterfaceHandler {
public:
    /**
     * @param handle_interface 本接口
     * @param string_pool 字符串池（需活得比 handler 久）
     * @param mac_address 设备 MAC 地址（OID_802_3_*_ADDRESS 查询返回）
     * @param speed 设备速度（OID_GEN_LINK_SPEED 按此上报，100bps 单位，对齐
     *        内核 f_rndis.c 的 bitrate()）
     */
    RndisCommunicationInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool,
                                       std::array<std::uint8_t, 6> mac_address, UsbSpeed speed);

    /**
     * @brief 创建 RNDIS 通信接口（描述符模板，未绑定 handler）
     *
     * 接口定义：COMM 类 + ACM 子类 + Vendor 协议（02/02/0xFF），一个中断 IN
     * 端点（mps=8、interval=32ms，RESPONSE_AVAILABLE 通知；对齐内核
     * f_rndis.c 的 fs_control_intf_ep：状态端点 mps≥8 且 interval≠0 是
     * cdc_ether 主机驱动绑定 RNDIS 的硬性要求）。
     * @param interrupt_in_ep 中断 IN 端点地址（设备→主机，状态通知）
     * @return 未绑定 handler 的 UsbInterface
     */
    static UsbInterface make_interface(std::uint8_t interrupt_in_ep);

    /**
     * @brief 关联数据接口处理器（装配时调用）
     *
     * 通信接口类特定描述符的 bDataInterface（CallManagement/Union）引用
     * 数据接口号，装配时从数据 handler 的 get_interface() 取——复合设备里
     * 通信接口不从接口 0 起，硬编码 1 会让主机驱动（rndis_host/Windows
     * rndismp）找不到数据接口
     * @param handler 数据接口 handler
     */
    void set_data_handler(RndisDataInterfaceHandler *handler) {
        data_handler_ = handler;
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

    // ========== 状态查询 API ==========

    /// 数据通道是否已启用：INIT 且主机设置了非零包过滤器（对齐 rndis.c 的
    /// RNDIS_DATA_INITIALIZED——filter≠0 才 carrier on 开始收发）
    [[nodiscard]] bool is_data_enabled() const {
        return state_ == RndisState::DataInitialized;
    }

    /// 当前 RNDIS 状态机状态
    [[nodiscard]] RndisState get_state() const {
        return state_;
    }

    // ========== 内部实现（子类无需关心） ==========

    void on_new_connection(TransferResponder &current_session, std::error_code &ec) override;

    void on_disconnection(std::error_code &ec) override;

    void handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) override;

protected:
    // 响应队列：GET_ENCAPSULATED_RESPONSE 按序取走（对齐 rndis.c 的响应队列）。
    // 状态机与队列是 RNDIS 协议核心，数据接口经 is_data_enabled() 查询
    std::deque<data_type> response_queue_;
    RndisState state_ = RndisState::Uninitialized;
    std::uint16_t packet_filter_ = 0;
    std::array<std::uint8_t, 6> mac_address_{};
    std::uint32_t link_speed_100bps_ = 0; // OID_GEN_LINK_SPEED 返回值（100bps 单位）
    std::uint8_t interface_number_; // 本接口号（通知 wIndex 用，构造时从接口对象取）
    RndisDataInterfaceHandler *data_handler_ = nullptr; // set_data_handler 装配时写入

    /// 状态通知通道（中断 IN 端点）：一条通知 = 一个完整消息（8 字节
    /// RESPONSE_AVAILABLE）。有挂起请求直接应答，否则入缓冲（上限 30 条）
    MessageInChannel notification_channel;

    // 消息处理（对齐 rndis.c 的 rndis_msg_parser）
    void process_rndis_message(const data_type &msg);
    /// 入队一条响应并发 RESPONSE_AVAILABLE 通知（超上限丢最旧）
    void enqueue_response(data_type &&response);
    /// 查询 OID 返回数据区字节；未知 OID 返回空（组装 NOT_SUPPORTED 的 QUERY_C）
    data_type query_oid(std::uint32_t oid);
    /// 复位设备状态（HALT/断连）：回 UNINITIALIZED 并清空响应队列
    void reset_device_state();
};

/**
 * @brief RNDIS 数据接口处理器：bulk 收发 RNDIS_MSG_PACKET 封装的以太网帧
 *
 * 对应 RNDIS 数据接口（0A/00/00，无 altsetting，对齐内核 f_rndis.c 的
 * rndis_data_intf——RNDIS 不需要 ECM 的 NOP altsetting）。主机发来的
 * bulk OUT 是 RNDIS_MSG_PACKET 消息（头 24 字节 + 以太网帧，一次传输可
 * 聚合多包/带尾填充），剥头后逐帧交给 NetworkBackend；发往主机的帧包上
 * 44 字节头（DataOffset=36，数据从字节 44 起）。封装格式对齐内核 rndis.c
 * 的 rndis_add_hdr / rndis_rm_hdr
 */
class USBIPDCPP_API RndisDataInterfaceHandler : public VirtualInterfaceHandler {
public:
    /**
     * @param handle_interface 本接口
     * @param string_pool 字符串池（需活得比 handler 久）
     * @param backend 网络后端（可空）。非空时主机发来的帧解析剥头后直接交给
     *        backend 消费并立即应答；为空走挂起模式（on_frame_received /
     *        take_frame，取的是原始 RNDIS 消息）
     * @param comm 通信接口处理器（RNDIS 状态机归属方），需活得比本 handler 久
     */
    RndisDataInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool, NetworkBackend *backend,
                              RndisCommunicationInterfaceHandler *comm);

    /**
     * @brief 创建 RNDIS 数据接口（描述符模板，未绑定 handler）
     *
     * 接口定义：CDC Data 类（0A/00/00），两个 Bulk 端点（Full speed，mps=64）。
     * @param in_ep Bulk IN 端点地址（设备→主机，发帧）
     * @param out_ep Bulk OUT 端点地址（主机→设备，收帧）
     * @return 未绑定 handler 的 UsbInterface
     */
    static UsbInterface make_interface(std::uint8_t in_ep, std::uint8_t out_ep);

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
     * @brief 主机发来一帧时回调（未设置 NetworkBackend 时的消费入口）
     * @param frame 以太网帧（右值引用，已剥 RNDIS 头）。契约：返回 false 表示
     *        未处理，请求整体挂起入通道（frame 可被移动走，原数据留在挂起
     *        请求里，等 take_frame() 取走）
     * @return true=已消费这条帧；false=未处理，挂起
     */
    virtual bool on_frame_received(data_type &&frame);

    // ========== 发帧 API（设备→主机，消息模式，非阻塞） ==========

    /**
     * @brief 发一帧到主机（自动包 RNDIS_MSG_PACKET 头；非阻塞，缓冲超上限时
     * 丢最旧——网络数据可丢，TCP 重传兜底）
     * @return 实际入缓冲字节数（含 RNDIS 头）；断连返回 0
     */
    std::size_t send_frame(const std::uint8_t *data, std::size_t size);

    /// 发一帧到主机（非阻塞）
    std::size_t send_frame(const data_type &frame);

    // ========== 取帧 API（主机→设备，挂起模式） ==========

    /**
     * @brief 阻塞取一帧主机发来的原始数据。timeout_ms=0 无限等；断连返回
     * nullopt。OUT 请求先挂起不立即应答（主机 NAK 背压），取走时才应答
     */
    std::optional<OutEndpointChannel::Pending> take_frame(std::uint32_t timeout_ms = 0);

    /// 非阻塞取一帧；无数据返回 nullopt
    std::optional<OutEndpointChannel::Pending> try_take_frame();

    // ========== 缓冲区配置 API ==========

    /// 设置发帧缓冲上限（条数，默认 0 = 无限；超限丢最旧）
    void set_tx_max_pending(std::size_t max_pending);

    // ========== 内部实现（子类无需关心） ==========

    void on_new_connection(TransferResponder &current_session, std::error_code &ec) override;

    void on_disconnection(std::error_code &ec) override;

    void handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) override;

protected:
    /// 发帧通道（消息模式）：一个 URB 恰好一帧（一条 RNDIS_MSG_PACKET 消息），
    /// 帧比请求短发短包结束——主机 usbnet 按消息长度拆包，字节流分片会坏帧
    MessageInChannel in_channel;

    /// 收帧通道：主机 bulk OUT 请求先挂起不应答（NAK 背压），take_frame()
    /// 取走时读出数据并应答
    OutEndpointChannel out_channel;

    NetworkBackend *backend_ = nullptr;
    RndisCommunicationInterfaceHandler *comm_ = nullptr; // RNDIS 状态机归属方
};

} // namespace usbipdcpp
