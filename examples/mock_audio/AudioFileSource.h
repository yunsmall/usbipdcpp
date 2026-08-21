#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "usbipdcpp/virtual_device/audio_sources/AudioSource.h"

namespace usbipdcpp {

/// 音频文件音源（基于 miniaudio，支持 WAV/MP3/FLAC/OGG Vorbis 等）。
/// mock_audio 示例私有类（2024-08 从虚拟设备库搬入），依赖 miniaudio（单头文件库，
/// 随 vcpkg 的 miniaudio 包安装；stb_vorbis.c 也在其 include 目录）。
/// 解码为 16 位 PCM 流式输出；支持采样率切换，重采样由 miniaudio 完成。
/// 播到文件末尾后循环播放（可通过 loop 关闭，关闭后输出静音）。
/// 初始化失败（文件打不开/格式不支持/采样率列表非法）不抛异常：
/// 打错误日志后进入降级状态，返回合法默认格式（同 FfmpegSource 的做法），
/// get_chunk 恒返回 false（调用方填静音），设备仍可正常枚举。
class AudioFileSource : public AudioSource {
public:
    /// @param path 音频文件路径（WAV/MP3/FLAC/OGG Vorbis）
    /// @param sample_rates 支持的采样率列表（Hz，任意正值，含非 8kHz 整数倍如 44100，
    ///                     由等时传输残差算法处理）。为空时仅支持文件原生采样率；
    ///                     非空时首个为初始采样率，列表内任意采样率由 miniaudio 重采样实现。
    /// @param loop 播到末尾后是否循环（默认循环）
    explicit AudioFileSource(std::string path, std::vector<std::uint32_t> sample_rates = {}, bool loop = true);
    ~AudioFileSource() override;

    // 持有 decoder 原始指针，禁止拷贝（浅拷贝会双释放）；同时也避免 dllexport 类
    // 在 Windows 下被强制实体化隐式拷贝构造，导致无 miniaudio 构建链接失败
    AudioFileSource(const AudioFileSource &) = delete;
    AudioFileSource &operator=(const AudioFileSource &) = delete;

    std::vector<AudioFormatInfo> supported_formats() const override;
    AudioFormatInfo current_format() const override;
    bool set_format(std::uint16_t channels, std::uint8_t bits_per_sample, std::uint32_t sample_rate) override;
    bool get_chunk(AudioChunk &chunk) override;

private:
    /// 按当前协商格式（重新）初始化解码器。失败返回 false
    bool open_decoder();

    std::string path;
    std::vector<std::uint32_t> sample_rates;
    bool loop;

    std::uint16_t file_channels = 1;
    std::uint32_t file_sample_rate = 48000;

    std::uint16_t channels = 1; // 当前协商声道数（等于 file_channels）
    std::uint32_t sample_rate = 48000; // 当前协商采样率
    bool init_ok = false; // 初始化成功标志，失败时降级为合法默认格式
    bool finished = false; // 非循环模式下已播完

    void *decoder = nullptr; // ma_decoder*，头文件不暴露 miniaudio
    std::vector<std::uint8_t> chunk_buffer; // get_chunk 输出缓冲区（1ms 数据）
};

} // namespace usbipdcpp
