#include "usbipdcpp/virtual_device/audio_sources/SineWaveSource.h"

#include <algorithm>
#include <cmath>

namespace usbipdcpp {

SineWaveSource::SineWaveSource(std::uint32_t frequency_hz, std::vector<std::uint32_t> sample_rates,
                               std::uint16_t channels, double amplitude) :
    frequency_hz_(frequency_hz), sample_rates_(std::move(sample_rates)), channels_(channels), amplitude_(amplitude) {
    // 过滤非法采样率（需为 8kHz 整数倍），保持传入顺序
    std::erase_if(sample_rates_, [](std::uint32_t r) { return r == 0 || r % 8000 != 0; });
    if (sample_rates_.empty()) {
        sample_rates_.push_back(48000);
    }
    // 首个为初始采样率（用户传入的第一个，不排序——排序会改变初始值）
    sample_rate_ = sample_rates_.front();
    regenerate();
}

std::vector<AudioFormatInfo> SineWaveSource::supported_formats() const {
    std::vector<AudioFormatInfo> formats;
    formats.reserve(sample_rates_.size());
    for (auto rate: sample_rates_) {
        formats.push_back({channels_, 16, rate});
    }
    return formats;
}

AudioFormatInfo SineWaveSource::current_format() const {
    return {channels_, 16, sample_rate_};
}

bool SineWaveSource::set_format(std::uint16_t channels, std::uint8_t bits_per_sample, std::uint32_t sample_rate) {
    if (bits_per_sample != 16 || (channels != 1 && channels != 2)) {
        return false;
    }
    if (std::find(sample_rates_.begin(), sample_rates_.end(), sample_rate) == sample_rates_.end()) {
        return false;
    }
    channels_ = channels;
    sample_rate_ = sample_rate;
    regenerate();
    return true;
}

bool SineWaveSource::get_chunk(AudioChunk &chunk) {
    chunk.data = buffer_.data();
    chunk.size = buffer_.size();
    return true;
}

void SineWaveSource::regenerate() {
    // 1 秒 PCM：整数 Hz 时恰好包含整周期数，循环播放无缝
    auto total_samples = sample_rate_ * channels_;
    buffer_.resize(total_samples * 2);

    const double max_amp = 32767.0 * amplitude_;
    const double two_pi = 2.0 * 3.14159265358979323846;
    const double phase_step = two_pi * static_cast<double>(frequency_hz_) / static_cast<double>(sample_rate_);

    auto *dst = reinterpret_cast<std::int16_t *>(buffer_.data());
    for (std::uint32_t sample = 0; sample < sample_rate_; ++sample) {
        auto value = static_cast<std::int16_t>(max_amp * std::sin(phase_step * sample));
        for (std::uint16_t ch = 0; ch < channels_; ++ch) {
            dst[sample * channels_ + ch] = value;
        }
    }
}

} // namespace usbipdcpp
