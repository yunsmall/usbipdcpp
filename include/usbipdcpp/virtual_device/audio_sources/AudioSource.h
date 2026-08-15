#pragma once

#include <cstdint>
#include <vector>

#include "usbipdcpp/Export.h"

namespace usbipdcpp {

/// 音频源支持的格式信息
struct AudioFormatInfo {
    std::uint16_t channels = 1; // 声道数
    std::uint8_t bits_per_sample = 16; // 位深
    std::uint32_t sample_rate = 48000; // 采样率（Hz）
};

/// 一段 PCM 音频数据
struct AudioChunk {
    const std::uint8_t *data = nullptr; // 数据指针（由 AudioSource 管理生命周期）
    std::size_t size = 0; // 数据字节数
};

/// 音频帧源抽象接口
/// 实现类负责生成/读取 PCM 数据，UacHandler 负责打包成 UAC 协议发送
class USBIPDCPP_API AudioSource {
public:
    virtual ~AudioSource() = default;

    /// 返回源支持的所有格式列表
    virtual std::vector<AudioFormatInfo> supported_formats() const = 0;

    /// 返回当前协商格式
    virtual AudioFormatInfo current_format() const = 0;

    /// 协商格式变更（仅支持列表内的格式，失败返回 false）
    virtual bool set_format(std::uint16_t channels, std::uint8_t bits_per_sample, std::uint32_t sample_rate) = 0;

    /// 拉取一块 PCM 数据。
    /// 返回的 data 指针在下次 get_chunk 调用前有效，调用方在期间使用即可，无需释放。
    /// 无数据可提供时返回 false（调用方应填充静音）。
    virtual bool get_chunk(AudioChunk &chunk) = 0;
};

} // namespace usbipdcpp
