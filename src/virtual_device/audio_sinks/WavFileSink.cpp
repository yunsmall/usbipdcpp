#include "usbipdcpp/virtual_device/audio_sinks/WavFileSink.h"

#include <spdlog/spdlog.h>

namespace usbipdcpp {

WavFileSink::WavFileSink(std::filesystem::path path) : path(std::move(path)) {}

WavFileSink::~WavFileSink() {
    finalize();
}

std::vector<AudioFormatInfo> WavFileSink::supported_formats() const {
    // 常见采样率 × 单双声道（16 位 PCM）
    static const std::uint32_t rates[] = {8000, 16000, 22050, 32000, 44100, 48000, 96000};
    std::vector<AudioFormatInfo> fmts;
    for (auto rate: rates) {
        fmts.push_back({1, 16, rate});
        fmts.push_back({2, 16, rate});
    }
    return fmts;
}

AudioFormatInfo WavFileSink::current_format() const {
    std::lock_guard lock(mutex);
    return format;
}

bool WavFileSink::set_format(std::uint16_t channels, std::uint8_t bits_per_sample, std::uint32_t sample_rate) {
    std::lock_guard lock(mutex);
    // 只支持 16 位 PCM 单/双声道（WAV 头与写入路径固定）
    if (bits_per_sample != 16 || (channels != 1 && channels != 2)) {
        return false;
    }
    if (channels != format.channels || sample_rate != format.sample_rate) {
        // 格式变化：关闭当前文件（回填头），下次收流按新格式重建
        finalize_locked();
        format = {channels, bits_per_sample, sample_rate};
    }
    return true;
}

void WavFileSink::write_pcm(const std::uint8_t *data, std::size_t size) {
    std::lock_guard lock(mutex);
    if (!file_open) {
        open_file();
    }
    file.write(reinterpret_cast<const char *>(data), static_cast<std::streamsize>(size));
    data_size += static_cast<std::uint32_t>(size);
    // 边写边回填头：finalize/析构只在进程正常退出时触发，强杀（TerminateProcess）
    // 不会执行析构，头里的 data size 恒为占位 0 导致文件无法播放
    update_header_locked();
    auto mb = data_size / (1024 * 1024);
    if (mb > last_reported_mb) {
        last_reported_mb = mb;
        SPDLOG_INFO("WavFileSink: 已写入 {} MB", mb);
    }
}

void WavFileSink::reset() {
    std::lock_guard lock(mutex);
    finalize_locked();
}

void WavFileSink::finalize() {
    std::lock_guard lock(mutex);
    finalize_locked();
}

void WavFileSink::finalize_locked() {
    if (!file_open) {
        return;
    }
    // 回填头后关闭
    update_header_locked();
    file.close();
    file_open = false;
    data_size = 0;
    SPDLOG_INFO("WavFileSink: 已写入 {}", path.string());
}

void WavFileSink::update_header_locked() {
    // 回填 RIFF chunk size（offset 4）与 data size（offset 40）
    auto end = file.tellp();
    file.seekp(4);
    write_u32le(file, 36 + data_size);
    file.seekp(40);
    write_u32le(file, data_size);
    file.seekp(end);
}

void WavFileSink::open_file() {
    // 标准 44 字节 PCM WAV 头（字段大小占位，关闭时回填）
    file.open(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        SPDLOG_ERROR("WavFileSink: 无法打开输出文件 {}", path.string());
        return;
    }
    file.write("RIFF", 4);
    write_u32le(file, 0); // chunk size 占位
    file.write("WAVE", 4);
    file.write("fmt ", 4);
    write_u32le(file, 16); // fmt chunk size
    write_u16le(file, 1); // audio format: PCM
    write_u16le(file, format.channels);
    write_u32le(file, format.sample_rate);
    write_u32le(file, format.sample_rate * format.channels * 2); // byte rate
    write_u16le(file, static_cast<std::uint16_t>(format.channels * 2)); // block align
    write_u16le(file, format.bits_per_sample);
    file.write("data", 4);
    write_u32le(file, 0); // data size 占位
    file_open = true;
    SPDLOG_INFO("WavFileSink: 开始写入 {}（{}ch {}Hz）", path.string(), format.channels, format.sample_rate);
}

void WavFileSink::write_u32le(std::ofstream &file, std::uint32_t value) {
    std::uint8_t bytes[4] = {static_cast<std::uint8_t>(value & 0xFF),
                             static_cast<std::uint8_t>((value >> 8) & 0xFF),
                             static_cast<std::uint8_t>((value >> 16) & 0xFF),
                             static_cast<std::uint8_t>((value >> 24) & 0xFF)};
    file.write(reinterpret_cast<const char *>(bytes), 4);
}

void WavFileSink::write_u16le(std::ofstream &file, std::uint16_t value) {
    std::uint8_t bytes[2] = {static_cast<std::uint8_t>(value & 0xFF), static_cast<std::uint8_t>((value >> 8) & 0xFF)};
    file.write(reinterpret_cast<const char *>(bytes), 2);
}

} // namespace usbipdcpp
