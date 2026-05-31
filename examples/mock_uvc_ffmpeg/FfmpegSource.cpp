#include "FfmpegSource.h"

#include <spdlog/spdlog.h>

#include "virtual_device/UvcConstants.h"

namespace usbipdcpp {

static constexpr std::uint32_t INTERVAL_100NS(double fps) {
    return static_cast<std::uint32_t>(10'000'000.0 / fps);
}

FfmpegSource::FfmpegSource(std::string video_path, bool passthrough) :
    video_path_(std::move(video_path)), passthrough_(passthrough) {
    packet_ = av_packet_alloc();
    if (!packet_) {
        SPDLOG_ERROR("FFmpeg: failed to allocate packet");
        return;
    }
    if (!init())
        SPDLOG_ERROR("FFmpeg: failed to open {}", video_path_);
}

FfmpegSource::~FfmpegSource() {
    sws_freeContext(sws_ctx_);
    av_frame_free(&yuy2_frame_);
    av_frame_free(&av_frame_);
    avcodec_free_context(&codec_ctx_);
    avformat_close_input(&fmt_ctx_);
    av_packet_free(&packet_);
}

bool FfmpegSource::init() {
    if (avformat_open_input(&fmt_ctx_, video_path_.c_str(), nullptr, nullptr) < 0) {
        SPDLOG_ERROR("FFmpeg: cannot open {}", video_path_);
        return false;
    }
    if (avformat_find_stream_info(fmt_ctx_, nullptr) < 0) {
        SPDLOG_ERROR("FFmpeg: cannot find stream info");
        return false;
    }

    for (unsigned i = 0; i < fmt_ctx_->nb_streams; ++i) {
        if (fmt_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            stream_idx_ = static_cast<int>(i);
            break;
        }
    }
    if (stream_idx_ < 0) {
        SPDLOG_ERROR("FFmpeg: no video stream found");
        return false;
    }

    auto *par = fmt_ctx_->streams[stream_idx_]->codecpar;
    width_ = static_cast<std::uint16_t>(par->width);
    height_ = static_cast<std::uint16_t>(par->height);

    auto r_frame_rate = fmt_ctx_->streams[stream_idx_]->r_frame_rate;
    fps_ = av_q2d(r_frame_rate);
    if (fps_ <= 0)
        fps_ = 30.0;
    frame_interval_ = INTERVAL_100NS(fps_);

    auto codec_name = avcodec_get_name(par->codec_id);
    auto pix_fmt_name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(par->format));
    if (!codec_name) codec_name = "?";
    if (!pix_fmt_name) pix_fmt_name = "?";

    const char *out_fmt = "YUY2 (decode)";

    // passthrough 模式：MJPEG/H264 透传原始编码数据，其余解码 → YUY2
    // 默认关闭（全部解码为 YUY2），保证兼容所有 UVC 驱动
    if (passthrough_) {
        switch (par->codec_id) {
        case AV_CODEC_ID_MJPEG:
            fourcc_ = UvcFourCC::MJPEG;
            out_fmt = "MJPEG (passthrough)";
            max_frame_size_ = static_cast<std::size_t>(width_) * height_ * 2;
            buffer_.resize(max_frame_size_);
            SPDLOG_INFO("FFmpeg: {} — input: {} ({}) {}x{} @ {:.2f}fps → output: {}",
                        video_path_, codec_name, pix_fmt_name, width_, height_, fps_, out_fmt);
            init_ok_ = true;
            return true;
        case AV_CODEC_ID_H264:
            fourcc_ = UvcFourCC::H264;
            out_fmt = "H264 (passthrough)";
            max_frame_size_ = static_cast<std::size_t>(width_) * height_ * 2;
            buffer_.resize(max_frame_size_);
            SPDLOG_INFO("FFmpeg: {} — input: {} ({}) {}x{} @ {:.2f}fps → output: {}",
                        video_path_, codec_name, pix_fmt_name, width_, height_, fps_, out_fmt);
            init_ok_ = true;
            return true;
        default:
            SPDLOG_INFO("FFmpeg: {} — input: {} ({}), passthrough not supported for this codec, falling back to YUY2",
                        video_path_, codec_name, pix_fmt_name);
            break;
        }
    }

    // 解码 + swscale → YUY2
    passthrough_ = false;
    fourcc_ = UvcFourCC::YUY2;
    SPDLOG_INFO("FFmpeg: {} — input: {} ({}) {}x{} @ {:.2f}fps → output: {}",
                video_path_, codec_name, pix_fmt_name, width_, height_, fps_, out_fmt);

    codec_ = avcodec_find_decoder(par->codec_id);
    if (!codec_) {
        SPDLOG_ERROR("FFmpeg: unsupported codec");
        return false;
    }
    codec_ctx_ = avcodec_alloc_context3(codec_);
    if (!codec_ctx_)
        return false;
    if (avcodec_parameters_to_context(codec_ctx_, par) < 0)
        return false;
    if (avcodec_open2(codec_ctx_, codec_, nullptr) < 0) {
        SPDLOG_ERROR("FFmpeg: cannot open codec");
        return false;
    }

    av_frame_ = av_frame_alloc();
    yuy2_frame_ = av_frame_alloc();
    if (!av_frame_ || !yuy2_frame_)
        return false;

    yuy2_frame_->format = AV_PIX_FMT_YUYV422;
    yuy2_frame_->width = width_;
    yuy2_frame_->height = height_;
    av_image_alloc(yuy2_frame_->data, yuy2_frame_->linesize, width_, height_, AV_PIX_FMT_YUYV422, 1);

    sws_ctx_ = sws_getContext(width_, height_, codec_ctx_->pix_fmt, width_, height_, AV_PIX_FMT_YUYV422,
                              SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_ctx_) {
        SPDLOG_ERROR("FFmpeg: cannot create swscale context");
        return false;
    }

    max_frame_size_ = static_cast<std::size_t>(width_) * height_ * 2;
    buffer_.resize(max_frame_size_);

    init_ok_ = true;
    return true;
}

std::vector<VideoFormatInfo> FfmpegSource::supported_formats() const {
    if (!init_ok_) {
        // init 失败时返回一个合法 YUY2 格式，避免描述符全是零导致驱动报错
        return {{UvcFourCC::YUY2, 320, 240, 320u * 240 * 2, 333333u, 333333u, 3333333u, 16}};
    }
    auto min_iv = frame_interval_;
    auto max_iv = frame_interval_ * 10;
    auto max_size = static_cast<std::uint32_t>(max_frame_size_);
    std::uint8_t bpp = passthrough_ ? 0 : 16;
    return {{fourcc_, width_, height_, max_size, frame_interval_, min_iv, max_iv, bpp}};
}

VideoFormatInfo FfmpegSource::current_format() const {
    return supported_formats().front();
}

bool FfmpegSource::set_format(std::uint32_t fourcc, std::uint16_t width, std::uint16_t height,
                               std::uint32_t frame_interval) {
    if (!init_ok_)
        return false;
    if (fourcc != fourcc_)
        return false;
    if (width != width_ || height != height_)
        return false;
    frame_interval_ = frame_interval;
    return true;
}

bool FfmpegSource::get_frame(VideoFrame &frame) {
    if (!init_ok_)
        return false;
    if (passthrough_)
        return get_frame_passthrough(frame);
    return get_frame_yuy2(frame);
}

bool FfmpegSource::get_frame_passthrough(VideoFrame &frame) {
    while (true) {
        int ret = av_read_frame(fmt_ctx_, packet_);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                SPDLOG_DEBUG("FFmpeg: EOF, looping");
                seek_to_start();
                continue;
            }
            return false;
        }
        if (packet_->stream_index != stream_idx_) {
            av_packet_unref(packet_);
            continue;
        }

        if (static_cast<std::size_t>(packet_->size) > buffer_.size()) {
            buffer_.resize(packet_->size);
            if (packet_->size > static_cast<int>(max_frame_size_))
                max_frame_size_ = packet_->size;
        }
        std::memcpy(buffer_.data(), packet_->data, packet_->size);

        frame.data = buffer_.data();
        frame.size = static_cast<std::size_t>(packet_->size);
        frame.is_keyframe = (packet_->flags & AV_PKT_FLAG_KEY) != 0;

        av_packet_unref(packet_);
        return true;
    }
}

bool FfmpegSource::get_frame_yuy2(VideoFrame &frame) {
    if (!codec_ctx_)
        return false;

    while (true) {
        int ret = av_read_frame(fmt_ctx_, packet_);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                SPDLOG_DEBUG("FFmpeg: EOF, looping");
                seek_to_start();
                continue;
            }
            return false;
        }
        if (packet_->stream_index != stream_idx_) {
            av_packet_unref(packet_);
            continue;
        }

        ret = avcodec_send_packet(codec_ctx_, packet_);
        av_packet_unref(packet_);
        if (ret < 0)
            continue;

        ret = avcodec_receive_frame(codec_ctx_, av_frame_);
        if (ret == AVERROR(EAGAIN))
            continue;
        if (ret < 0)
            return false;

        sws_scale(sws_ctx_, av_frame_->data, av_frame_->linesize, 0, height_,
                  yuy2_frame_->data, yuy2_frame_->linesize);
        av_frame_unref(av_frame_);

        auto *src = yuy2_frame_->data[0];
        auto *dst = buffer_.data();
        auto line_sz = static_cast<std::size_t>(width_) * 2;
        for (int y = 0; y < height_; ++y) {
            std::memcpy(dst, src, line_sz);
            src += yuy2_frame_->linesize[0];
            dst += line_sz;
        }
        frame.data = buffer_.data();
        frame.size = line_sz * height_;
        frame.is_keyframe = true;
        return true;
    }
}

void FfmpegSource::seek_to_start() {
    if (!passthrough_)
        avcodec_flush_buffers(codec_ctx_);
    av_seek_frame(fmt_ctx_, stream_idx_, 0, AVSEEK_FLAG_BACKWARD);
}

std::size_t FfmpegSource::max_frame_size() const {
    return max_frame_size_;
}

std::uint32_t FfmpegSource::frame_interval() const {
    return frame_interval_;
}

} // namespace usbipdcpp
