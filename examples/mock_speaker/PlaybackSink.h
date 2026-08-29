#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "usbipdcpp/utils/RingBuffer.h"
#include "usbipdcpp/virtual_device/audio_sinks/AudioSink.h"

struct ma_device; // 前置声明（miniaudio 的 typedef 在全局命名空间）：头文件不暴露 miniaudio 类型

namespace usbipdcpp {

/// 本机声卡播放音频汇（基于 miniaudio，拉模型回调从环形缓冲取数据填声卡，
/// 缓冲不足时填静音）。采样率/声道协商变化时重启播放设备，重采样由声卡驱动处理。
/// 打开播放设备失败（无声卡等）不抛异常：打错误日志后进入丢弃模式
/// （write_pcm 仅计数），设备仍可正常枚举
class PlaybackSink : public AudioSink {
public:
    /// @param device_name 播放设备名（miniaudio 枚举名，默认空 = 系统默认输出设备）
    explicit PlaybackSink(std::string device_name = {});
    ~PlaybackSink() override;

    // 持有 ma_device 原始指针，禁止拷贝（浅拷贝会双释放）
    PlaybackSink(const PlaybackSink &) = delete;
    PlaybackSink &operator=(const PlaybackSink &) = delete;

    std::vector<AudioFormatInfo> supported_formats() const override;
    AudioFormatInfo current_format() const override;
    bool set_format(std::uint16_t channels, std::uint8_t bits_per_sample, std::uint32_t sample_rate) override;
    void write_pcm(const std::uint8_t *data, std::size_t size) override;
    void reset() override;

    /// 已收到的 PCM 字节总数（统计用）
    [[nodiscard]] std::uint64_t received_bytes() const;

private:
    void open_device(); // 按当前格式打开播放设备；失败进丢弃模式（调用方需已持有 mutex）
    void close_device();
    // miniaudio 拉模型回调（ma_device_data_proc 签名）
    static void data_callback(struct ma_device *device, void *output, const void *input,
                              std::uint32_t frame_count);

    std::string device_name;
    AudioFormatInfo format{1, 16, 48000};
    mutable std::mutex mutex; // 收流线程写缓冲 vs 播放回调读缓冲
    // 512KB 约 2.7s @96kHz 单声道（0.7s @48kHz 双声道）：吸收主机 URB 突发与
    // 网络尖峰的抖动余量；满则丢新数据（回调填静音）
    RingBuffer buffer{512 * 1024};
    std::uint64_t received = 0;
    bool discarding = false; // 播放设备不可用：仅计数

    void *device = nullptr; // ma_device*（new 分配，析构释放）
    bool device_initialized = false; // ma_device_init 成功后才 uninit
    void *context = nullptr; // ma_context*（--device 指定名字时自建，需活得比 device 久）
};

} // namespace usbipdcpp
