#pragma once

#include <cstdint>
#include <vector>

#include "usbipdcpp/Export.h"
#include "usbipdcpp/virtual_device/audio_sources/AudioSource.h"

namespace usbipdcpp {

/// 傅里叶级数中的一项谐波
struct FourierHarmonic {
    std::uint32_t frequency = 0; // 频率（Hz），必须为整数（保证 1 秒循环无缝）
    double amplitude = 0.5; // 幅度，0.0~1.0 满幅比例
    double phase = 0.0; // 初始相位（弧度）
};

/// 傅里叶级数音源：y(t) = Σ A_k·sin(2π·f_k·t + φ_k)
/// 预生成 1 秒 PCM 循环播放，get_chunk 每次返回整段。
/// 仅支持 16 位有符号 PCM；各谐波频率需为整数 Hz（保证 1 秒内恰好整周期，循环拼接处无相位跳变）。
/// 叠加结果超出满幅时裁剪（防溢出），幅度由调用方控制。
/// 支持多个采样率（SET_CUR 可切换），采样率需为 8kHz 整数倍
/// （高速等时传输下每 microframe 数据量需为整数字节）。
class USBIPDCPP_API FourierSource : public AudioSource {
public:
    /// @param harmonics 谐波列表（频率/幅度/相位），为空时退化为静音
    /// @param sample_rates 支持的采样率列表（Hz，默认 {48000}），首个为初始采样率
    /// @param channels 声道数（1 或 2，默认 1）
    explicit FourierSource(std::vector<FourierHarmonic> harmonics = {}, std::vector<std::uint32_t> sample_rates = {48000},
                           std::uint16_t channels = 1);

    std::vector<AudioFormatInfo> supported_formats() const override;
    AudioFormatInfo current_format() const override;
    bool set_format(std::uint16_t channels, std::uint8_t bits_per_sample, std::uint32_t sample_rate) override;
    bool get_chunk(AudioChunk &chunk) override;

private:
    /// 重新生成 1 秒叠加 PCM
    void regenerate();

    std::vector<FourierHarmonic> harmonics;
    std::vector<std::uint32_t> sample_rates;
    std::uint32_t sample_rate;
    std::uint16_t channels;

    std::vector<std::uint8_t> buffer;
};

} // namespace usbipdcpp
