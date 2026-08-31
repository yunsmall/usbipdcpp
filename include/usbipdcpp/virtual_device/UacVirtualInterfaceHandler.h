#pragma once

#include <atomic>
#include <deque>
#include <memory>

#include "usbipdcpp/Export.h"
#include "usbipdcpp/virtual_device/UacConstants.h"
#include "usbipdcpp/virtual_device/InEndpointChannel.h"
#include "usbipdcpp/virtual_device/VirtualInterfaceHandler.h"
#include "usbipdcpp/virtual_device/audio_sinks/AudioSink.h"
#include "usbipdcpp/virtual_device/audio_sources/AudioSource.h"

namespace usbipdcpp {

/// UAC 设备配置 — 控制 AudioControl 接口描述符与 Feature Unit 行为
struct UacDeviceConfig {
    /// 输入终端类型。默认麦克风（ITT_MICROPHONE）；
    /// 扬声器方向传 TT_USB_STREAMING（数据从 USB 流入），此时输出终端用 output_terminal_type
    std::uint16_t input_terminal_type = ITT_MICROPHONE;
    /// 输出终端类型（仅扬声器方向使用，默认 Speaker）
    std::uint16_t output_terminal_type = ITT_SPEAKER;
    /// 声道数（1 或 2）。0 表示从 AudioSource 推断
    std::uint8_t channels = 0;
    /// Feature Unit 是否提供静音控制
    bool feature_unit_mute = true;
    /// Feature Unit 是否提供音量控制
    bool feature_unit_volume = true;
    /// 音量范围下限（1/256 dB 单位，默认 -42dB）
    std::int16_t volume_min_db256 = -0x2A00;
    /// 音量范围上限（1/256 dB 单位，默认 0dB）
    std::int16_t volume_max_db256 = 0;
};

/// AudioControl 接口处理器 — 类描述符 + Feature Unit（mute/volume）控制
class USBIPDCPP_API UacAudioControlHandler : public VirtualInterfaceHandler {
public:
    UacAudioControlHandler(UsbInterface &handle_interface, StringPool &string_pool);

    /// 应用设备配置（必须在 UacDeviceHelper::setup_microphone 调用 setup_interface_handlers 之前完成）
    void set_config(const UacDeviceConfig &config);

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
    void on_new_connection(TransferResponder &current_session, error_code &ec) override;
    void on_disconnection(error_code &ec) override;
    void handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) override;
    void on_setup_interface_handlers() override;

    /// 当前静音状态（供 AS handler 查询）
    [[nodiscard]] bool is_muted() const {
        return mute;
    }

    /// 当前音量线性增益，Q16 定点（0dB = 65536，-∞ = 0），供 AS handler 缩放样本
    [[nodiscard]] std::uint32_t volume_scale_q16() const {
        return gain_q16;
    }

private:
    void build_class_descriptor();

    /// 经 AC 中断端点推送 UAC1 状态字（对齐内核 f_uac1.c 的 audio_notify：
    /// 无挂起 URB 时缓存，主机后续提交的中断 IN URB 立即拿到，状态不丢）
    void send_ac_status(data_type status);

    data_type class_desc;
    bool desc_built = false;
    UacDeviceConfig config{};
    bool mute = false;
    std::int16_t volume_db = 0; // 单位 1/256 dB，0 表示 0dB
    std::uint32_t gain_q16 = 65536; // 10^(volume_db/256/20) 的 Q16 表示

    // AC 中断端点：状态变化时无挂起 URB 的暂存（UAC1 状态字 2 字节）。
    // push 非阻塞：有挂起请求直接应答，否则入缓冲（对齐内核 f_uac1 audio_notify）
    MessageInChannel status_channel;
};

/// AudioStreaming 接口处理器 — 类描述符 + 采样率协商 + ISO PCM 推流
class USBIPDCPP_API UacAudioStreamingSourceHandler : public VirtualInterfaceHandler {
public:
    UacAudioStreamingSourceHandler(UsbInterface &handle_interface, StringPool &string_pool,
                             std::unique_ptr<AudioSource> source);

    [[nodiscard]] data_type get_class_specific_descriptor() override;
    data_type request_get_descriptor(std::uint8_t type, std::uint8_t language_id, std::uint16_t descriptor_length,
                                     std::uint32_t *p_status) override;
    void handle_non_standard_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                      std::uint32_t transfer_flags,
                                                      std::uint32_t transfer_buffer_length,
                                                      const SetupPacket &setup_packet, TransferHandle transfer,
                                                      std::error_code &ec) override;
    // Linux snd-usb-audio 对 UAC1 的采样率控制发给端点（recipient=Endpoint，wIndex=端点地址），
    // 而非接口（UAC 1.0 规范允许的实现差异），需在端点级入口处理
    void handle_non_standard_request_type_control_urb_to_endpoint(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                                  std::uint32_t transfer_flags,
                                                                  std::uint32_t transfer_buffer_length,
                                                                  const SetupPacket &setup_packet,
                                                                  TransferHandle transfer, std::error_code &ec) override;
    void handle_isochronous_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                                     std::uint32_t transfer_buffer_length, TransferHandle transfer, int num_iso_packets,
                                     std::error_code &ec) override;
    void on_new_connection(TransferResponder &current_session, error_code &ec) override;
    void on_disconnection(error_code &ec) override;
    void request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) override;
    void on_setup_interface_handlers() override;

    // AS 的类描述符（General + Format Type）必须出现在每个 alt，
    // 否则主机驱动在 alt 1 找不到格式描述符无法解析
    [[nodiscard]] bool put_class_specific_descriptor_in_all_alts() const override {
        return true;
    }

    AudioSource *get_source() {
        return source.get();
    }

    void set_ac_handler(UacAudioControlHandler *handler) {
        ac_handler = handler;
    }

private:
    /// 收集当前声道数/位深下 source 支持的所有采样率（升序去重）
    [[nodiscard]] std::vector<std::uint32_t> supported_sample_rates(const AudioFormatInfo &fmt) const;
    void build_class_descriptor();

    /// 处理采样率控制请求（GET_CUR/GET_MIN/GET_MAX/SET_CUR）。
    /// 接口级与端点级入口共用（Linux snd-usb-audio 从端点级发起）。
    /// 返回 true 表示已自行提交响应，调用方直接返回；false 表示未识别
    bool handle_sampling_freq_control(std::uint32_t seqnum, std::uint8_t request, GenericTransfer *trx,
                                      TransferHandle &transfer, std::uint32_t transfer_buffer_length);

    /// 向 dst 填充 n 字节 PCM：从 source 拉数据，不足时填静音；应用 mute/volume
    void fill_pcm(std::uint8_t *dst, std::size_t n);

    /// 按当前采样率更新每 microframe 包字节数的调度参数（对齐内核 gadget u_audio.c）
    void update_packet_bytes();

    /// TransferScheduler 处理器（通知语义）：调度线程在服务时刻（数据时长
    /// 后）调用，填 PCM 数据、自行 session.submit_ret_submit 发送响应，
    /// 并 on_urb_done 上报完成（同端点串行依赖上报）
    void process_iso_in(TransferResponder &session, const UsbEndpoint &ep, std::uint32_t seqnum, int num_iso_packets,
                        TransferHandle transfer);

    UacAudioControlHandler *ac_handler = nullptr;

    std::unique_ptr<AudioSource> source;
    data_type class_desc;
    bool streaming = false;

    // ISO IN 包调度参数，对齐内核 gadget u_audio.c（u_audio_start_playback）：
    // 每包字节数 = 基准包长 + 残差累加溢出时补一帧，包长恒为帧大小的整数倍
    // （样本交错结构不被切断），平均速率精确匹配采样率，非 8kHz 整数倍采样率
    // （如 44100）也正确。采样率变化时由 update_packet_bytes 重算
    std::size_t packet_framesize = 0;   // 一帧 PCM 字节数 = 声道数×样本字节
    std::size_t packet_interval = 0;    // 每秒包数：高速 bInterval=1 → 每 microframe 一包
    std::size_t packet_base = 0;        // 基准包长 = 帧大小×(采样率/interval)
    std::size_t packet_residue_step = 0; // 每包累加的残差 = 帧大小×(采样率%interval)
    std::size_t packet_residue_acc = 0;  // 残差累加器（跨包/跨 URB 连续）

    // 当前音频块引用 + 已消费偏移（跨 URB 连续流）
    const std::uint8_t *chunk_data = nullptr;
    std::size_t chunk_size = 0;
    std::size_t chunk_offset = 0;
};

/// AudioStreaming 接口处理器（扬声器方向）— 类描述符 + 采样率协商 + ISO OUT 收流
/// 与 UacAudioStreamingSourceHandler（麦克风 IN 推流）对称：数据面反转为消费主机发来的
/// PCM（写入 AudioSink），控制面（采样率协商、类描述符）逻辑相同
class USBIPDCPP_API UacAudioStreamingSinkHandler : public VirtualInterfaceHandler {
public:
    UacAudioStreamingSinkHandler(UsbInterface &handle_interface, StringPool &string_pool,
                                 std::unique_ptr<AudioSink> sink);

    [[nodiscard]] data_type get_class_specific_descriptor() override;
    data_type request_get_descriptor(std::uint8_t type, std::uint8_t language_id, std::uint16_t descriptor_length,
                                     std::uint32_t *p_status) override;
    void handle_non_standard_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                      std::uint32_t transfer_flags,
                                                      std::uint32_t transfer_buffer_length,
                                                      const SetupPacket &setup_packet, TransferHandle transfer,
                                                      std::error_code &ec) override;
    // Linux snd-usb-audio 对 UAC1 的采样率控制发给端点（recipient=Endpoint，wIndex=端点地址），
    // 而非接口（UAC 1.0 规范允许的实现差异），需在端点级入口处理
    void handle_non_standard_request_type_control_urb_to_endpoint(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                                  std::uint32_t transfer_flags,
                                                                  std::uint32_t transfer_buffer_length,
                                                                  const SetupPacket &setup_packet,
                                                                  TransferHandle transfer, std::error_code &ec) override;
    void handle_isochronous_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                                     std::uint32_t transfer_buffer_length, TransferHandle transfer, int num_iso_packets,
                                     std::error_code &ec) override;
    void on_new_connection(TransferResponder &current_session, error_code &ec) override;
    void on_disconnection(error_code &ec) override;
    void request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) override;
    void on_setup_interface_handlers() override;

    // AS 的类描述符（General + Format Type）必须出现在每个 alt，
    // 否则主机驱动在 alt 1 找不到格式描述符无法解析
    [[nodiscard]] bool put_class_specific_descriptor_in_all_alts() const override {
        return true;
    }

    AudioSink *get_sink() {
        return sink.get();
    }

    void set_ac_handler(UacAudioControlHandler *handler) {
        ac_handler = handler;
    }

private:
    void build_class_descriptor();

    /// 处理采样率控制请求（GET_CUR/GET_MIN/GET_MAX/SET_CUR）。
    /// 接口级与端点级入口共用（Linux snd-usb-audio 从端点级发起）。
    /// 返回 true 表示已自行提交响应，调用方直接返回；false 表示未识别
    bool handle_sampling_freq_control(std::uint32_t seqnum, std::uint8_t request, GenericTransfer *trx,
                                      TransferHandle &transfer, std::uint32_t transfer_buffer_length);

    /// TransferScheduler 处理器（通知语义）：调度线程在服务时刻（数据时长
    /// 后）调用，消费 PCM（音量应用 + 写 sink + 收流闭环统计）、自行
    /// session.submit_ret_submit 发送响应，并 on_urb_done 上报完成
    ///（同端点串行依赖上报）
    void process_iso_out(TransferResponder &session, const UsbEndpoint &ep, std::uint32_t seqnum, int num_iso_packets,
                         TransferHandle transfer);

    UacAudioControlHandler *ac_handler = nullptr;

    std::unique_ptr<AudioSink> sink;
    data_type class_desc;
    bool streaming = false;

    // 收流速率闭环：主机每 URB 提交存在固定开销（usbip-win 收 RET → 重提交 →
    // TCP 往返，实测 ~60µs），按数据时长延迟响应会让接收速率恒低于消费速率，
    // 缓冲水位缓降导致周期性欠载（播放沙沙）。用墙钟窗口实测接收速率做反馈，
    // 修正响应延迟（负 = 提前响应），把接收速率锁到消费速率。单位 µs。
    // 原子：网络线程（handle 提交时算 data_duration）读、调度线程
    //（process_iso_out 闭环统计）写
    std::atomic<std::int64_t> pacing_delta_us{0};
    // 闭环测速：每 100 个 URB（约 1 秒）用 received 累计增量/墙钟时间算接收速率
    std::uint32_t urb_stat_count = 0;
    std::uint64_t urb_stat_bytes = 0;
    std::int64_t urb_stat_last_bytes = 0;   // 上一窗口的累计接收字节
    std::int64_t urb_stat_last_time_us = 0; // 上一窗口的墙钟时刻（steady_clock µs）
};

/// UAC 设备辅助类 — 在 device 上注册 AC/AS 接口 handler 并设置描述符
class USBIPDCPP_API UacDeviceHelper {
public:
    /// 向 device 注入 UAC 接口 handler（麦克风方向，与 setup_speaker 对称）。
    /// device 必须已有两个接口（AC + AS），且第二个接口需含 ISO IN 端点
    static void setup_microphone(std::shared_ptr<UsbDevice> device, StringPool &string_pool,
                                 std::unique_ptr<AudioSource> source, const UacDeviceConfig &config = {});

    /// 向 device 注入 UAC 接口 handler（扬声器方向，ISO OUT 收流）。
    /// device 必须已有两个接口（AC + AS），且第二个接口需含 ISO OUT 端点；
    /// AC 的 input_terminal_type 需为 TT_USB_STREAMING（扬声器拓扑）
    static void setup_speaker(std::shared_ptr<UsbDevice> device, StringPool &string_pool,
                              std::unique_ptr<AudioSink> sink, const UacDeviceConfig &config = {});
};

} // namespace usbipdcpp
