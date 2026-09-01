#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <system_error>
#include <vector>

#include "usbipdcpp/Export.h"
#include "usbipdcpp/virtual_device/UvcConstants.h"
#include "usbipdcpp/virtual_device/VirtualDeviceHandler.h"
#include "usbipdcpp/virtual_device/InEndpointChannel.h"
#include "usbipdcpp/virtual_device/VirtualInterfaceHandler.h"
#include "usbipdcpp/virtual_device/video_sources/VideoSource.h"

namespace usbipdcpp {

/// PROBE/COMMIT 协商结构体（UVC 1.5, 48 字节）
struct UvcStreamingControl {
    std::uint16_t bmHint = 0;
    std::uint8_t bFormatIndex = 1;
    std::uint8_t bFrameIndex = 1;
    std::uint32_t dwFrameInterval = 333333;
    std::uint16_t wKeyFrameRate = 0;
    std::uint16_t wPFrameRate = 0;
    std::uint16_t wCompQuality = 0;
    std::uint16_t wCompWindowSize = 0;
    std::uint16_t wDelay = 0;
    std::uint32_t dwMaxVideoFrameSize = 0;
    std::uint32_t dwMaxPayloadTransferSize = 0;
    std::uint32_t dwClockFrequency = 27000000;
    std::uint8_t bmFramingInfo = 0x03;
    std::uint8_t bPreferredVersion = 1;
    std::uint8_t bMinVersion = 1;
    std::uint8_t bMaxVersion = 1;
    // UVC 1.5 additional fields
    std::uint8_t bUsage = 0;
    std::uint8_t bBitDepthLuma = 0;
    std::uint8_t bmSettings = 0;
    std::uint8_t bMaxNumberOfRefFramesPlus1 = 0;
    std::uint16_t bmRateControlModes = 0;
    std::uint64_t bmLayoutPerStream = 0;

    static constexpr std::size_t SIZE = 48; // UVC 1.5 full size

    data_type serialize() const;
    void deserialize(const std::uint8_t *data, std::size_t len);
};

class UvcVideoStreamingHandler;

/// VideoControl 接口处理器 — 处理 PU 属性查询/设置 + 与 VS 接口协同
class USBIPDCPP_API UvcVideoControlHandler : public VirtualInterfaceHandler {
public:
    explicit UvcVideoControlHandler(UsbInterface &handle_interface, StringPool &string_pool);

    [[nodiscard]] data_type get_class_specific_descriptor() override;
    data_type request_get_descriptor(std::uint8_t type, std::uint8_t language_id, std::uint16_t descriptor_length,
                                     std::uint32_t *p_status) override;
    void handle_non_standard_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                      std::uint32_t transfer_flags,
                                                      std::uint32_t transfer_buffer_length,
                                                      const SetupPacket &setup_packet, TransferHandle transfer,
                                                      std::error_code &ec) override;
    void handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                                   std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                   std::error_code &ec) override;
    void on_setup_interface_handlers() override;
    void on_new_connection(TransferResponder &current_session, error_code &ec) override;
    void on_disconnection(error_code &ec) override;
    void handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) override;

    void set_vs_handler(UvcVideoStreamingHandler *handler) {
        vs_handler_ = handler;
    }

private:
    void build_class_descriptor();
    void send_vc_status(data_type status);

    data_type class_desc_;
    bool desc_built_ = false;
    bool power_on_ = true;
    UvcVideoStreamingHandler *vs_handler_ = nullptr;

    /**
     * @brief 状态通知通道（消息模式，中断 IN 端点）
     *
     * 封装「挂起-应答」：主机中断 IN 请求先挂起，send_vc_status() 推入
     * 状态事件时匹配应答（UVC 1.5 状态通知，6 字节）
     */
    MessageInChannel status_channel;
};

/// VideoStreaming 接口处理器 — PROBE/COMMIT + ISO 流推送
class USBIPDCPP_API UvcVideoStreamingHandler : public VirtualInterfaceHandler {
public:
    UvcVideoStreamingHandler(UsbInterface &handle_interface, StringPool &string_pool,
                             std::unique_ptr<VideoSource> source);

    [[nodiscard]] data_type get_class_specific_descriptor() override;
    data_type request_get_descriptor(std::uint8_t type, std::uint8_t language_id, std::uint16_t descriptor_length,
                                     std::uint32_t *p_status) override;
    void handle_non_standard_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                      std::uint32_t transfer_flags,
                                                      std::uint32_t transfer_buffer_length,
                                                      const SetupPacket &setup_packet, TransferHandle transfer,
                                                      std::error_code &ec) override;
    void handle_isochronous_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                                     std::uint32_t transfer_buffer_length, TransferHandle transfer, int num_iso_packets,
                                     std::error_code &ec) override;
    void on_new_connection(TransferResponder &current_session, error_code &ec) override;
    void on_disconnection(error_code &ec) override;
    void request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) override;
    std::uint8_t request_get_interface(std::uint32_t *p_status) override;
    void on_setup_interface_handlers() override;

    VideoSource *get_source() {
        return source_.get();
    }

    void set_vc_handler(UvcVideoControlHandler *handler) {
        vc_handler_ = handler;
    }

    /// VC handler 通知停止流（Video Power Mode off 等场景）
    void notify_stop_streaming() {
        streaming_ = false;
    }

private:
    void build_class_descriptor();

    UvcVideoControlHandler *vc_handler_ = nullptr;

    std::unique_ptr<VideoSource> source_;
    data_type class_desc_;
    UvcStreamingControl probe_data_{};
    bool committed_ = false;
    bool streaming_ = false;

    // 帧缓冲
    std::vector<std::uint8_t> frame_buffer_;
    std::size_t frame_offset_ = 0;
    bool current_fid_ = false;

    // 帧时钟：本帧开始时刻 + 协商帧间隔。对齐真实摄像头语义——帧率由帧时钟
    // 决定，不随主机消费速度漂移：
    // - 带宽不足（一帧传不完一个帧间隔）：丢帧切最新帧，画面实时只是帧率低
    // - 带宽富余（帧传完还没到帧间隔）：空包等下一帧，避免快放
    // 首帧前为 epoch（now 恒 ≥ 它 + 间隔），保证开流第一帧立即开始
    std::chrono::steady_clock::time_point frame_started_at_{};
    std::chrono::microseconds frame_interval_{};
};

/// UVC 设备辅助类 — 在 device 上装配 UVC 功能（VC + VS 接口 handler、互相关联、IAD）
class USBIPDCPP_API UvcDeviceHelper {
public:
    /**
     * @brief 在 device 上装配 UVC 功能（VC + VS 两个相邻接口）
     * @param device 目标设备（接口必须已在 device->interfaces 中，地址稳定）
     * @param vc_interface_number UVC 功能首个接口（VC）的 interface_number；
     *        VS 按 interface_number = vc_interface_number + 1 在
     *        device->interfaces 中查找（IAD 要求功能接口编号连续，与数组
     *        下标无关——复合设备里 UVC 接口可能不在 interfaces 开头）。
     *        设备里其他接口的 handler 不受影响（只绑定 VC/VS 两个接口）
     * @param string_pool 字符串池
     * @param source 视频源
     * @return 出错时的错误码（如 VC/VS 接口缺失），成功返回默认构造（无错误）
     */
    static std::error_code setup(std::shared_ptr<UsbDevice> device, std::uint8_t vc_interface_number,
                                 StringPool &string_pool, std::unique_ptr<VideoSource> source);
};

} // namespace usbipdcpp
