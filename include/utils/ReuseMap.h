#pragma once

#include <cstddef>
#include <vector>
#include <utility>

namespace usbipdcpp {

/**
 * @brief 基于 std::vector 的 map，删除时标记槽位为空，
 *        下次插入复用空槽，空槽用尽才 push_back 扩容。
 *        线性查找 O(n)，适合小容量高频 insert/erase 场景。
 */
template<typename Key, typename Value>
class ReuseMap {
    struct Slot {
        Key key{};
        Value value{};
        bool occupied = false;
    };

    std::vector<Slot> slots_;
    size_t size_ = 0;

public:
    /// 预分配内存，避免后续 push_back 时 reallocate
    void reserve(size_t n) { slots_.reserve(n); }

    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }

    Value *insert(const Key &key, Value value) {
        // 先查是否已存在
        for (auto &slot : slots_) {
            if (slot.occupied && slot.key == key) return &slot.value;
        }
        // 复用空槽
        for (auto &slot : slots_) {
            if (!slot.occupied) {
                slot.key = key;
                slot.value = std::move(value);
                slot.occupied = true;
                size_++;
                return &slot.value;
            }
        }
        // 无空槽，扩容
        slots_.push_back({key, std::move(value), true});
        size_++;
        return &slots_.back().value;
    }

    Value *find(const Key &key) {
        for (auto &slot : slots_) {
            if (slot.occupied && slot.key == key) return &slot.value;
        }
        return nullptr;
    }

    const Value *find(const Key &key) const {
        for (const auto &slot : slots_) {
            if (slot.occupied && slot.key == key) return &slot.value;
        }
        return nullptr;
    }

    bool erase(const Key &key) {
        for (auto &slot : slots_) {
            if (slot.occupied && slot.key == key) {
                slot.occupied = false;
                size_--;
                return true;
            }
        }
        return false;
    }

    template<typename F>
    void for_each(F &&f) {
        for (auto &slot : slots_) {
            if (slot.occupied) f(slot.key, slot.value);
        }
    }

    template<typename F>
    void for_each(F &&f) const {
        for (const auto &slot : slots_) {
            if (slot.occupied) f(slot.key, slot.value);
        }
    }

    void clear() {
        for (auto &slot : slots_) slot.occupied = false;
        size_ = 0;
    }
};

}  // namespace usbipdcpp