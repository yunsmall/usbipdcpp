#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace usbipdcpp {

/// 视频源支持的格式信息
struct VideoFormatInfo {
    std::uint32_t fourcc; // FOURCC 像素格式 (UvcFourCC)
    std::uint16_t width; // 帧宽度
    std::uint16_t height; // 帧高度
    std::uint32_t max_frame_size; // 最大帧字节数
    std::uint32_t default_frame_interval; // 默认帧间隔（100ns 单位）
    std::uint8_t bits_per_pixel; // 每像素位数
};

/// 视频帧
struct VideoFrame {
    const std::uint8_t *data; // 帧数据指针（由 VideoSource 管理生命周期）
    std::size_t size; // 帧数据字节数
    bool is_keyframe; // 是否为关键帧
};

/// 视频帧源抽象接口
/// 实现类负责生成/读取视频帧，UvcHandler 负责打包成 UVC 协议发送
class VideoSource {
public:
    virtual ~VideoSource() = default;

    /// 返回源支持的所有格式列表
    virtual std::vector<VideoFormatInfo> supported_formats() const = 0;

    /// 当前协商的格式
    virtual VideoFormatInfo current_format() const = 0;

    /// 切换格式。仅切换描述符索引，UvcHandler 会在 COMMIT 后调用
    virtual bool set_format(std::uint32_t fourcc, std::uint16_t width, std::uint16_t height,
                            std::uint32_t frame_interval) = 0;

    /// 获取下一帧。data 指针由源管理，在下次 get_frame 调用前有效
    virtual bool get_frame(VideoFrame &frame) = 0;

    /// 当前格式下的最大帧大小（用于分配 ISO 传输缓冲区）
    virtual std::size_t max_frame_size() const = 0;

    /// 当前格式帧间隔（100ns 单位），用于 ISO 传输调度
    virtual std::uint32_t frame_interval() const = 0;
};

} // namespace usbipdcpp
