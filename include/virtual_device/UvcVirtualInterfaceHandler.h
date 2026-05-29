#pragma once

// ==================== 已知问题 / Known Issues ====================
// Windows: usbvideo.sys 加载后报 Code 10 (STATUS_IO_DEVICE_ERROR 0xC00000E5)。
//   标准 USB 枚举（15 个控制传输）全部成功，但 usbvideo.sys 在 StartDevice 阶段
//   就失败了，我们的服务器看不到任何 UVC 类特定请求或非控制传输。
//   已经尝试过：修正 bcdUVC 版本号、CS_INTERFACE GET_DESCRIPTOR 响应、
//   PROBE/COMMIT 大小适配、RET_SUBMIT padding 清零、iInterface 同步等，均未解决。
//   Linux (vhci-hcd + uvcvideo) 完全正常，可以 PROBE/COMMIT 协商 + ISO 推流。
//   如果你知道原因或能修复 Windows 兼容性，请提 PR，非常感谢！
// ================================================================

#include <memory>
#include <vector>

#include "Export.h"
#include "virtual_device/UvcConstants.h"
#include "virtual_device/VirtualDeviceHandler.h"
#include "virtual_device/VirtualInterfaceHandler.h"
#include "virtual_device/video_sources/VideoSource.h"

namespace usbipdcpp {

/// PROBE/COMMIT 协商结构体（UVC 1.5, 48 字节）
struct UvcStreamingControl {
    std::uint16_t bmHint = 0;
    std::uint8_t bFormatIndex = 1;
    std::uint8_t bFrameIndex = 1;
    std::uint32_t dwFrameInterval = 333333;
    std::uint16_t wKeyFrameRate = 1;
    std::uint16_t wPFrameRate = 0;
    std::uint16_t wCompQuality = 1;
    std::uint16_t wCompWindowSize = 1;
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

    static constexpr std::size_t SIZE = 48;

    data_type serialize() const;
    void deserialize(const std::uint8_t *data, std::size_t len);
};

class UvcVideoStreamingHandler;

/// VideoControl 接口处理器 — 处理 PU 属性查询/设置 + 与 VS 接口协同
class USBIPDCPP_API UvcVideoControlHandler : public VirtualInterfaceHandler {
public:
    explicit UvcVideoControlHandler(UsbInterface &handle_interface, StringPool &string_pool);

    [[nodiscard]] data_type get_class_specific_descriptor() override;
    void handle_non_standard_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                      std::uint32_t transfer_flags,
                                                      std::uint32_t transfer_buffer_length,
                                                      const SetupPacket &setup_packet, TransferHandle transfer,
                                                      std::error_code &ec) override;
    void handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                                   std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                   std::error_code &ec) override;
    void request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) override;
    std::uint8_t request_get_interface(std::uint32_t *p_status) override;
    void request_set_feature(std::uint16_t feature_selector, std::uint32_t *p_status) override;
    void request_endpoint_set_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                      std::uint32_t *p_status) override;
    void request_clear_feature(std::uint16_t feature_selector, std::uint32_t *p_status) override;
    void request_endpoint_clear_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                        std::uint32_t *p_status) override;
    std::uint16_t request_get_status(std::uint32_t *p_status) override;
    std::uint16_t request_endpoint_get_status(std::uint8_t ep_address, std::uint32_t *p_status) override;
    void on_setup_interface_handlers() override;
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

    std::deque<data_type> pending_status_;
    mutable std::mutex status_mutex_;
};

/// VideoStreaming 接口处理器 — PROBE/COMMIT + ISO 流推送
class USBIPDCPP_API UvcVideoStreamingHandler : public VirtualInterfaceHandler {
public:
    UvcVideoStreamingHandler(UsbInterface &handle_interface, StringPool &string_pool,
                             std::unique_ptr<VideoSource> source);

    [[nodiscard]] data_type get_class_specific_descriptor() override;
    void handle_non_standard_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                      std::uint32_t transfer_flags,
                                                      std::uint32_t transfer_buffer_length,
                                                      const SetupPacket &setup_packet, TransferHandle transfer,
                                                      std::error_code &ec) override;
    void handle_isochronous_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                                     std::uint32_t transfer_buffer_length, TransferHandle transfer, int num_iso_packets,
                                     std::error_code &ec) override;
    void on_new_connection(Session &current_session, error_code &ec) override;
    void on_disconnection(error_code &ec) override;
    void request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) override;
    std::uint8_t request_get_interface(std::uint32_t *p_status) override;
    void request_set_feature(std::uint16_t feature_selector, std::uint32_t *p_status) override;
    void request_endpoint_set_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                      std::uint32_t *p_status) override;
    void request_clear_feature(std::uint16_t feature_selector, std::uint32_t *p_status) override;
    void request_endpoint_clear_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                        std::uint32_t *p_status) override;
    std::uint16_t request_get_status(std::uint32_t *p_status) override;
    std::uint16_t request_endpoint_get_status(std::uint8_t ep_address, std::uint32_t *p_status) override;
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
};

/// UVC 设备辅助类 — 在 device 上注册 VC/VS 接口 handler 并设置描述符
class USBIPDCPP_API UvcDeviceHelper {
public:
    /// 向 device 注入 UVC 接口 handler。
    /// device 必须已有两个接口（VC + VS），且第二个接口需含 ISO IN 端点
    static void setup(std::shared_ptr<UsbDevice> device, StringPool &string_pool, std::unique_ptr<VideoSource> source);
};

} // namespace usbipdcpp
