#pragma once

#include <array>
#include <cstdint>
#include <optional>

#include "usbipdcpp/virtual_device/InEndpointChannel.h"
#include "usbipdcpp/virtual_device/OutEndpointChannel.h"
#include "usbipdcpp/virtual_device/VirtualInterfaceHandler.h"
#include "usbipdcpp/virtual_device/network_backends/NetworkBackend.h"

namespace usbipdcpp {

class EcmDataInterfaceHandler;

/**
 * @brief ECM 通信接口处理器：响应 CDC 类控制请求，中断 IN 端点发送状态通知
 *
 * 对应 ECM 规范的数据接口之一（02/06/00）。控制面只有 SET_ETHERNET_PACKET_FILTER
 * 一个必答请求（接口 up 时 Linux cdc_ether 必发，无数据、回成功即可），其余
 * 类请求对齐内核 f_ecm.c 一律 STALL（我们 bmEthernetStatistics=0、bNumberPowerFilters=0
 * 声明了不支持统计与电源过滤）。NETWORK_CONNECTION / SPEED_CHANGE 通知可选：
 * cdc_ether 主机驱动初始 link up，不发通知也能通信（通知用于反映设备侧状态变化）。
 */
class USBIPDCPP_API EcmCommunicationInterfaceHandler : public VirtualInterfaceHandler {
public:
    /**
     * @param handle_interface 本接口（接口号取通知 wIndex 用）
     * @param string_pool 字符串池（需活得比 handler 久）
     * @param mac_address 设备 MAC 地址（12 个 hex 字符写 iMACAddress 字符串描述符）
     */
    EcmCommunicationInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool,
                                     std::array<std::uint8_t, 6> mac_address);

    /**
     * @brief 创建 ECM 通信接口（描述符模板，未绑定 handler）
     *
     * 接口定义：CDC 类、Ethernet Networking 子类（02/06/00），一个中断 IN
     * 端点（mps=16、interval=32ms，用于 NETWORK_CONNECTION / SPEED_CHANGE 状态通知）。
     * @param interrupt_in_ep 中断 IN 端点地址（设备→主机，状态通知）
     * @return 未绑定 handler 的 UsbInterface
     */
    static UsbInterface make_interface(std::uint8_t interrupt_in_ep);

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
     * @brief 主机设置以太网包过滤器时回调（Linux cdc_ether 接口 up 时必发）
     * @param filter 过滤位图（ECM120 §6.2.4 表 8：D0 混杂 / D1 全多播 / D2 定向 /
     *        D3 广播 / D4 多播）
     */
    virtual void on_set_packet_filter(std::uint16_t filter);

    // ========== 通知 API ==========

    /**
     * @brief 发送 NETWORK_CONNECTION 通知（连接状态，wValue=0/1，无数据字节）
     * @param up true=链路连接（设备侧可收发帧），false=断开
     * @note 对齐内核 f_ecm.c 的 ecm_do_notify：状态放 wValue，wLength=0
     */
    void send_network_connection(bool up);

    /**
     * @brief 发送 CONNECTION_SPEED_CHANGE 通知（上下行速率）
     * @param up_speed 上行速率（设备→主机，bits/sec）
     * @param down_speed 下行速率（主机→设备，bits/sec）
     * @note 对齐内核：数据 8 字节 = up/down 各 4 字节小端
     */
    void send_speed_change(std::uint32_t up_speed, std::uint32_t down_speed);

    // ========== 接口关联 API ==========

    /// 关联数据接口处理器（Union 描述符按固定接口 0/1 排列，示例结构如此）
    void set_data_handler(EcmDataInterfaceHandler *handler) {
        data_handler_ = handler;
    }

    /// 获取关联的数据接口处理器
    EcmDataInterfaceHandler *get_data_handler() const {
        return data_handler_;
    }

    // ========== 状态查询 API ==========

    /// 获取当前包过滤器位图（主机 SET_ETHERNET_PACKET_FILTER 设置）
    [[nodiscard]] std::uint16_t get_packet_filter() const {
        return packet_filter_;
    }

    // ========== 内部实现（子类无需关心） ==========

    void on_new_connection(TransferResponder &current_session, std::error_code &ec) override;

    void on_disconnection(std::error_code &ec) override;

    void handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) override;

protected:
    std::uint16_t packet_filter_ = 0;
    std::uint8_t mac_string_index_;
    std::uint8_t interface_number_; // 本接口号（通知 wIndex 用，构造时从接口对象取）

    /// 状态通知通道（中断 IN 端点）：一条通知 = 一个完整消息。有挂起请求直接
    /// 应答，否则入缓冲（上限 1 条：只保留最新状态，通知低频丢旧保新即可）
    MessageInChannel notification_channel;

    EcmDataInterfaceHandler *data_handler_ = nullptr;
};

/**
 * @brief ECM 数据接口处理器：bulk 收发以太网帧
 *
 * 对应 ECM 规范的数据接口之二（0A/00/00）。alt0 无端点、alt1 两个 bulk——
 * 主机在 alt1 激活后才开始收发帧（内核 f_ecm.c 的 set_alt 打开数据面的语义，
 * 由基类 request_set_interface 校验 alt 存在性即可，无需额外状态）。
 *
 * 数据面一帧一条：bulk IN 方向用消息模式通道（一个 URB 应答恰好一帧；若用
 * 字节流分片，主机 rx_urb_size 缓冲会被塞入多帧，usbnet 把整个 URB 当一帧
 * 解析导致坏帧）。OUT 方向挂起（NAK 背压）或交给 NetworkBackend 立即消费。
 */
class USBIPDCPP_API EcmDataInterfaceHandler : public VirtualInterfaceHandler {
public:
    /**
     * @param handle_interface 本接口
     * @param string_pool 字符串池（需活得比 handler 久）
     * @param backend 网络后端（可空）。非空时主机发来的帧直接交给 backend
     *        消费并立即应答；为空走挂起模式（on_frame_received / take_frame）
     */
    EcmDataInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool,
                            NetworkBackend *backend = nullptr);

    /**
     * @brief 创建 ECM 数据接口（描述符模板，未绑定 handler）
     *
     * 接口定义：CDC Data 类（0A/00/00），alt0 无端点 / alt1 两个 Bulk 端点
     * （Full speed，mps=64）。
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
     * @param frame 以太网帧（右值引用）。契约：返回 false 表示未处理，请求
     *        挂起入通道，此时必须保证 frame 未被移动走（数据留在挂起请求里，
     *        等 take_frame() 取走）
     * @return true=已消费这条帧（handler 立即应答）；false=未处理，挂起
     */
    virtual bool on_frame_received(data_type &&frame);

    // ========== 发帧 API（设备→主机，消息模式，非阻塞） ==========

    /**
     * @brief 发一帧到主机（非阻塞；缓冲超上限时丢最旧，主机长期不读时防内存
     * 无限增长——网络数据可丢，TCP 重传兜底）
     * @return 实际入缓冲字节数；断连返回 0
     */
    std::size_t send_frame(const std::uint8_t *data, std::size_t size);

    /// 发一帧到主机（非阻塞）
    std::size_t send_frame(const data_type &frame);

    // ========== 取帧 API（主机→设备，挂起模式） ==========

    /**
     * @brief 阻塞取一帧主机发来的数据。timeout_ms=0 无限等；断连返回 nullopt。
     * OUT 请求先挂起不立即应答（主机 NAK 背压），take_frame() 取走时才应答
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
    /// 发帧通道（消息模式）：一个 URB 恰好一帧，帧比请求短时发短包结束
    /// （usbnet 主机把每个 URB 当一帧；字节流分片会把多帧塞进一个 URB）
    MessageInChannel in_channel;

    /// 收帧通道：主机 bulk OUT 请求先挂起不应答（NAK 背压），take_frame()
    /// 取走时读出数据并应答
    OutEndpointChannel out_channel;

    NetworkBackend *backend_ = nullptr;
};

} // namespace usbipdcpp
