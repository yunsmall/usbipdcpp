#pragma once

#include <cstdint>
#include <array>
#include <atomic>
#include <shared_mutex>
#include <unordered_map>
#include <optional>
#include <vector>
#include <mutex>

namespace usbipdcpp {

/**
 * @brief 分段锁的传输追踪器，用于高效追踪并发传输
 *
 * 核心优化：
 * - 按 seqnum 分段，分布均匀，降低竞争
 * - 原子计数器：快速路径无需锁
 *
 * @tparam TransferPtr 传输指针类型（如 libusb_transfer*, usb_transfer_t*）
 * @tparam SegmentCount 分段数量，默认16
 */
template<typename TransferPtr, size_t SegmentCount = 16>
class ConcurrentTransferTracker {
    static_assert((SegmentCount & (SegmentCount - 1)) == 0, "SegmentCount must be power of 2");

public:
    static constexpr size_t SEGMENT_COUNT = SegmentCount;

    struct TransferInfo {
        std::uint32_t seqnum;
        TransferPtr transfer;
        std::uint8_t endpoint;
    };

    ConcurrentTransferTracker() = default;
    ~ConcurrentTransferTracker() = default;

    /**
     * @brief 注册一个新的传输追踪
     * @return true 成功
     */
    bool register_transfer(std::uint32_t seqnum, TransferPtr transfer, std::uint8_t endpoint) {
        size_t segment_idx = get_segment_index(seqnum);
        std::lock_guard lock(segment_locks_[segment_idx]);

        auto [it, inserted] = segments_[segment_idx].emplace(
            seqnum,
            TransferInfo{
                .seqnum = seqnum,
                .transfer = transfer,
                .endpoint = endpoint
            });

        if (inserted) {
            concurrent_transfer_count_.fetch_add(1, std::memory_order_release);
            return true;
        }

        return false;
    }

    /**
     * @brief 查询传输是否存在
     */
    bool contains(std::uint32_t seqnum) const {
        size_t segment_idx = get_segment_index(seqnum);
        std::shared_lock lock(segment_locks_[segment_idx]);
        return segments_[segment_idx].count(seqnum) > 0;
    }

    /**
     * @brief 获取传输信息
     */
    std::optional<TransferInfo> get(std::uint32_t seqnum) const {
        size_t segment_idx = get_segment_index(seqnum);
        std::shared_lock lock(segment_locks_[segment_idx]);
        auto it = segments_[segment_idx].find(seqnum);
        if (it != segments_[segment_idx].end()) {
            return it->second;
        }
        return std::nullopt;
    }

    /**
     * @brief 删除传输
     */
    bool remove(std::uint32_t seqnum) {
        size_t segment_idx = get_segment_index(seqnum);
        std::lock_guard lock(segment_locks_[segment_idx]);
        auto it = segments_[segment_idx].find(seqnum);
        if (it != segments_[segment_idx].end()) {
            segments_[segment_idx].erase(it);
            concurrent_transfer_count_.fetch_sub(1, std::memory_order_release);
            return true;
        }
        return false;
    }

    /**
     * @brief 清空所有传输追踪
     */
    void clear() {
        for (size_t i = 0; i < SEGMENT_COUNT; ++i) {
            std::lock_guard lock(segment_locks_[i]);
            segments_[i].clear();
        }
        concurrent_transfer_count_.store(0, std::memory_order_release);
    }

    /**
     * @brief 获取所有传输信息（用于批量取消等操作）
     */
    std::vector<TransferInfo> get_all_transfers() const {
        std::vector<TransferInfo> result;
        for (size_t i = 0; i < SEGMENT_COUNT; ++i) {
            std::shared_lock lock(segment_locks_[i]);
            for (const auto& [seqnum, info] : segments_[i]) {
                result.push_back(info);
            }
        }
        return result;
    }

    /**
     * @brief 获取当前并发数
     */
    size_t concurrent_count() const {
        return concurrent_transfer_count_.load(std::memory_order_acquire);
    }

private:
    size_t get_segment_index(std::uint32_t seqnum) const {
        return seqnum & (SEGMENT_COUNT - 1);
    }

    mutable std::array<std::shared_mutex, SEGMENT_COUNT> segment_locks_;
    std::array<std::unordered_map<std::uint32_t, TransferInfo>, SEGMENT_COUNT> segments_;
    std::atomic<size_t> concurrent_transfer_count_{0};
};

} // namespace usbipdcpp