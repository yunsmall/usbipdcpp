#pragma once

#include <cstdint>
#include <chrono>
#include <unordered_map>
#include <string>
#include <mutex>
#include <spdlog/spdlog.h>

namespace usbipdcpp {

/**
 * @brief 延时追踪器，用于分析数据包处理的各个阶段耗时
 * 线程安全
 */
class LatencyTracker {
public:
    /**
     * @brief 开始追踪指定 seqnum
     */
    void start_tracking(std::uint32_t seqnum) {
        std::lock_guard lock(mutex_);
        tracking_map_[seqnum] = std::chrono::steady_clock::now();
    }

    /**
     * @brief 追踪并打印自开始以来经过的时间
     * @param seqnum 序列号
     * @param message 格式化消息
     */
    void track(std::uint32_t seqnum, const char* message) {
        std::lock_guard lock(mutex_);
        auto it = tracking_map_.find(seqnum);
        if (it == tracking_map_.end()) {
            return;
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - it->second);
        spdlog::info("[seqnum:{}] {} (elapsed: {} us)", seqnum, message, elapsed.count());
    }

    /**
     * @brief 结束追踪并打印总时间
     */
    void end_tracking(std::uint32_t seqnum) {
        std::lock_guard lock(mutex_);
        auto it = tracking_map_.find(seqnum);
        if (it == tracking_map_.end()) {
            return;
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - it->second);
        spdlog::info("[seqnum:{}] tracking ended (total: {} us)", seqnum, elapsed.count());
        tracking_map_.erase(it);
    }

    /**
     * @brief 结束追踪并打印自定义消息和总时间
     */
    void end_tracking(std::uint32_t seqnum, const char* message) {
        std::lock_guard lock(mutex_);
        auto it = tracking_map_.find(seqnum);
        if (it == tracking_map_.end()) {
            return;
        }

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - it->second);
        spdlog::info("[seqnum:{}] {} (total: {} us)", seqnum, message, elapsed.count());
        tracking_map_.erase(it);
    }

    /**
     * @brief 检查是否正在追踪指定 seqnum
     */
    [[nodiscard]] bool is_tracking(std::uint32_t seqnum) const {
        std::lock_guard lock(mutex_);
        return tracking_map_.find(seqnum) != tracking_map_.end();
    }

    /**
     * @brief 清除所有追踪
     */
    void clear() {
        std::lock_guard lock(mutex_);
        tracking_map_.clear();
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::uint32_t, std::chrono::steady_clock::time_point> tracking_map_;
};

} // namespace usbipdcpp


// 宏定义

#if defined(USBIPDCPP_TRACK_PACKAGE) || defined(USBIPDCPP_FORCE_TRACK_PACKAGE)

// 在类中声明延时追踪器成员
#define LATENCY_TRACKER_MEMBER(name) usbipdcpp::LatencyTracker name

// 初始化追踪
#define LATENCY_TRACK_START(tracker, seqnum) (tracker).start_tracking(seqnum)

// 追踪并打印经过的时间
#define LATENCY_TRACK(tracker, seqnum, message) (tracker).track(seqnum, message)

// 结束追踪并打印总时间
#define LATENCY_TRACK_END(tracker, seqnum) (tracker).end_tracking(seqnum)

// 结束追踪并打印自定义消息
#define LATENCY_TRACK_END_MSG(tracker, seqnum, message) (tracker).end_tracking(seqnum, message)

#else

#define LATENCY_TRACKER_MEMBER(name)
#define LATENCY_TRACK_START(tracker, seqnum) ((void)0)
#define LATENCY_TRACK(tracker, seqnum, message) ((void)0)
#define LATENCY_TRACK_END(tracker, seqnum) ((void)0)
#define LATENCY_TRACK_END_MSG(tracker, seqnum, message) ((void)0)

#endif
