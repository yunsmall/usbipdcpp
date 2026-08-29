#include "PlaybackSink.h"

#include <algorithm>
#include <cstring>

// 单头文件库：仅此编译单元提供实现
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>
#include <spdlog/spdlog.h>

namespace usbipdcpp {

PlaybackSink::PlaybackSink(std::string device_name) : device_name(std::move(device_name)) {
    device = new ma_device{};
    open_device();
}

PlaybackSink::~PlaybackSink() {
    close_device();
    delete static_cast<ma_device *>(device);
}

std::vector<AudioFormatInfo> PlaybackSink::supported_formats() const {
    // 常见采样率 × 单双声道（16 位 PCM），重采样交给声卡驱动
    static const std::uint32_t rates[] = {8000, 16000, 22050, 32000, 44100, 48000, 96000};
    std::vector<AudioFormatInfo> fmts;
    for (auto rate: rates) {
        fmts.push_back({1, 16, rate});
        fmts.push_back({2, 16, rate});
    }
    return fmts;
}

AudioFormatInfo PlaybackSink::current_format() const {
    std::lock_guard lock(mutex);
    return format;
}

bool PlaybackSink::set_format(std::uint16_t channels, std::uint8_t bits_per_sample, std::uint32_t sample_rate) {
    std::unique_lock lock(mutex);
    if (bits_per_sample != 16 || (channels != 1 && channels != 2)) {
        return false;
    }
    bool restart = !discarding && (channels != format.channels || sample_rate != format.sample_rate);
    format = {channels, bits_per_sample, sample_rate};
    lock.unlock();
    if (restart) {
        // 不能持锁重启：ma_device_uninit 等回调线程退出，回调拿同一把锁会死锁
        open_device();
    }
    return true;
}

void PlaybackSink::write_pcm(const std::uint8_t *data, std::size_t size) {
    std::lock_guard lock(mutex);
    received += size;
    if (discarding) {
        return;
    }
    // 缓冲满时丢新数据（RingBuffer.write 返回实际写入字节数），回调侧缺数据填静音
    auto written = buffer.write(data, size);
    if (written < size) {
        SPDLOG_WARN("PlaybackSink: 缓冲满，丢弃 {} 字节", size - written);
    }
}

void PlaybackSink::reset() {
    std::lock_guard lock(mutex);
    buffer.clear();
}

std::uint64_t PlaybackSink::received_bytes() const {
    std::lock_guard lock(mutex);
    return received;
}

void PlaybackSink::open_device() {
    close_device();
    discarding = false;

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_s16;
    config.playback.channels = format.channels;
    config.sampleRate = format.sample_rate;
    config.dataCallback = data_callback;
    config.pUserData = this;

    ma_result result;
    if (device_name.empty()) {
        result = ma_device_init(NULL, &config, static_cast<ma_device *>(device));
    }
    else {
        // 指定设备名：自建 context 枚举设备匹配名字。注意 ma_device_init 后 context
        // 必须保持存活，所以存成员；析构时随 device 一起释放
        auto *ctx = new ma_context;
        context = ctx;
        if (ma_context_init(NULL, 0, NULL, ctx) != MA_SUCCESS) {
            SPDLOG_ERROR("PlaybackSink: miniaudio context 初始化失败，进入丢弃模式");
            discarding = true;
            return;
        }
        ma_device_info *devices = nullptr;
        ma_uint32 count = 0;
        if (ma_context_get_devices(ctx, &devices, &count, NULL, NULL) != MA_SUCCESS || count == 0) {
            SPDLOG_ERROR("PlaybackSink: 枚举播放设备失败，进入丢弃模式");
            discarding = true;
            return;
        }
        bool found = false;
        for (ma_uint32 i = 0; i < count; i++) {
            if (device_name == devices[i].name) {
                config.playback.pDeviceID = &devices[i].id;
                found = true;
                break;
            }
        }
        if (!found) {
            SPDLOG_ERROR("PlaybackSink: 找不到播放设备 '{}'，进入丢弃模式", device_name);
            discarding = true;
            return;
        }
        result = ma_device_init(ctx, &config, static_cast<ma_device *>(device));
    }

    if (result != MA_SUCCESS) {
        SPDLOG_ERROR("PlaybackSink: 打开播放设备失败（{}），进入丢弃模式", ma_result_description(result));
        discarding = true;
        return;
    }
    device_initialized = true;
    if (ma_device_start(static_cast<ma_device *>(device)) != MA_SUCCESS) {
        SPDLOG_ERROR("PlaybackSink: 启动播放失败，进入丢弃模式");
        close_device();
        discarding = true;
        return;
    }
    SPDLOG_INFO("PlaybackSink: 开始播放（{}ch {}Hz）", format.channels, format.sample_rate);
}

void PlaybackSink::close_device() {
    auto *dev = static_cast<ma_device *>(device);
    if (device_initialized) {
        // uninit 会等待回调线程退出后才返回
        ma_device_uninit(dev);
        device_initialized = false;
    }
    auto *ctx = static_cast<ma_context *>(context);
    if (ctx != nullptr) {
        ma_context_uninit(ctx);
        delete ctx;
        context = nullptr;
    }
}

void PlaybackSink::data_callback(struct ma_device *device, void *output, const void *input,
                                 std::uint32_t frame_count) {
    auto *self = static_cast<PlaybackSink *>(device->pUserData);
    auto frame_size = ma_get_bytes_per_frame(device->playback.format, device->playback.channels);
    auto n = static_cast<std::size_t>(frame_count) * frame_size;
    std::lock_guard lock(self->mutex);
    auto read = self->buffer.read(static_cast<std::uint8_t *>(output), n);
    if (read < n) {
        std::memset(static_cast<std::uint8_t *>(output) + read, 0, n - read); // 缺数据填静音
    }
}

} // namespace usbipdcpp
