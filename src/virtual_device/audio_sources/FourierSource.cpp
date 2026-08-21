#include "usbipdcpp/virtual_device/audio_sources/FourierSource.h"

#include <algorithm>
#include <cmath>

namespace usbipdcpp {

FourierSource::FourierSource(std::vector<FourierHarmonic> harmonics, std::vector<std::uint32_t> sample_rates,
                             std::uint16_t channels) :
    harmonics(std::move(harmonics)), sample_rates(std::move(sample_rates)), channels(channels) {
    // 注意：初始化列表已完成 move，函数体内必须用 this-> 访问成员（参数已失效）
    // 过滤非法项：频率必须为整数 Hz（1 秒循环无缝）；
    // 幅度/相位非有限值（NaN/Inf）会污染整个输出（sin(NaN) 转整数是未定义行为），直接剔除
    std::erase_if(this->harmonics, [](const FourierHarmonic &h) {
        return h.frequency == 0 || !std::isfinite(h.amplitude) || !std::isfinite(h.phase);
    });
    for (auto &h: this->harmonics) {
        h.amplitude = std::clamp(h.amplitude, 0.0, 1.0);
    }
    // 过滤非法采样率（保留所有正值：UAC 设备支持任意采样率，8kHz 倍数的
    // 历史限制会让 44100 等非 8kHz 整数倍采样率被静默丢弃，见 SineWaveSource 注释）
    std::erase_if(this->sample_rates, [](std::uint32_t r) { return r == 0; });
    if (this->sample_rates.empty()) {
        this->sample_rates.push_back(48000);
    }
    // 首个为初始采样率（用户传入的第一个，不排序——排序会改变初始值）
    sample_rate = this->sample_rates.front();
    regenerate();
}

std::vector<AudioFormatInfo> FourierSource::supported_formats() const {
    std::vector<AudioFormatInfo> formats;
    formats.reserve(sample_rates.size());
    for (auto rate: sample_rates) {
        formats.push_back({channels, 16, rate});
    }
    return formats;
}

AudioFormatInfo FourierSource::current_format() const {
    return {channels, 16, sample_rate};
}

bool FourierSource::set_format(std::uint16_t new_channels, std::uint8_t bits_per_sample,
                               std::uint32_t new_sample_rate) {
    if (bits_per_sample != 16 || (new_channels != 1 && new_channels != 2)) {
        return false;
    }
    if (std::find(sample_rates.begin(), sample_rates.end(), new_sample_rate) == sample_rates.end()) {
        return false;
    }
    channels = new_channels;
    sample_rate = new_sample_rate;
    regenerate();
    return true;
}

bool FourierSource::get_chunk(AudioChunk &chunk) {
    chunk.data = buffer.data();
    chunk.size = buffer.size();
    return true;
}

void FourierSource::regenerate() {
    // 1 秒 PCM：各谐波频率为整数 Hz 时恰好包含整周期数，循环播放无缝
    auto total_samples = sample_rate * channels;
    buffer.resize(total_samples * 2);

    const double two_pi = 2.0 * 3.14159265358979323846;
    auto *dst = reinterpret_cast<std::int16_t *>(buffer.data());
    for (std::uint32_t sample = 0; sample < sample_rate; ++sample) {
        double value = 0.0;
        for (auto &h: harmonics) {
            // y(t) = Σ A_k·sin(2π·f_k·t + φ_k)，各谐波独立初始相位
            value += h.amplitude *
                     std::sin(two_pi * static_cast<double>(h.frequency) * sample / sample_rate + h.phase);
        }
        // 裁剪到满幅防溢出
        auto clamped = static_cast<std::int16_t>(
                std::clamp(value, -1.0, 1.0) * 32767.0);
        for (std::uint16_t ch = 0; ch < channels; ++ch) {
            dst[sample * channels + ch] = clamped;
        }
    }
}

} // namespace usbipdcpp
