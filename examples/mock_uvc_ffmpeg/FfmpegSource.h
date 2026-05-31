#pragma once

#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include "virtual_device/video_sources/VideoSource.h"

namespace usbipdcpp {

/// 基于 FFmpeg 的视频源。
/// passthrough=false（默认）：全部解码为 YUY2，兼容所有 UVC 驱动。
/// passthrough=true：MJPEG/H264 直接透传，其他解码为 YUY2。
class FfmpegSource : public VideoSource {
public:
    explicit FfmpegSource(std::string video_path, bool passthrough = false);
    ~FfmpegSource() override;

    std::vector<VideoFormatInfo> supported_formats() const override;
    VideoFormatInfo current_format() const override;
    bool set_format(std::uint32_t fourcc, std::uint16_t width, std::uint16_t height,
                    std::uint32_t frame_interval) override;
    bool get_frame(VideoFrame &frame) override;
    std::size_t max_frame_size() const override;
    std::uint32_t frame_interval() const override;

private:
    bool init();
    bool get_frame_passthrough(VideoFrame &frame);
    bool get_frame_yuy2(VideoFrame &frame);
    void seek_to_start();

    std::string video_path_;
    std::vector<std::uint8_t> buffer_;
    std::size_t max_frame_size_{};

    bool init_ok_ = false;

    std::uint16_t width_{};
    std::uint16_t height_{};
    double fps_{};
    std::uint32_t frame_interval_{};
    std::uint32_t fourcc_{};
    bool passthrough_ = false;

    AVFormatContext *fmt_ctx_ = nullptr;
    AVCodecContext *codec_ctx_ = nullptr;
    const AVCodec *codec_ = nullptr;
    AVFrame *av_frame_ = nullptr;
    AVFrame *yuy2_frame_ = nullptr;
    SwsContext *sws_ctx_ = nullptr;
    AVPacket *packet_ = nullptr;
    int stream_idx_ = -1;
};

} // namespace usbipdcpp
