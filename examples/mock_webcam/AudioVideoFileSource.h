#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

#include "usbipdcpp/virtual_device/audio_sources/AudioSource.h"
#include "usbipdcpp/virtual_device/video_sources/VideoSource.h"

namespace usbipdcpp {

/// 音画同步文件源共享状态：单 demuxer + 音/视频双解码器。
///
/// 同步语义：主时钟 = 音频已消费样本数/采样率（音频是连续消费的时钟源），
/// 视频取帧按主时钟对齐（返回"当前时刻应显示的那一帧"，过期帧快进丢弃，
/// 未到时刻的帧不提前出）。播放到文件尾音视频一起循环回开头，主时钟归零。
///
/// 线程安全：audio_get_chunk（UAC 调度线程）与 video_get_frame（UVC 网络
/// 线程）并发调用，内部互斥串行化 FFmpeg 调用
class AudioVideoFileState {
public:
    /// @param path 媒体文件路径（必须同时含视频轨与音轨）
    /// @param sample_rates 音频输出采样率列表（swresample 重采样目标，首个为初始值）
    explicit AudioVideoFileState(std::string path, std::vector<std::uint32_t> sample_rates);
    ~AudioVideoFileState();

    AudioVideoFileState(const AudioVideoFileState &) = delete;
    AudioVideoFileState &operator=(const AudioVideoFileState &) = delete;

    /// 打开与解码器初始化是否成功（音轨/视频轨缺失均视为失败）
    bool ok() const {
        return init_ok_;
    }

    // ========== VideoSource 侧（FfmpegVideoSourceView 调用） ==========

    /// 视频支持格式（单格式：文件宽高 + 文件帧率）
    std::vector<VideoFormatInfo> video_formats() const;

    /// 取主时钟时刻应显示的帧（无就绪帧返回 false，调用方按 UVC 丢帧处理）
    bool video_get_frame(VideoFrame &frame);

    std::size_t video_max_frame_size() const {
        return max_frame_size_;
    }

    /// 文件帧间隔（100ns 单位）
    std::uint32_t video_frame_interval() const {
        return frame_interval_;
    }

    // ========== AudioSource 侧（FfmpegAudioSourceView 调用） ==========

    /// 音频支持格式（文件声道数 × 采样率列表）
    std::vector<AudioFormatInfo> audio_formats() const;

    /// 当前协商格式
    AudioFormatInfo audio_current_format() const;

    /// 切换采样率（重建 swresample；声道/位深必须与文件一致）
    bool audio_set_format(std::uint16_t channels, std::uint8_t bits_per_sample, std::uint32_t sample_rate);

    /// 取一块 PCM（每块 1ms，对齐 AudioFileSource），推进主时钟
    bool audio_get_chunk(AudioChunk &chunk);

private:
    bool init();
    /// 锁内：读视频包解码一帧转 YUY2 入 pending 队列（EOF 循环 seek）
    bool decode_next_video_frame_locked();
    /// 锁内：视频解码器 seek 到目标时刻（flush + 清 pending，主时钟追帧用）
    void seek_video_to_locked(std::int64_t target_us);
    /// 锁内：音轨解码转 S16LE 填满 dst（不足静音补），推进主时钟
    void decode_audio_locked(std::int16_t *dst, std::size_t out_samples);
    /// 锁内：音频 demuxer + 解码器 seek 回开头，主时钟归零（音频循环）
    void seek_to_start_locked();

    std::string path_;
    std::vector<std::uint32_t> sample_rates_;

    // 双 demuxer：音视频各自独立读包游标。单 demuxer 时音频解码（跳过视频包）
    // 会把游标推到文件后方，视频解码永远追不上（画面停在首帧）；视频独立
    // demuxer 后可按主时钟自由 seek 追赶
    AVFormatContext *fmt_ctx_ = nullptr; // 音频专用（主时钟推进）
    AVFormatContext *video_fmt_ctx_ = nullptr; // 视频专用（按主时钟取帧）
    int video_stream_ = -1;
    int audio_stream_ = -1;
    AVCodecContext *video_codec_ = nullptr;
    AVCodecContext *audio_codec_ = nullptr;
    AVFrame *vframe_ = nullptr;
    AVFrame *yuy2_ = nullptr;
    SwsContext *sws_ = nullptr;
    SwrContext *swr_ = nullptr;
    AVFrame *aframe_ = nullptr;
    AVPacket *packet_ = nullptr; // 音频 demuxer 读包用
    AVPacket *video_packet_ = nullptr; // 视频 demuxer 读包用（与音频隔离）

    // 视频参数（init 时从文件取）
    std::uint16_t width_ = 0;
    std::uint16_t height_ = 0;
    double fps_ = 0;
    std::uint32_t frame_interval_ = 0;
    std::size_t max_frame_size_ = 0;

    // 音频参数（文件声道 + 当前采样率）
    std::uint16_t channels_ = 1;
    std::uint32_t sample_rate_ = 48000;
    std::vector<std::uint8_t> chunk_buffer_; // audio_get_chunk 返回的 1ms 块
    std::vector<std::uint8_t> vbuffer_; // video_get_frame 返回帧的存储（活到下次调用）
    std::vector<std::uint8_t> last_frame_; // 已显示帧缓存：下一帧未到时重放（画面等音频，不阻塞）

    std::mutex mutex_;
    // 主时钟：音频已消费样本数（audio_get_chunk 推进，单调递增）
    std::uint64_t audio_samples_ = 0;
    // 音频文件时长（µs，AV_TIME_BASE）：主时钟取模用——视频取帧位置 = 主时钟
    // 对文件时长取模，音频 EOF 循环（游标回 0 重新供给）与视频循环对齐
    std::int64_t audio_duration_us_ = 0;
    // 已解码待取视频帧 {pts(µs), YUY2 数据}，按解码序（B 帧 pts 可能回退，容忍）
    std::vector<std::pair<std::int64_t, std::vector<std::uint8_t>>> pending_frames_;
    // 视频解码位置（最后解码帧的 pts，µs）：落后主时钟超容差时 seek 追赶
    std::int64_t last_decoded_pts_us_ = -1;
    // 视频内容尾（EOF 前最后解码帧的 pts，µs）：主时钟取模基准是音频文件
    // 时长，可能超出视频实际内容尾（B 帧流最后帧 pts < 容器时长）——超出的
    // 窗口内视频无帧可出，get_frame 追帧会死循环，取帧侧把主时钟钳制到这里
    std::int64_t video_content_end_us_ = 0;

    bool init_ok_ = false;
};

/// 视频侧视图（VideoSource 接口），与音频视图共享同一文件源
class FfmpegVideoSourceView : public VideoSource {
public:
    explicit FfmpegVideoSourceView(std::shared_ptr<AudioVideoFileState> state);

    std::vector<VideoFormatInfo> supported_formats() const override;
    VideoFormatInfo current_format() const override;
    bool set_format(std::uint32_t fourcc, std::uint16_t width, std::uint16_t height,
                    std::uint32_t frame_interval) override;
    bool get_frame(VideoFrame &frame) override;
    std::size_t max_frame_size() const override;
    std::uint32_t frame_interval() const override;

private:
    std::shared_ptr<AudioVideoFileState> state_;
};

/// 音频侧视图（AudioSource 接口），与视频视图共享同一文件源
class FfmpegAudioSourceView : public AudioSource {
public:
    explicit FfmpegAudioSourceView(std::shared_ptr<AudioVideoFileState> state);

    std::vector<AudioFormatInfo> supported_formats() const override;
    AudioFormatInfo current_format() const override;
    bool set_format(std::uint16_t channels, std::uint8_t bits_per_sample, std::uint32_t sample_rate) override;
    bool get_chunk(AudioChunk &chunk) override;

private:
    std::shared_ptr<AudioVideoFileState> state_;
};

} // namespace usbipdcpp
