#pragma once

#include <mutex>
#include <type_traits>
#include <algorithm>
#include <utility>

namespace usbipdcpp {

/**
 * @brief 固定大小的对象池
 *
 * 特点：
 * - 预分配所有对象，避免运行时内存分配
 * - 可验证指针归属，防止重复 free
 * - alloc O(1)，free O(log n)
 * - 可选线程安全
 *
 * @tparam T 对象类型
 * @tparam PoolSize 池大小
 * @tparam ThreadSafe 是否线程安全（默认 false）
 */
template<typename T, size_t PoolSize, bool ThreadSafe = false>
class ObjectPool {
    static_assert(PoolSize > 0, "PoolSize must be greater than 0");

public:
    ObjectPool() {
        // 创建对象
        for (size_t i = 0; i < PoolSize; ++i) {
            pool_[i] = {new T{}, false};
        }
        // 按指针排序，用于二分查找
        std::sort(pool_, pool_ + PoolSize, [](const auto &a, const auto &b) {
            return a.first < b.first;
        });
        // 初始化空闲索引栈
        for (size_t i = 0; i < PoolSize; ++i) {
            free_stack_[i] = i;
        }
        free_top_ = PoolSize;
    }

    ~ObjectPool() {
        clear();
    }

    // 禁止拷贝和移动
    ObjectPool(const ObjectPool &) = delete;
    ObjectPool &operator=(const ObjectPool &) = delete;
    ObjectPool(ObjectPool &&) = delete;
    ObjectPool &operator=(ObjectPool &&) = delete;

    /**
     * @brief 分配对象
     * @return 对象指针，池空时返回 nullptr
     */
    T *alloc() {
        if constexpr (ThreadSafe) {
            std::lock_guard<std::mutex> lock(mutex_);
            return alloc_impl();
        }
        else {
            return alloc_impl();
        }
    }

    /**
     * @brief 归还对象
     * @param obj 对象指针
     * @return true 成功归还，false 指针不属于本池或重复 free
     */
    bool free(T *obj) {
        if (!obj) {
            return false;
        }

        if constexpr (ThreadSafe) {
            std::lock_guard<std::mutex> lock(mutex_);
            return free_impl(obj);
        }
        else {
            return free_impl(obj);
        }
    }

    /**
     * @brief 获取池中可用数量
     */
    size_t available() const {
        if constexpr (ThreadSafe) {
            std::lock_guard<std::mutex> lock(mutex_);
            return free_top_;
        }
        else {
            return free_top_;
        }
    }

    /**
     * @brief 获取池总容量
     */
    constexpr size_t capacity() const {
        return PoolSize;
    }

    /**
     * @brief 清空池（删除所有对象）
     */
    void clear() {
        if constexpr (ThreadSafe) {
            std::lock_guard<std::mutex> lock(mutex_);
            clear_impl();
        }
        else {
            clear_impl();
        }
    }

private:
    std::pair<T *, bool> pool_[PoolSize] = {};
    size_t free_stack_[PoolSize] = {};
    size_t free_top_ = 0;
    mutable std::conditional_t<ThreadSafe, std::mutex, char> mutex_{};

    T *alloc_impl() {
        if (free_top_ == 0) {
            return nullptr;
        }
        size_t index = free_stack_[--free_top_];
        pool_[index].second = true;
        return pool_[index].first;
    }

    bool free_impl(T *obj) {
        // 二分查找指针
        auto it = std::lower_bound(pool_, pool_ + PoolSize, obj,
                                   [](const auto &elem, T *val) {
                                       return elem.first < val;
                                   });
        if (it == pool_ + PoolSize || it->first != obj) {
            return false; // 不是本池的对象
        }
        if (!it->second) {
            return false; // 重复 free
        }
        it->second = false;
        // 计算索引并压回栈
        size_t index = it - pool_;
        free_stack_[free_top_++] = index;
        return true;
    }

    void clear_impl() {
        for (size_t i = 0; i < PoolSize; ++i) {
            delete pool_[i].first;
            pool_[i] = {nullptr, false};
        }
        free_top_ = 0;
    }
};

} // namespace usbipdcpp
