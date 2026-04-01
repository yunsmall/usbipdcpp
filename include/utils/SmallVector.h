#pragma once

#include <array>
#include <vector>
#include <cstddef>
#include <iterator>

namespace usbipdcpp {

/**
 * @brief 小型向量容器，优先使用栈内存，溢出时回退到堆
 *
 * 适用于嵌入式平台，避免常见情况下的动态内存分配。
 * 当元素数量超过 N 时，自动迁移到堆存储。
 *
 * @tparam T 元素类型
 * @tparam N 栈上最大元素数量
 */
template<typename T, std::size_t N>
class SmallVector {
    static_assert(N > 0, "N must be greater than 0");

public:
    using value_type = T;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using reference = T&;
    using const_reference = const T&;
    using pointer = T*;
    using const_pointer = const T*;
    using iterator = T*;
    using const_iterator = const T*;

    SmallVector() noexcept = default;

    ~SmallVector() = default;

    SmallVector(const SmallVector& other)
        : size_(other.size_), on_heap_(other.on_heap_) {
        if (on_heap_) {
            heap_storage_ = other.heap_storage_;
        } else {
            for (size_type i = 0; i < size_; ++i) {
                stack_storage_[i] = other.stack_storage_[i];
            }
        }
    }

    SmallVector(SmallVector&& other) noexcept
        : size_(other.size_), on_heap_(other.on_heap_) {
        if (on_heap_) {
            heap_storage_ = std::move(other.heap_storage_);
        } else {
            for (size_type i = 0; i < size_; ++i) {
                stack_storage_[i] = std::move(other.stack_storage_[i]);
            }
        }
        other.size_ = 0;
        other.on_heap_ = false;
    }

    SmallVector& operator=(const SmallVector& other) {
        if (this != &other) {
            clear();
            size_ = other.size_;
            on_heap_ = other.on_heap_;
            if (on_heap_) {
                heap_storage_ = other.heap_storage_;
            } else {
                for (size_type i = 0; i < size_; ++i) {
                    stack_storage_[i] = other.stack_storage_[i];
                }
            }
        }
        return *this;
    }

    SmallVector& operator=(SmallVector&& other) noexcept {
        if (this != &other) {
            clear();
            size_ = other.size_;
            on_heap_ = other.on_heap_;
            if (on_heap_) {
                heap_storage_ = std::move(other.heap_storage_);
            } else {
                for (size_type i = 0; i < size_; ++i) {
                    stack_storage_[i] = std::move(other.stack_storage_[i]);
                }
            }
            other.size_ = 0;
            other.on_heap_ = false;
        }
        return *this;
    }

    void push_back(const T& value) {
        if (on_heap_) {
            heap_storage_.push_back(value);
        } else if (size_ < N) {
            stack_storage_[size_] = value;
        } else {
            // 首次溢出，迁移到堆
            migrate_to_heap();
            heap_storage_.push_back(value);
        }
        ++size_;
    }

    void push_back(T&& value) {
        if (on_heap_) {
            heap_storage_.push_back(std::move(value));
        } else if (size_ < N) {
            stack_storage_[size_] = std::move(value);
        } else {
            // 首次溢出，迁移到堆
            migrate_to_heap();
            heap_storage_.push_back(std::move(value));
        }
        ++size_;
    }

    template<typename... Args>
    reference emplace_back(Args&&... args) {
        if (on_heap_) {
            heap_storage_.emplace_back(std::forward<Args>(args)...);
        } else if (size_ < N) {
            stack_storage_[size_] = T(std::forward<Args>(args)...);
        } else {
            migrate_to_heap();
            heap_storage_.emplace_back(std::forward<Args>(args)...);
        }
        ++size_;
        return back();
    }

    void pop_back() {
        if (on_heap_) {
            heap_storage_.pop_back();
        }
        --size_;
    }

    void clear() noexcept {
        if (on_heap_) {
            heap_storage_.clear();
        }
        size_ = 0;
        on_heap_ = false;
    }

    void resize(size_type new_size) {
        if (new_size <= N) {
            if (on_heap_) {
                // 从堆迁移回栈
                for (size_type i = 0; i < new_size; ++i) {
                    stack_storage_[i] = std::move(heap_storage_[i]);
                }
                heap_storage_.clear();
                on_heap_ = false;
            }
            size_ = new_size;
        } else {
            if (!on_heap_) {
                migrate_to_heap();
            }
            heap_storage_.resize(new_size);
            size_ = new_size;
        }
    }

    void reserve([[maybe_unused]] size_type new_cap) {
        if (new_cap > N && !on_heap_) {
            migrate_to_heap();
        }
        if (on_heap_) {
            heap_storage_.reserve(new_cap);
        }
    }

    [[nodiscard]] reference operator[](size_type pos) {
        return on_heap_ ? heap_storage_[pos] : stack_storage_[pos];
    }

    [[nodiscard]] const_reference operator[](size_type pos) const {
        return on_heap_ ? heap_storage_[pos] : stack_storage_[pos];
    }

    [[nodiscard]] reference at(size_type pos) {
        return on_heap_ ? heap_storage_.at(pos) : stack_storage_.at(pos);
    }

    [[nodiscard]] const_reference at(size_type pos) const {
        return on_heap_ ? heap_storage_.at(pos) : stack_storage_.at(pos);
    }

    [[nodiscard]] reference front() { return (*this)[0]; }
    [[nodiscard]] const_reference front() const { return (*this)[0]; }
    [[nodiscard]] reference back() { return (*this)[size_ - 1]; }
    [[nodiscard]] const_reference back() const { return (*this)[size_ - 1]; }

    [[nodiscard]] pointer data() noexcept {
        return on_heap_ ? heap_storage_.data() : stack_storage_.data();
    }

    [[nodiscard]] const_pointer data() const noexcept {
        return on_heap_ ? heap_storage_.data() : stack_storage_.data();
    }

    [[nodiscard]] iterator begin() noexcept { return data(); }
    [[nodiscard]] const_iterator begin() const noexcept { return data(); }
    [[nodiscard]] const_iterator cbegin() const noexcept { return data(); }

    [[nodiscard]] iterator end() noexcept { return data() + size_; }
    [[nodiscard]] const_iterator end() const noexcept { return data() + size_; }
    [[nodiscard]] const_iterator cend() const noexcept { return data() + size_; }

    [[nodiscard]] size_type size() const noexcept { return size_; }
    [[nodiscard]] size_type capacity() const noexcept {
        return on_heap_ ? heap_storage_.capacity() : N;
    }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }
    [[nodiscard]] bool on_heap() const noexcept { return on_heap_; }

private:
    void migrate_to_heap() {
        heap_storage_.reserve(N + 1);
        for (size_type i = 0; i < size_; ++i) {
            heap_storage_.push_back(std::move(stack_storage_[i]));
        }
        on_heap_ = true;
    }

    std::array<T, N> stack_storage_{};
    std::vector<T> heap_storage_;
    size_type size_ = 0;
    bool on_heap_ = false;
};

} // namespace usbipdcpp
