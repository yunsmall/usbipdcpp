#pragma once

#include <cstdint>
#include <vector>

#include "usbipdcpp/Export.h"
#include "usbipdcpp/virtual_device/audio_sources/AudioSource.h"

namespace usbipdcpp {

/// 正弦波测试音源
/// 预生成 1 秒 PCM 循环播放，get_chunk 每次返回整段。
/// 仅支持 16 位有符号 PCM，频率需为整数 Hz（保证 1 秒内恰好整周期，循环拼接处无相位跳变）。
/// 支持多个采样率（SET_CUR 可切换），采样率需为 8kHz 整数倍
/// （高速等时传输下每 microframe 数据量需为整数字节）。
class USBIPDCPP_API SineWaveSource : public AudioSource {
public:
    /// @param frequency_hz 正弦频率（整数 Hz，默认 440）
    /// @param sample_rates 支持的采样率列表（Hz，默认 {48000}），首个为初始采样率
    /// @param channels 声道数（1 或 2，默认 1）
    /// @param amplitude 幅度，0.0~1.0 满幅比例（默认 0.5）
    explicit SineWaveSource(std::uint32_t frequency_hz = 440, std::vector<std::uint32_t> sample_rates = {48000},
                            std::uint16_t channels = 1, double amplitude = 0.5);

    std::vector<AudioFormatInfo> supported_formats() const override;
    AudioFormatInfo current_format() const override;
    bool set_format(std::uint16_t channels, std::uint8_t bits_per_sample, std::uint32_t sample_rate) override;
    bool get_chunk(AudioChunk &chunk) override;

private:
    /// 重新生成 1 秒正弦 PCM
    void regenerate();

    std::uint32_t frequency_hz_;
    std::vector<std::uint32_t> sample_rates_;
    std::uint32_t sample_rate_;
    std::uint16_t channels_;
    double amplitude_;

    std::vector<std::uint8_t> buffer_;
};

} // namespace usbipdcpp
