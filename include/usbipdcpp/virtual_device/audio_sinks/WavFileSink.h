#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <vector>

#include "usbipdcpp/virtual_device/audio_sinks/AudioSink.h"

namespace usbipdcpp {

/// 把收流 PCM 写成 WAV 文件的音频汇（PCM 16 位小端）
/// 收流开始时自动创建文件，每次写入后回填 RIFF 与 data 长度字段（运行期间
/// 文件即可播放，强杀进程不会丢失头）。格式协商变化时关闭当前文件，下次
/// 收流重新创建
class USBIPDCPP_API WavFileSink : public AudioSink {
public:
    explicit WavFileSink(std::filesystem::path path);
    ~WavFileSink() override;

    std::vector<AudioFormatInfo> supported_formats() const override;
    AudioFormatInfo current_format() const override;
    bool set_format(std::uint16_t channels, std::uint8_t bits_per_sample, std::uint32_t sample_rate) override;
    void write_pcm(const std::uint8_t *data, std::size_t size) override;
    void reset() override;

    /// 关闭文件（幂等，析构自动调用）
    void finalize();

private:
    void open_file();
    void finalize_locked(); // 调用方必须已持有 mutex
    void update_header_locked(); // 回填头长度字段，调用方必须已持有 mutex
    static void write_u32le(std::ofstream &file, std::uint32_t value);
    static void write_u16le(std::ofstream &file, std::uint16_t value);

    std::filesystem::path path;
    mutable std::mutex mutex; // 收流线程写文件 vs 外部 finalize 并发
    std::ofstream file;
    AudioFormatInfo format{1, 16, 48000};
    std::uint32_t data_size = 0; // 已写入的 PCM 字节数
    std::uint32_t last_reported_mb = 0; // 上次打日志的整 MB 数（只打一次阈值）
    bool file_open = false;
};

} // namespace usbipdcpp
