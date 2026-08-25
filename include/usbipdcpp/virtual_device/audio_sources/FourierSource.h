#pragma once

#include <cstdint>
#include <vector>

#include "usbipdcpp/Export.h"
#include "usbipdcpp/virtual_device/audio_sources/AudioSource.h"

namespace usbipdcpp {

/// 傅里叶级数中的一项谐波
struct FourierHarmonic {
    std::uint32_t frequency = 0; // 频率（Hz），必须为整数（保证 1 秒循环无缝）
    double amplitude = 0.5; // 幅度，-1.0~1.0 满幅比例（负值表示反相，等价于相位翻转 π）
    double phase = 0.0; // 初始相位（弧度）
};

/// 傅里叶级数音源：y(t) = Σ A_k·sin(2π·f_k·t + φ_k)
/// 预生成 1 秒 PCM 循环播放，get_chunk 每次返回整段。
/// 仅支持 16 位有符号 PCM；各谐波频率需为整数 Hz（保证 1 秒内恰好整周期，循环拼接处无相位跳变）。
/// 叠加峰值超过满幅时的处理由 normalize 决定：整体除以峰值（波形形状不变）
/// 或逐采样点削波，幅度由调用方控制。
/// 支持多个采样率（SET_CUR 可切换），采样率需为 8kHz 整数倍
/// （高速等时传输下每 microframe 数据量需为整数字节）。
class USBIPDCPP_API FourierSource : public AudioSource {
public:
    /// @param harmonics 谐波列表（频率/幅度/相位），为空时退化为静音
    /// @param sample_rates 支持的采样率列表（Hz，默认 {48000}），首个为初始采样率
    /// @param channels 声道数（1 或 2，默认 1）
    /// @param normalize 峰值超过满幅时的处理：true（默认）所有采样点整体除以峰值
    /// （保持波形形状不削波；峰值不超满幅时不动，幅度语义保持）；false 逐采样点
    /// 削波到满幅
    explicit FourierSource(std::vector<FourierHarmonic> harmonics = {}, std::vector<std::uint32_t> sample_rates = {48000},
                           std::uint16_t channels = 1, bool normalize = true);

    std::vector<AudioFormatInfo> supported_formats() const override;
    AudioFormatInfo current_format() const override;
    bool set_format(std::uint16_t channels, std::uint8_t bits_per_sample, std::uint32_t sample_rate) override;
    bool get_chunk(AudioChunk &chunk) override;

    /// 重新设置防溢出方案（超限时整体除以峰值 / 逐采样点削波），立即重新生成 PCM
    /// @param normalize true（默认）峰值超过满幅时所有采样点整体除以峰值（保持波形形状
    /// 不削波；峰值不超满幅时不动）；false 逐采样点削波到满幅
    void set_normalize(bool normalize);

private:
    /// 重新生成 1 秒叠加 PCM
    void regenerate();

    std::vector<FourierHarmonic> harmonics;
    std::vector<std::uint32_t> sample_rates;
    std::uint32_t sample_rate;
    std::uint16_t channels;
    bool normalize;

    std::vector<std::uint8_t> buffer;
};

} // namespace usbipdcpp
