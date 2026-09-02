#include "AudioVideoFileSource.h"

#include <algorithm>
#include <cstring>

#include <spdlog/spdlog.h>

#include "usbipdcpp/virtual_device/UvcConstants.h"

namespace usbipdcpp {

static constexpr std::uint32_t INTERVAL_100NS(double fps) {
    return static_cast<std::uint32_t>(10'000'000.0 / fps);
}

// ==================== AudioVideoFileState ====================

AudioVideoFileState::AudioVideoFileState(std::string path, std::vector<std::uint32_t> sample_rates) :
    path_(std::move(path)), sample_rates_(std::move(sample_rates)) {
    if (sample_rates_.empty()) {
        SPDLOG_ERROR("AudioVideoFileState: sample rate list must not be empty");
        return;
    }
    sample_rate_ = sample_rates_.front();
    packet_ = av_packet_alloc();
    video_packet_ = av_packet_alloc();
    if (!packet_ || !video_packet_) {
        SPDLOG_ERROR("FFmpeg: failed to allocate packet");
        return;
    }
    if (!init())
        SPDLOG_ERROR("FFmpeg: failed to open {}", path_);
}

AudioVideoFileState::~AudioVideoFileState() {
    sws_freeContext(sws_);
    swr_free(&swr_);
    av_frame_free(&yuy2_);
    av_frame_free(&aframe_);
    av_frame_free(&vframe_);
    avcodec_free_context(&audio_codec_);
    avcodec_free_context(&video_codec_);
    avformat_close_input(&video_fmt_ctx_);
    avformat_close_input(&fmt_ctx_);
    av_packet_free(&video_packet_);
    av_packet_free(&packet_);
}

bool AudioVideoFileState::init() {
    if (avformat_open_input(&fmt_ctx_, path_.c_str(), nullptr, nullptr) < 0) {
        SPDLOG_ERROR("FFmpeg: cannot open {}", path_);
        return false;
    }
    if (avformat_find_stream_info(fmt_ctx_, nullptr) < 0) {
        SPDLOG_ERROR("FFmpeg: cannot find stream info");
        return false;
    }
    // 视频专用 demuxer：与音频 demuxer 各自独立游标（音视频按各自节奏推进，
    // 互不干扰；视频按主时钟自由 seek 追赶）
    if (avformat_open_input(&video_fmt_ctx_, path_.c_str(), nullptr, nullptr) < 0) {
        SPDLOG_ERROR("FFmpeg: cannot open {} (video demuxer)", path_);
        return false;
    }
    if (avformat_find_stream_info(video_fmt_ctx_, nullptr) < 0) {
        SPDLOG_ERROR("FFmpeg: cannot find stream info (video demuxer)");
        return false;
    }

    for (unsigned i = 0; i < video_fmt_ctx_->nb_streams; ++i) {
        if (video_fmt_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_ = static_cast<int>(i);
            break;
        }
    }
    for (unsigned i = 0; i < fmt_ctx_->nb_streams; ++i) {
        if (fmt_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audio_stream_ = static_cast<int>(i);
            break;
        }
    }
    // 音画同步的前提：文件必须同时有视频轨和音轨
    if (video_stream_ < 0) {
        SPDLOG_ERROR("FFmpeg: no video stream found in {}", path_);
        return false;
    }
    if (audio_stream_ < 0) {
        SPDLOG_ERROR("FFmpeg: no audio stream found in {}（音画同步需要音轨）", path_);
        return false;
    }

    // ---- 视频解码器 + swscale（对齐 FfmpegSource 的 YUY2 路径） ----
    auto *vpar = video_fmt_ctx_->streams[video_stream_]->codecpar;
    width_ = static_cast<std::uint16_t>(vpar->width);
    height_ = static_cast<std::uint16_t>(vpar->height);
    auto r_frame_rate = video_fmt_ctx_->streams[video_stream_]->r_frame_rate;
    fps_ = av_q2d(r_frame_rate);
    if (fps_ <= 0)
        fps_ = 30.0;
    frame_interval_ = INTERVAL_100NS(fps_);

    auto *vcodec = avcodec_find_decoder(vpar->codec_id);
    if (!vcodec) {
        SPDLOG_ERROR("FFmpeg: unsupported video codec");
        return false;
    }
    video_codec_ = avcodec_alloc_context3(vcodec);
    vframe_ = av_frame_alloc();
    yuy2_ = av_frame_alloc();
    if (!video_codec_ || !vframe_ || !yuy2_)
        return false;
    if (avcodec_parameters_to_context(video_codec_, vpar) < 0)
        return false;
    if (avcodec_open2(video_codec_, vcodec, nullptr) < 0) {
        SPDLOG_ERROR("FFmpeg: cannot open video codec");
        return false;
    }

    yuy2_->format = AV_PIX_FMT_YUYV422;
    yuy2_->width = width_;
    yuy2_->height = height_;
    if (av_image_alloc(yuy2_->data, yuy2_->linesize, width_, height_, AV_PIX_FMT_YUYV422, 1) < 0)
        return false;
    sws_ = sws_getContext(width_, height_, video_codec_->pix_fmt, width_, height_, AV_PIX_FMT_YUYV422,
                          SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!sws_) {
        SPDLOG_ERROR("FFmpeg: cannot create swscale context");
        return false;
    }
    max_frame_size_ = static_cast<std::size_t>(width_) * height_ * 2;

    // ---- 音频解码器 + swresample（输出 S16LE × 文件声道 × 当前采样率） ----
    auto *apar = fmt_ctx_->streams[audio_stream_]->codecpar;
    audio_duration_us_ = fmt_ctx_->duration > 0 ? fmt_ctx_->duration : 0;
    channels_ = static_cast<std::uint16_t>(apar->ch_layout.nb_channels > 0 ? apar->ch_layout.nb_channels : 1);
    auto *acodec = avcodec_find_decoder(apar->codec_id);
    if (!acodec) {
        SPDLOG_ERROR("FFmpeg: unsupported audio codec");
        return false;
    }
    audio_codec_ = avcodec_alloc_context3(acodec);
    aframe_ = av_frame_alloc();
    if (!audio_codec_ || !aframe_)
        return false;
    if (avcodec_parameters_to_context(audio_codec_, apar) < 0)
        return false;
    if (avcodec_open2(audio_codec_, acodec, nullptr) < 0) {
        SPDLOG_ERROR("FFmpeg: cannot open audio codec");
        return false;
    }

    // 输出声道布局：文件声道（av_channel_layout_default 支持 1/2 声道）
    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, channels_);
    if (swr_alloc_set_opts2(&swr_, &out_layout, AV_SAMPLE_FMT_S16, sample_rate_, &audio_codec_->ch_layout,
                            audio_codec_->sample_fmt, audio_codec_->sample_rate, 0, nullptr) < 0) {
        SPDLOG_ERROR("FFmpeg: cannot create swresample context");
        return false;
    }
    if (swr_init(swr_) < 0) {
        SPDLOG_ERROR("FFmpeg: cannot init swresample");
        return false;
    }

    auto codec_name = avcodec_get_name(vpar->codec_id);
    SPDLOG_INFO("FFmpeg: {} — video {}x{} @ {:.2f}fps, audio {}ch {}Hz → S16LE {}Hz", path_, width_, height_,
                fps_, channels_, audio_codec_->sample_rate, sample_rate_);

    init_ok_ = true;
    return true;
}

std::vector<VideoFormatInfo> AudioVideoFileState::video_formats() const {
    if (!init_ok_) {
        // init 失败时返回一个合法 YUY2 格式，避免描述符全是零导致驱动报错（同 FfmpegSource）
        return {{UvcFourCC::YUY2, 320, 240, 320u * 240 * 2, 333333u, 333333u, 3333333u, 16}};
    }
    auto min_iv = frame_interval_;
    auto max_iv = frame_interval_ * 10;
    return {{UvcFourCC::YUY2, width_, height_, static_cast<std::uint32_t>(max_frame_size_), frame_interval_,
             min_iv, max_iv, 16}};
}

bool AudioVideoFileState::decode_next_video_frame_locked() {
    while (true) {
        int ret = av_read_frame(video_fmt_ctx_, video_packet_);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                SPDLOG_INFO("FFmpeg: video ended, looping from start");
                // 记录视频内容尾（EOF 前最后解码帧 pts）：主时钟取模基准是
                // 音频文件时长，会超出视频实际内容尾，取帧侧靠它钳制主时钟
                if (last_decoded_pts_us_ > 0)
                    video_content_end_us_ = last_decoded_pts_us_;
                // 循环回文件头：清掉未弹完的尾帧（EOF 触发时主时钟快照可能
                // 已过内容尾，尾帧永远 ≤ 主时钟，不清会让下次调用死等重放）
                pending_frames_.clear();
                // 视频循环回文件头：视频侧全部对象（demuxer、解码器、sws、
                // frame 缓冲）与 init 一致整套重建。实测 EOF 状态下只 seek 会让
                // mov demuxer 游标回绕不彻底（读几个包又 EOF）；复用旧解码器/
                // sws 会进坏状态（avcodec_send_packet / sws_scale 内部卡死不返回，
                // get_frame 持锁卡死整条 UVC 拉流）。整套重建 100% 干净，开销
                // ~1ms 每文件时长一次，可忽略
                avcodec_free_context(&video_codec_);
                video_codec_ = nullptr;
                sws_freeContext(sws_);
                sws_ = nullptr;
                av_frame_free(&vframe_);
                vframe_ = nullptr;
                av_frame_free(&yuy2_);
                yuy2_ = nullptr;
                avformat_close_input(&video_fmt_ctx_);
                video_fmt_ctx_ = nullptr;
                video_stream_ = -1;
                if (avformat_open_input(&video_fmt_ctx_, path_.c_str(), nullptr, nullptr) < 0 ||
                    avformat_find_stream_info(video_fmt_ctx_, nullptr) < 0) {
                    SPDLOG_ERROR("FFmpeg: cannot reopen {} (video loop)", path_);
                    return false;
                }
                for (unsigned i = 0; i < video_fmt_ctx_->nb_streams; ++i) {
                    if (video_fmt_ctx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
                        video_stream_ = static_cast<int>(i);
                        break;
                    }
                }
                if (video_stream_ < 0) {
                    SPDLOG_ERROR("FFmpeg: no video stream after reopen");
                    return false;
                }
                // 重建解码器 + sws + 帧缓冲（同文件参数不变，宽高沿用）
                auto *vpar = video_fmt_ctx_->streams[video_stream_]->codecpar;
                auto *vcodec = avcodec_find_decoder(vpar->codec_id);
                vframe_ = av_frame_alloc();
                yuy2_ = av_frame_alloc();
                if (!vcodec || !vframe_ || !yuy2_) {
                    SPDLOG_ERROR("FFmpeg: cannot alloc video loop objects");
                    return false;
                }
                video_codec_ = avcodec_alloc_context3(vcodec);
                if (!video_codec_ || avcodec_parameters_to_context(video_codec_, vpar) < 0 ||
                    avcodec_open2(video_codec_, vcodec, nullptr) < 0) {
                    SPDLOG_ERROR("FFmpeg: cannot reopen video codec (loop)");
                    return false;
                }
                yuy2_->format = AV_PIX_FMT_YUYV422;
                yuy2_->width = width_;
                yuy2_->height = height_;
                if (av_image_alloc(yuy2_->data, yuy2_->linesize, width_, height_, AV_PIX_FMT_YUYV422, 1) < 0) {
                    SPDLOG_ERROR("FFmpeg: cannot alloc yuy2 buffer (loop)");
                    return false;
                }
                sws_ = sws_getContext(width_, height_, video_codec_->pix_fmt, width_, height_, AV_PIX_FMT_YUYV422,
                                      SWS_BILINEAR, nullptr, nullptr, nullptr);
                if (!sws_) {
                    SPDLOG_ERROR("FFmpeg: cannot create swscale (loop)");
                    return false;
                }
                last_decoded_pts_us_ = -1;
                // EOF 触发的这次 get_frame 里主时钟快照可能已超出视频内容尾
                //（取模基准是音频时长），追帧永远追不上会死循环；重建后直接
                // 返回，本次调用出已弹帧/重放上一帧，下次调用用钳制后的主时钟
                // 从文件头正常追
                return false;
            }
            return false;
        }
        if (video_packet_->stream_index != video_stream_) {
            av_packet_unref(video_packet_);
            continue;
        }
        ret = avcodec_send_packet(video_codec_, video_packet_);
        av_packet_unref(video_packet_);
        if (ret < 0)
            continue;
        ret = avcodec_receive_frame(video_codec_, vframe_);
        if (ret == AVERROR(EAGAIN))
            continue;
        if (ret < 0)
            return false;

        // pts → µs（NOPTS 帧视为立即可显示：pts 记 -1，取帧侧按"已到"处理）
        std::int64_t pts_us = -1;
        if (vframe_->pts != AV_NOPTS_VALUE) {
            pts_us = av_rescale_q(vframe_->pts, video_fmt_ctx_->streams[video_stream_]->time_base,
                                  AVRational{1, 1'000'000});
        }
        last_decoded_pts_us_ = pts_us;

        std::vector<std::uint8_t> yuy2(max_frame_size_);
        sws_scale(sws_, vframe_->data, vframe_->linesize, 0, height_, yuy2_->data, yuy2_->linesize);
        av_frame_unref(vframe_);

        // 行拷贝到紧凑缓冲（sws 输出 linesize 可能带对齐填充）
        auto *src = yuy2_->data[0];
        auto line_sz = static_cast<std::size_t>(width_) * 2;
        auto *dst = yuy2.data();
        for (int y = 0; y < height_; ++y) {
            std::memcpy(dst, src, line_sz);
            src += yuy2_->linesize[0];
            dst += line_sz;
        }
        // 按 pts 插入排序：mpeg4 等含 B 帧的编码解码序 pts 会回退，取帧按
        // 显示序（pts）判断快进/播放
        auto it = std::lower_bound(pending_frames_.begin(), pending_frames_.end(), pts_us,
                                   [](const auto &a, std::int64_t pts) { return a.first < pts; });
        pending_frames_.insert(it, {pts_us, std::move(yuy2)});
        return true;
    }
}

bool AudioVideoFileState::video_get_frame(VideoFrame &frame) {
    std::lock_guard lock(mutex_);
    if (!init_ok_)
        return false;
    // 主时钟（µs）：由音频消费推进；对音频文件时长取模——视频取帧位置与
    // 音频循环点对齐（音频 EOF 循环供给、视频 EOF 循环，各自循环周期都是
    // 文件时长）
    auto clock_us = static_cast<std::int64_t>(audio_samples_ * 1'000'000ULL / sample_rate_);
    if (audio_duration_us_ > 0)
        clock_us %= audio_duration_us_;
    // 主时钟钳制到视频内容尾前：取模基准是音频时长，会超出视频最后一帧
    // pts（B 帧流最后帧 < 容器时长）——超出窗口内视频无帧可出，追帧循环
    // 解到 EOF 重建回 0 又全 ≤ clock，永远弹不空 → 死循环持锁。留 1 帧裕量，
    // 循环点前后视频都追得上（丢的文件尾最后 1 帧由 EOF 重建衔接）
    if (video_content_end_us_ > 0) {
        auto clock_limit = video_content_end_us_ - frame_interval_ / 10;
        if (clock_us > clock_limit)
            clock_us = clock_limit;
    }
    // 视频解码位置落后主时钟超过容差时 seek 追赶（音频先流/快进场景：
    // 顺序解码追不上；容差内顺序解，避免频繁 seek）
    constexpr std::int64_t SEEK_TOLERANCE_US = 500'000;
    // 本次调用是否需 seek：进入时解码位置落后主时钟超容差才 seek（音频先流/
    // 快进场景）。注意只在进入时决定一次——追帧循环内重复判断会在 seek 后
    // 解出的低 pts 帧上误判"仍落后"而反复 seek（关键帧在文件头时每轮解出
    // pts=0 的帧，无限循环）
    bool need_seek = (last_decoded_pts_us_ < 0 || clock_us - last_decoded_pts_us_ > SEEK_TOLERANCE_US);
    // 本次调用已弹出的"最新已到帧"：弹出后继续解码追主时钟（快进），
    // 直到队头超前或解码失败——不能每帧都返回，否则视频逐帧追（解码速率
    // = 拉帧速率 = 播放速率，落后永远追不上）
    std::pair<std::int64_t, std::vector<std::uint8_t>> latest{-1, {}};
    bool have_latest = false;
    while (true) {
        if (pending_frames_.empty()) {
            if (need_seek) {
                seek_video_to_locked(clock_us);
                need_seek = false;
            }
            if (!decode_next_video_frame_locked())
                break; // 解码失败：返回已弹出的帧（如有）
            continue;
        }
        if (pending_frames_.front().first > clock_us)
            break; // 队头超前（还没到播放时刻）：latest 就是当前应显示的帧
        latest = std::move(pending_frames_.front());
        pending_frames_.erase(pending_frames_.begin());
        have_latest = true;
        // continue：弹出后队列空时回循环开头解码追帧（快进到主时钟）
    }
    if (!have_latest) {
        // 无已到帧（理论上只有 seek 后首帧解码超前才发生）：重放上一帧
        //（UVC 是拉模型，无帧会让驱动拉流卡住）
        if (!last_frame_.empty()) {
            frame.data = last_frame_.data();
            frame.size = last_frame_.size();
            frame.is_keyframe = true;
            return true;
        }
        return false;
    }
    last_frame_ = std::move(latest.second);
    vbuffer_ = last_frame_;
    frame.data = vbuffer_.data();
    frame.size = vbuffer_.size();
    frame.is_keyframe = true;
    return true;
}

void AudioVideoFileState::seek_video_to_locked(std::int64_t target_us) {
    // 视频 demuxer 独立 seek：flush 解码器 + 清待取帧 + 按时间 seek（BACKWARD
    // 保证落在 ≤ 目标的最近关键帧，解码后快进到目标时刻）。不动音频游标。
    // 注意：只在未到 EOF 时用（EOF 后 mov demuxer 的 seek 回绕不可靠，循环
    // 回文件头走 decode 的 EOF 分支整套重建）
    avcodec_flush_buffers(video_codec_);
    pending_frames_.clear();
    auto target_ts = av_rescale_q(target_us, AVRational{1, 1'000'000},
                                  video_fmt_ctx_->streams[video_stream_]->time_base);
    av_seek_frame(video_fmt_ctx_, video_stream_, target_ts, AVSEEK_FLAG_BACKWARD);
    last_decoded_pts_us_ = -1;
}

void AudioVideoFileState::decode_audio_locked(std::int16_t *dst, std::size_t out_samples) {
    std::size_t got = 0;
    while (got < out_samples) {
        int ret = av_read_frame(fmt_ctx_, packet_);
        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                SPDLOG_INFO("FFmpeg: file ended, looping from start");
                seek_to_start_locked();
                continue;
            }
            break;
        }
        if (packet_->stream_index != audio_stream_) {
            av_packet_unref(packet_);
            continue;
        }
        ret = avcodec_send_packet(audio_codec_, packet_);
        av_packet_unref(packet_);
        if (ret < 0)
            continue;
        while (got < out_samples) {
            ret = avcodec_receive_frame(audio_codec_, aframe_);
            if (ret == AVERROR(EAGAIN))
                break;
            if (ret < 0)
                break;
            // swr_convert 输出到 dst 剩余位置，每次输出样本数 ≤ 输入帧样本数
            std::uint8_t *out_data =
                    reinterpret_cast<std::uint8_t *>(dst + got * channels_);
            int out_count = static_cast<int>(out_samples - got);
            int converted = swr_convert(swr_, &out_data, out_count,
                                        const_cast<const std::uint8_t **>(aframe_->data), aframe_->nb_samples);
            if (converted > 0)
                got += static_cast<std::size_t>(converted);
            av_frame_unref(aframe_);
            if (converted == 0)
                break; // 输入样本未产生输出（重采样缓冲），等下一帧
        }
    }
    // 不足部分补静音（解码器尾音/损坏帧）
    if (got < out_samples)
        std::memset(dst + got * channels_, 0, (out_samples - got) * channels_ * sizeof(std::int16_t));
    // 主时钟推进 = 实际输出的样本数
    audio_samples_ += out_samples;
}

void AudioVideoFileState::seek_to_start_locked() {
    // 音频循环：音频 demuxer + 解码器 flush + seek 回 0。主时钟（audio_samples_）
    // 不归零——它是播放进度（输出样本数），必须单调递增，视频取帧靠它对齐；
    // 游标回 0 只是重新供给输入（swr 输出是连续循环的，内容不跳变）
    avcodec_flush_buffers(audio_codec_);
    av_seek_frame(fmt_ctx_, -1, 0, AVSEEK_FLAG_BACKWARD);
}

std::vector<AudioFormatInfo> AudioVideoFileState::audio_formats() const {
    if (!init_ok_)
        return {{1, 16, 48000}};
    std::vector<AudioFormatInfo> fmts;
    for (auto rate: sample_rates_)
        fmts.push_back({channels_, 16, rate});
    return fmts;
}

AudioFormatInfo AudioVideoFileState::audio_current_format() const {
    return {channels_, 16, sample_rate_};
}

bool AudioVideoFileState::audio_set_format(std::uint16_t channels, std::uint8_t bits_per_sample,
                                           std::uint32_t sample_rate) {
    std::lock_guard lock(mutex_);
    if (!init_ok_)
        return false;
    if (channels != channels_ || bits_per_sample != 16)
        return false;
    if (sample_rate == sample_rate_)
        return true;
    // 只接受支持列表内的采样率（重采样目标，音画同步的文件源不能任意变速）
    if (std::find(sample_rates_.begin(), sample_rates_.end(), sample_rate) == sample_rates_.end())
        return false;
    sample_rate_ = sample_rate;
    // 重建 swresample（目标采样率变化）
    swr_free(&swr_);
    AVChannelLayout out_layout;
    av_channel_layout_default(&out_layout, channels_);
    if (swr_alloc_set_opts2(&swr_, &out_layout, AV_SAMPLE_FMT_S16, sample_rate_, &audio_codec_->ch_layout,
                            audio_codec_->sample_fmt, audio_codec_->sample_rate, 0, nullptr) < 0 ||
        swr_init(swr_) < 0) {
        SPDLOG_ERROR("FFmpeg: cannot recreate swresample for {}Hz", sample_rate);
        return false;
    }
    return true;
}

bool AudioVideoFileState::audio_get_chunk(AudioChunk &chunk) {
    std::lock_guard lock(mutex_);
    if (!init_ok_)
        return false;
    // 每块 1ms 数据（采样率为 8kHz 整数倍时恰好整除，对齐 AudioFileSource）
    auto frames_per_chunk = static_cast<std::size_t>(sample_rate_ / 1000);
    chunk_buffer_.resize(frames_per_chunk * channels_ * sizeof(std::int16_t));
    decode_audio_locked(reinterpret_cast<std::int16_t *>(chunk_buffer_.data()), frames_per_chunk);
    chunk.data = chunk_buffer_.data();
    chunk.size = chunk_buffer_.size();
    return true;
}

// ==================== FfmpegVideoSourceView ====================

FfmpegVideoSourceView::FfmpegVideoSourceView(std::shared_ptr<AudioVideoFileState> state) :
    state_(std::move(state)) {
}

std::vector<VideoFormatInfo> FfmpegVideoSourceView::supported_formats() const {
    return state_->video_formats();
}

VideoFormatInfo FfmpegVideoSourceView::current_format() const {
    return state_->video_formats().front();
}

bool FfmpegVideoSourceView::set_format(std::uint32_t fourcc, std::uint16_t width, std::uint16_t height,
                                       std::uint32_t frame_interval) {
    // 文件源宽高/格式固定：只接受文件本身的格式（同 FfmpegSource 的校验）
    auto fmt = current_format();
    if (fourcc != fmt.fourcc || width != fmt.width || height != fmt.height)
        return false;
    // frame_interval 只影响 UVC 帧描述符与 PROBE 协商，文件源按文件帧率出帧
    return true;
}

bool FfmpegVideoSourceView::get_frame(VideoFrame &frame) {
    return state_->video_get_frame(frame);
}

std::size_t FfmpegVideoSourceView::max_frame_size() const {
    return state_->video_max_frame_size();
}

std::uint32_t FfmpegVideoSourceView::frame_interval() const {
    return state_->video_frame_interval();
}

// ==================== FfmpegAudioSourceView ====================

FfmpegAudioSourceView::FfmpegAudioSourceView(std::shared_ptr<AudioVideoFileState> state) :
    state_(std::move(state)) {
}

std::vector<AudioFormatInfo> FfmpegAudioSourceView::supported_formats() const {
    return state_->audio_formats();
}

AudioFormatInfo FfmpegAudioSourceView::current_format() const {
    return state_->audio_current_format();
}

bool FfmpegAudioSourceView::set_format(std::uint16_t channels, std::uint8_t bits_per_sample,
                                       std::uint32_t sample_rate) {
    return state_->audio_set_format(channels, bits_per_sample, sample_rate);
}

bool FfmpegAudioSourceView::get_chunk(AudioChunk &chunk) {
    return state_->audio_get_chunk(chunk);
}

} // namespace usbipdcpp
