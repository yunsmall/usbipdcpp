// Vorbis 解码由外部 stb_vorbis 提供：miniaudio 检测到 STB_VORBIS_INCLUDE_STB_VORBIS_H
// 宏才编译 Vorbis 解码后端。直接包含 .c 实现文件是 stb 库的标准集成方式。
// 注意：必须放在所有系统头之前，其内部宏会与 Windows SDK 头冲突。
// stb 头文件布局随平台不同（vcpkg 在 include 根目录、Termux 在 include/stb/ 子目录），
// 用 __has_include 自适应；两种都没有时仅 OGG 解码不可用（WAV/MP3/FLAC 正常）
#if __has_include("stb_vorbis.c")
#include "stb_vorbis.c"
#elif __has_include("stb/stb_vorbis.c")
#include "stb/stb_vorbis.c"
#endif
// stb_vorbis.c 的播放矩阵宏（L/C/R）不清理，会污染后续包含的 Windows 头（winnt.h 位域成员同名）
#undef L
#undef C
#undef R

#include "AudioFileSource.h"

#include <algorithm>
#include <cstring>

#include "spdlog/spdlog.h"

#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

namespace usbipdcpp {

AudioFileSource::AudioFileSource(std::string path, std::vector<std::uint32_t> sample_rates, bool loop) :
    path(std::move(path)), sample_rates(std::move(sample_rates)), loop(loop) {

    // 先以文件原生格式打开一次，探测声道数与采样率
    {
        ma_decoder probe;
        auto cfg = ma_decoder_config_init(ma_format_s16, 0, 0);
        if (ma_decoder_init_file(this->path.c_str(), &cfg, &probe) != MA_SUCCESS) {
            SPDLOG_ERROR("音频文件无法打开: {}", this->path);
            return;
        }
        file_channels = static_cast<std::uint16_t>(probe.outputChannels);
        file_sample_rate = probe.outputSampleRate;
        ma_decoder_uninit(&probe);
    }

    if (this->sample_rates.empty()) {
        // 仅原生采样率：必须为 8kHz 整数倍（等时传输每 microframe 数据量需为整数字节）
        this->sample_rates.push_back(file_sample_rate);
    }
    for (auto r: this->sample_rates) {
        if (r == 0 || r % 8000 != 0) {
            SPDLOG_ERROR("采样率必须为 8kHz 整数倍: {}", r);
            return;
        }
    }

    // 首个采样率为初始值（与 SineWaveSource 一致）
    channels = file_channels;
    sample_rate = this->sample_rates.front();

    if (!open_decoder()) {
        SPDLOG_ERROR("解码器初始化失败: {}", this->path);
        return;
    }
    init_ok = true;
}

AudioFileSource::~AudioFileSource() {
    if (decoder) {
        ma_decoder_uninit(static_cast<ma_decoder *>(decoder));
        delete static_cast<ma_decoder *>(decoder);
    }
}

bool AudioFileSource::open_decoder() {
    auto *dec = new ma_decoder{};
    auto cfg = ma_decoder_config_init(ma_format_s16, file_channels, sample_rate);
    if (ma_decoder_init_file(path.c_str(), &cfg, dec) != MA_SUCCESS) {
        delete dec;
        return false;
    }
    if (decoder) {
        ma_decoder_uninit(static_cast<ma_decoder *>(decoder));
        delete static_cast<ma_decoder *>(decoder);
    }
    decoder = dec;
    finished = false;
    return true;
}

std::vector<AudioFormatInfo> AudioFileSource::supported_formats() const {
    if (!init_ok) {
        // 初始化失败时返回一个合法默认格式，避免描述符全是零导致驱动报错（同 FfmpegSource 做法）
        return {{1, 16, 48000}};
    }
    std::vector<AudioFormatInfo> formats;
    formats.reserve(sample_rates.size());
    for (auto rate: sample_rates) {
        formats.push_back({channels, 16, rate});
    }
    return formats;
}

AudioFormatInfo AudioFileSource::current_format() const {
    if (!init_ok) {
        return {1, 16, 48000};
    }
    return {channels, 16, sample_rate};
}

bool AudioFileSource::set_format(std::uint16_t new_channels, std::uint8_t bits_per_sample,
                                 std::uint32_t new_sample_rate) {
    if (!init_ok) {
        // 软失败状态：接受与降级格式一致的切换请求（no-op），保证驱动能正常开流（输出静音）
        return bits_per_sample == 16 && new_channels == 1 && new_sample_rate == 48000;
    }
    if (bits_per_sample != 16 || new_channels != file_channels) {
        return false;
    }
    if (std::find(sample_rates.begin(), sample_rates.end(), new_sample_rate) == sample_rates.end()) {
        return false;
    }
    if (new_sample_rate == sample_rate) {
        return true;
    }

    // 记录旧播放位置，重建解码器后按比例换算到新采样率。
    // 注意：Vorbis 解码器 get_cursor 恒为 0，位置保持仅对 WAV/MP3/FLAC 有效
    ma_uint64 old_pos = 0;
    if (decoder) {
        ma_decoder_get_cursor_in_pcm_frames(static_cast<ma_decoder *>(decoder), &old_pos);
    }
    auto old_rate = sample_rate;

    sample_rate = new_sample_rate;
    if (!open_decoder()) {
        // 重建失败回滚采样率，避免 current_format 与实际解码器状态不一致
        sample_rate = old_rate;
        return false;
    }
    auto new_pos = static_cast<ma_uint64>(static_cast<double>(old_pos) * new_sample_rate / old_rate);
    ma_decoder_seek_to_pcm_frame(static_cast<ma_decoder *>(decoder), new_pos);
    return true;
}

bool AudioFileSource::get_chunk(AudioChunk &chunk) {
    if (!init_ok) {
        return false;
    }
    auto *dec = static_cast<ma_decoder *>(decoder);
    if (finished) {
        return false;
    }

    // 每块 1ms 数据（采样率为 8kHz 整数倍，恰好整除）
    auto frames_per_chunk = static_cast<std::size_t>(sample_rate / 1000);
    chunk_buffer.resize(frames_per_chunk * channels * 2);
    auto *dst = reinterpret_cast<std::int16_t *>(chunk_buffer.data());

    std::size_t got = 0;
    bool looped_once = false;
    while (got < frames_per_chunk) {
        ma_uint64 frames_read = 0;
        ma_decoder_read_pcm_frames(dec, dst + got * channels, frames_per_chunk - got, &frames_read);
        if (frames_read == 0) {
            if (loop && !looped_once) {
                // 循环播放：回到开头继续读（空文件回绕后仍无数据则输出静音）
                ma_decoder_seek_to_pcm_frame(dec, 0);
                looped_once = true;
                continue;
            }
            finished = true;
            break;
        }
        got += static_cast<std::size_t>(frames_read);
    }

    if (got == 0) {
        return false;
    }
    // 末尾不足 1ms 时返回短块，调用方按 chunk.size 消费，剩余按静音处理
    chunk.data = chunk_buffer.data();
    chunk.size = got * channels * 2;
    return true;
}

} // namespace usbipdcpp
