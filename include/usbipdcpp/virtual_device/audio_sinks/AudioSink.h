#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "usbipdcpp/Export.h"
#include "usbipdcpp/virtual_device/audio_sources/AudioSource.h"

namespace usbipdcpp {

/// 音频帧消费端抽象接口
/// 与 AudioSource（生成 PCM）对称：实现类负责接收/播放/转存 PCM 数据，
/// UacAudioStreamingSinkHandler 把 iso OUT 收流写入 Sink
class USBIPDCPP_API AudioSink {
public:
    virtual ~AudioSink() = default;

    /// 返回 Sink 支持的所有格式列表
    virtual std::vector<AudioFormatInfo> supported_formats() const = 0;

    /// 返回当前协商格式
    virtual AudioFormatInfo current_format() const = 0;

    /// 协商格式变更（仅支持列表内的格式，失败返回 false）
    virtual bool set_format(std::uint16_t channels, std::uint8_t bits_per_sample, std::uint32_t sample_rate) = 0;

    /// 写入一段 PCM 数据（iso OUT 收流线程调用，实现类自行保证线程安全）
    virtual void write_pcm(const std::uint8_t *data, std::size_t size) = 0;

    /// 断连/流停止时清空缓冲，保证下次连接不播放旧数据（默认空实现）
    virtual void reset() {}
};

} // namespace usbipdcpp
