#pragma once

#include <vector>
#include <mutex>
#include <type_traits>

namespace usbipdcpp {

/**
 * @brief 对象池，支持有上限的动态扩容
 *
 * 特点：
 * - 预分配初始数量对象，避免频繁内存分配
 * - 支持动态扩容，有上限保证内存可控
 * - 可选线程安全
 *
 * @tparam T 对象类型
 * @tparam InitialSize 初始大小
 * @tparam MaxSize 最大容量（默认等于 InitialSize，不扩容）
 * @tparam ThreadSafe 是否线程安全（默认 false）
 */
template<typename T, size_t InitialSize, size_t MaxSize = InitialSize, bool ThreadSafe = false>
class ObjectPool {
    static_assert(InitialSize > 0, "InitialSize must be greater than 0");
    static_assert(MaxSize >= InitialSize, "MaxSize must be >= InitialSize");

public:
    ObjectPool() {
        // 预分配初始对象
        for (size_t i = 0; i < InitialSize; ++i) {
            pool_.push_back(new T{});
        }
        available_ = InitialSize;
        capacity_ = InitialSize;
    }

    ~ObjectPool() {
        clear();
    }

    // 禁止拷贝
    ObjectPool(const ObjectPool &) = delete;
    ObjectPool &operator=(const ObjectPool &) = delete;

    // 允许移动
    ObjectPool(ObjectPool &&other) noexcept {
        if constexpr (ThreadSafe) {
            std::lock_guard<std::mutex> lock(other.mutex_);
        }
        pool_ = std::move(other.pool_);
        available_ = other.available_;
        capacity_ = other.capacity_;
        other.available_ = 0;
        other.capacity_ = 0;
    }

    ObjectPool &operator=(ObjectPool &&other) noexcept {
        if (this != &other) {
            clear();
            if constexpr (ThreadSafe) {
                std::lock_guard<std::mutex> lock(other.mutex_);
            }
            pool_ = std::move(other.pool_);
            available_ = other.available_;
            capacity_ = other.capacity_;
            other.available_ = 0;
            other.capacity_ = 0;
        }
        return *this;
    }

    /**
     * @brief 分配对象
     * @return 对象指针，池空且达上限时返回 nullptr
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
     * @return true 成功归还，false 池满（对象未归还）
     */
    bool free(T *obj) {
        if (!obj)
            return false;

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
            return available_;
        }
        else {
            return available_;
        }
    }

    /**
     * @brief 获取池总容量
     */
    size_t capacity() const {
        if constexpr (ThreadSafe) {
            std::lock_guard<std::mutex> lock(mutex_);
            return capacity_;
        }
        else {
            return capacity_;
        }
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
    static constexpr size_t CHUNK_SIZE = 16; // 每次扩容数量

    std::vector<T *> pool_;
    size_t available_ = 0;
    size_t capacity_ = 0;
    mutable std::conditional_t<ThreadSafe, std::mutex, char> mutex_{};

    T *alloc_impl() {
        if (available_ > 0) {
            return pool_[--available_];
        }

        // 池空，尝试扩容
        if (capacity_ < MaxSize) {
            size_t grow = std::min(CHUNK_SIZE, MaxSize - capacity_);
            for (size_t i = 0; i < grow; ++i) {
                pool_.push_back(new T{});
            }
            available_ += grow;
            capacity_ += grow;
            return pool_[--available_];
        }

        return nullptr; // 达到上限
    }

    bool free_impl(T *obj) {
        if (available_ >= capacity_) {
            return false; // 池满
        }
        pool_[available_++] = obj;
        return true;
    }

    void clear_impl() {
        for (auto *ptr: pool_) {
            delete ptr;
        }
        pool_.clear();
        available_ = 0;
        capacity_ = 0;
    }
};

} // namespace usbipdcpp
