#pragma once

#include <cstdint>
#include <array>
#include <shared_mutex>
#include <unordered_map>
#include <tuple>

namespace usbipdcpp {

/**
 * @brief 分段锁的 unlink 映射表，用于高效追踪 unlink 请求
 *
 * 核心优化：
 * - 按 seqnum 分段，分布均匀，降低竞争
 *
 * @tparam SegmentCount 分段数量，默认16
 */
template<size_t SegmentCount = 16>
class ConcurrentUnlinkMap {
    static_assert((SegmentCount & (SegmentCount - 1)) == 0, "SegmentCount must be power of 2");

public:
    static constexpr size_t SEGMENT_COUNT = SegmentCount;

    ConcurrentUnlinkMap() = default;
    ~ConcurrentUnlinkMap() = default;

    /**
     * @brief 插入映射：original_seqnum -> unlink_seqnum
     */
    void insert(std::uint32_t original_seqnum, std::uint32_t unlink_seqnum) {
        size_t segment_idx = get_segment_index(original_seqnum);
        std::lock_guard lock(segment_locks_[segment_idx]);
        segments_[segment_idx][original_seqnum] = unlink_seqnum;
    }

    /**
     * @brief 查询是否存在映射
     * @return {found, unlink_seqnum}
     */
    std::tuple<bool, std::uint32_t> get(std::uint32_t seqnum) const {
        size_t segment_idx = get_segment_index(seqnum);
        std::shared_lock lock(segment_locks_[segment_idx]);
        auto it = segments_[segment_idx].find(seqnum);
        if (it != segments_[segment_idx].end()) {
            return {true, it->second};
        }
        return {false, 0};
    }

    /**
     * @brief 检查是否存在映射
     */
    bool contains(std::uint32_t seqnum) const {
        size_t segment_idx = get_segment_index(seqnum);
        std::shared_lock lock(segment_locks_[segment_idx]);
        return segments_[segment_idx].count(seqnum) > 0;
    }

    /**
     * @brief 删除映射
     */
    void erase(std::uint32_t seqnum) {
        size_t segment_idx = get_segment_index(seqnum);
        std::lock_guard lock(segment_locks_[segment_idx]);
        segments_[segment_idx].erase(seqnum);
    }

    /**
     * @brief 清空所有映射
     */
    void clear() {
        for (size_t i = 0; i < SEGMENT_COUNT; ++i) {
            std::lock_guard lock(segment_locks_[i]);
            segments_[i].clear();
        }
    }

    /**
     * @brief 获取映射数量
     */
    size_t size() const {
        size_t total = 0;
        for (size_t i = 0; i < SEGMENT_COUNT; ++i) {
            std::shared_lock lock(segment_locks_[i]);
            total += segments_[i].size();
        }
        return total;
    }

private:
    size_t get_segment_index(std::uint32_t seqnum) const {
        return seqnum & (SEGMENT_COUNT - 1);
    }

    mutable std::array<std::shared_mutex, SEGMENT_COUNT> segment_locks_;
    std::array<std::unordered_map<std::uint32_t, std::uint32_t>, SEGMENT_COUNT> segments_;
};

} // namespace usbipdcpp
