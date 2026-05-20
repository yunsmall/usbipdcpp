#pragma once

#include <cstddef>
#include <vector>
#include <utility>

namespace usbipdcpp {

template<typename Key>
class ReuseSet {
    struct Slot {
        Key key{};
        bool occupied = false;
    };

    std::vector<Slot> slots_;
    size_t size_ = 0;

public:
    void reserve(size_t n) { slots_.reserve(n); }

    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }

    /// 插入。返回 true 成功，false 已存在。
    bool insert(const Key &key) {
        for (auto &slot : slots_) {
            if (slot.occupied && slot.key == key) return false;
        }
        for (auto &slot : slots_) {
            if (!slot.occupied) {
                slot.key = key;
                slot.occupied = true;
                size_++;
                return true;
            }
        }
        slots_.push_back({key, true});
        size_++;
        return true;
    }

    bool contains(const Key &key) const {
        for (const auto &slot : slots_) {
            if (slot.occupied && slot.key == key) return true;
        }
        return false;
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
            if (slot.occupied) f(slot.key);
        }
    }

    template<typename F>
    void for_each(F &&f) const {
        for (const auto &slot : slots_) {
            if (slot.occupied) f(slot.key);
        }
    }

    void clear() {
        for (auto &slot : slots_) slot.occupied = false;
        size_ = 0;
    }
};

}  // namespace usbipdcpp
