#pragma once

#include <algorithm>
#include <compare>
#include <concepts>
#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace usbipdcpp {

/**
 * @brief 小型向量容器，优先使用栈内存，溢出时回退到堆
 *
 * 适用于嵌入式平台，避免常见情况下的动态内存分配；元素数量超过 N 时自动迁移到
 * 堆存储。接口对齐 std::vector：迭代器是裸指针（连续迭代器），resize 对新增元素
 * 值初始化，pop_back/clear/erase 会析构元素。
 *
 * 与 std::vector 的差异：clear() 会释放堆存储并回到栈存储（容量变回 N），
 * 而不是保留容量。
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
    using reverse_iterator = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    SmallVector() noexcept = default;

    ~SmallVector() {
        if (!on_heap_) {
            destroy_stack_elements();
        }
    }

    SmallVector(std::initializer_list<T> init) {
        reserve(init.size());
        for (const auto &value: init) {
            push_back(value);
        }
    }

    /**
     * @brief 迭代器区间构造
     * @param first 起始迭代器
     * @param last 结束迭代器（不含）
     * @note 排除自身类型，避免和拷贝/移动构造函数抢占重载
     */
    template<std::input_iterator It>
        requires(!std::same_as<std::remove_cvref_t<It>, SmallVector>)
    SmallVector(It first, It last) {
        for (; first != last; ++first) {
            push_back(*first);
        }
    }

    /// 构造 count 个值初始化的元素
    explicit SmallVector(size_type count) {
        resize(count);
    }

    /// 构造 count 个 value 的副本
    SmallVector(size_type count, const T &value) {
        assign(count, value);
    }

    SmallVector(const SmallVector &other) {
        reserve(other.size_);
        for (const auto &value: other) {
            push_back(value);
        }
    }

    SmallVector(SmallVector &&other) noexcept(std::is_nothrow_move_constructible_v<T>) {
        move_from(other);
    }

    SmallVector &operator=(const SmallVector &other) {
        if (this != &other) {
            SmallVector tmp(other);
            swap(tmp);
        }
        return *this;
    }

    SmallVector &operator=(SmallVector &&other) noexcept(std::is_nothrow_move_constructible_v<T> &&
                                                          std::is_nothrow_move_assignable_v<T>) {
        if (this != &other) {
            clear();
            move_from(other);
        }
        return *this;
    }

    SmallVector &operator=(std::initializer_list<T> init) {
        SmallVector tmp(init);
        swap(tmp);
        return *this;
    }

    // ========== 元素访问 ==========

    [[nodiscard]] reference operator[](size_type pos) {
        return data()[pos];
    }

    [[nodiscard]] const_reference operator[](size_type pos) const {
        return data()[pos];
    }

    [[nodiscard]] reference at(size_type pos) {
        if (pos >= size_) {
            throw std::out_of_range("SmallVector::at");
        }
        return data()[pos];
    }

    [[nodiscard]] const_reference at(size_type pos) const {
        if (pos >= size_) {
            throw std::out_of_range("SmallVector::at");
        }
        return data()[pos];
    }

    [[nodiscard]] reference front() { return data()[0]; }
    [[nodiscard]] const_reference front() const { return data()[0]; }
    [[nodiscard]] reference back() { return data()[size_ - 1]; }
    [[nodiscard]] const_reference back() const { return data()[size_ - 1]; }

    [[nodiscard]] pointer data() noexcept {
        return on_heap_ ? heap_storage_.data() : stack_data();
    }

    [[nodiscard]] const_pointer data() const noexcept {
        return on_heap_ ? heap_storage_.data() : stack_data();
    }

    // ========== 迭代器（裸指针即连续迭代器） ==========

    [[nodiscard]] iterator begin() noexcept { return data(); }
    [[nodiscard]] const_iterator begin() const noexcept { return data(); }
    [[nodiscard]] const_iterator cbegin() const noexcept { return data(); }

    [[nodiscard]] iterator end() noexcept { return data() + size_; }
    [[nodiscard]] const_iterator end() const noexcept { return data() + size_; }
    [[nodiscard]] const_iterator cend() const noexcept { return data() + size_; }

    [[nodiscard]] reverse_iterator rbegin() noexcept { return reverse_iterator(end()); }
    [[nodiscard]] const_reverse_iterator rbegin() const noexcept { return const_reverse_iterator(end()); }
    [[nodiscard]] const_reverse_iterator crbegin() const noexcept { return const_reverse_iterator(cend()); }

    [[nodiscard]] reverse_iterator rend() noexcept { return reverse_iterator(begin()); }
    [[nodiscard]] const_reverse_iterator rend() const noexcept { return const_reverse_iterator(begin()); }
    [[nodiscard]] const_reverse_iterator crend() const noexcept { return const_reverse_iterator(cbegin()); }

    // ========== 容量 ==========

    [[nodiscard]] size_type size() const noexcept { return size_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0; }

    [[nodiscard]] size_type capacity() const noexcept {
        return on_heap_ ? heap_storage_.capacity() : N;
    }

    [[nodiscard]] size_type max_size() const noexcept { return heap_storage_.max_size(); }

    /// 当前是否使用堆存储
    [[nodiscard]] bool on_heap() const noexcept { return on_heap_; }

    void reserve(size_type new_cap) {
        if (new_cap <= N) {
            return;
        }
        if (!on_heap_) {
            migrate_to_heap();
        }
        heap_storage_.reserve(new_cap);
    }

    /// 堆元素数不超过 N 时迁回栈存储，否则收缩堆容量
    void shrink_to_fit() {
        if (!on_heap_) {
            return;
        }
        if (size_ <= N) {
            migrate_to_stack();
        }
        else {
            heap_storage_.shrink_to_fit();
        }
    }

    // ========== 修改 ==========

    void clear() noexcept {
        if (on_heap_) {
            // clear 与 std::vector 不同：释放堆存储，回到栈存储
            std::vector<T>().swap(heap_storage_);
            on_heap_ = false;
        }
        else {
            destroy_stack_elements();
        }
        size_ = 0;
    }

    void push_back(const T &value) {
        if (on_heap_) {
            heap_storage_.push_back(value);
            size_ = heap_storage_.size();
        }
        else if (size_ < N) {
            std::construct_at(stack_data() + size_, value);
            ++size_;
        }
        else {
            // 先构造副本再迁移：value 可能引用本容器里的元素，迁移会让引用失效
            T tmp(value);
            migrate_to_heap();
            heap_storage_.push_back(std::move(tmp));
            size_ = heap_storage_.size();
        }
    }

    void push_back(T &&value) {
        if (on_heap_) {
            heap_storage_.push_back(std::move(value));
            size_ = heap_storage_.size();
        }
        else if (size_ < N) {
            std::construct_at(stack_data() + size_, std::move(value));
            ++size_;
        }
        else {
            T tmp(std::move(value));
            migrate_to_heap();
            heap_storage_.push_back(std::move(tmp));
            size_ = heap_storage_.size();
        }
    }

    template<typename... Args>
    reference emplace_back(Args &&...args) {
        if (on_heap_) {
            heap_storage_.emplace_back(std::forward<Args>(args)...);
            size_ = heap_storage_.size();
            return heap_storage_.back();
        }
        if (size_ < N) {
            auto *constructed = std::construct_at(stack_data() + size_, std::forward<Args>(args)...);
            ++size_;
            return *constructed;
        }
        T tmp(std::forward<Args>(args)...);
        migrate_to_heap();
        heap_storage_.push_back(std::move(tmp));
        size_ = heap_storage_.size();
        return back();
    }

    void pop_back() {
        if (on_heap_) {
            heap_storage_.pop_back();
            --size_;
        }
        else {
            std::destroy_at(stack_data() + size_ - 1);
            --size_;
        }
    }

    /**
     * @brief 调整元素个数
     * @param new_size 新的大小
     * @note 扩容部分值初始化（要求 T 可默认构造）；缩小部分析构多余元素
     */
    void resize(size_type new_size) {
        if (new_size < size_) {
            truncate(new_size);
            return;
        }
        if (new_size == size_) {
            return;
        }
        if (on_heap_) {
            heap_storage_.resize(new_size);
            size_ = new_size;
            if (new_size <= N) {
                migrate_to_stack();
            }
            return;
        }
        if (new_size <= N) {
            for (size_type i = size_; i < new_size; ++i) {
                std::construct_at(stack_data() + i);
            }
            size_ = new_size;
            return;
        }
        migrate_to_heap();
        heap_storage_.resize(new_size);
        size_ = new_size;
    }

    /**
     * @brief 调整元素个数，新增元素用 value 填充
     * @param new_size 新的大小
     * @param value 新增元素的初值
     */
    void resize(size_type new_size, const T &value) {
        if (new_size < size_) {
            truncate(new_size);
            return;
        }
        if (new_size == size_) {
            return;
        }
        // value 可能引用本容器元素，先复制一份，扩容/迁移后引用会失效
        T tmp(value);
        if (on_heap_) {
            heap_storage_.resize(new_size, tmp);
            size_ = new_size;
            if (new_size <= N) {
                migrate_to_stack();
            }
            return;
        }
        if (new_size <= N) {
            for (size_type i = size_; i < new_size; ++i) {
                std::construct_at(stack_data() + i, tmp);
            }
            size_ = new_size;
            return;
        }
        migrate_to_heap();
        heap_storage_.resize(new_size, tmp);
        size_ = new_size;
    }

    void assign(size_type count, const T &value) {
        T tmp(value);
        SmallVector replacement;
        replacement.reserve(count);
        for (size_type i = 0; i < count; ++i) {
            replacement.push_back(tmp);
        }
        swap(replacement);
    }

    template<std::input_iterator It>
    void assign(It first, It last) {
        SmallVector replacement;
        for (; first != last; ++first) {
            replacement.push_back(*first);
        }
        swap(replacement);
    }

    /**
     * @brief 在 pos 前插入一个元素
     * @param pos 插入位置（指向本容器）
     * @param value 要插入的值
     * @return 指向新元素的迭代器
     */
    iterator insert(const_iterator pos, const T &value) {
        T tmp(value);
        return insert_value(static_cast<size_type>(pos - cbegin()), std::move(tmp));
    }

    iterator insert(const_iterator pos, T &&value) {
        T tmp(std::move(value));
        return insert_value(static_cast<size_type>(pos - cbegin()), std::move(tmp));
    }

    iterator insert(const_iterator pos, size_type count, const T &value) {
        const auto idx = static_cast<size_type>(pos - cbegin());
        // 先在临时容器里拼好再交换：value/迭代器可能引用本容器元素，直接原地移位会失效
        SmallVector replacement;
        replacement.reserve(size_ + count);
        for (size_type i = 0; i < idx; ++i) {
            replacement.push_back((*this)[i]);
        }
        for (size_type i = 0; i < count; ++i) {
            replacement.push_back(value);
        }
        for (size_type i = idx; i < size_; ++i) {
            replacement.push_back((*this)[i]);
        }
        swap(replacement);
        return begin() + static_cast<difference_type>(idx);
    }

    template<std::input_iterator It>
    iterator insert(const_iterator pos, It first, It last) {
        const auto idx = static_cast<size_type>(pos - cbegin());
        SmallVector replacement;
        for (size_type i = 0; i < idx; ++i) {
            replacement.push_back((*this)[i]);
        }
        for (; first != last; ++first) {
            replacement.push_back(*first);
        }
        for (size_type i = idx; i < size_; ++i) {
            replacement.push_back((*this)[i]);
        }
        swap(replacement);
        return begin() + static_cast<difference_type>(idx);
    }

    template<typename... Args>
    iterator emplace(const_iterator pos, Args &&...args) {
        T tmp(std::forward<Args>(args)...);
        return insert_value(static_cast<size_type>(pos - cbegin()), std::move(tmp));
    }

    iterator erase(const_iterator pos) { return erase(pos, pos + 1); }

    iterator erase(const_iterator first, const_iterator last) {
        const auto idx = static_cast<size_type>(first - cbegin());
        const auto count = static_cast<size_type>(last - first);
        if (count == 0) {
            return begin() + static_cast<difference_type>(idx);
        }
        if (on_heap_) {
            heap_storage_.erase(heap_storage_.begin() + static_cast<difference_type>(idx),
                                heap_storage_.begin() + static_cast<difference_type>(idx + count));
            size_ = heap_storage_.size();
        }
        else {
            auto *base = stack_data();
            for (size_type i = idx; i + count < size_; ++i) {
                base[i] = std::move(base[i + count]);
            }
            for (size_type i = size_ - count; i < size_; ++i) {
                std::destroy_at(base + i);
            }
            size_ -= count;
        }
        return begin() + static_cast<difference_type>(idx);
    }

    void swap(SmallVector &other) noexcept(std::is_nothrow_move_constructible_v<T> &&
                                            std::is_nothrow_move_assignable_v<T>) {
        if (this == &other) {
            return;
        }
        SmallVector tmp(std::move(other));
        other = std::move(*this);
        *this = std::move(tmp);
    }

    friend void swap(SmallVector &lhs, SmallVector &rhs) noexcept(noexcept(lhs.swap(rhs))) {
        lhs.swap(rhs);
    }

private:
    [[nodiscard]] T *stack_data() noexcept {
        return std::launder(reinterpret_cast<T *>(stack_storage_));
    }

    [[nodiscard]] const T *stack_data() const noexcept {
        return std::launder(reinterpret_cast<const T *>(stack_storage_));
    }

    void destroy_stack_elements() noexcept {
        auto *base = stack_data();
        for (size_type i = 0; i < size_; ++i) {
            std::destroy_at(base + i);
        }
    }

    /// 析构 [new_size, size_) 的元素并把 size_ 降到 new_size
    void truncate(size_type new_size) {
        if (on_heap_) {
            heap_storage_.resize(new_size);
            size_ = new_size;
            if (new_size <= N) {
                migrate_to_stack();
            }
        }
        else {
            auto *base = stack_data();
            for (size_type i = new_size; i < size_; ++i) {
                std::destroy_at(base + i);
            }
            size_ = new_size;
        }
    }

    /**
     * @brief 把栈元素迁移到堆
     *
     * 迁移失败时清掉已搬过去的元素，容器保持可用（已移动的栈元素值未指定）
     */
    void migrate_to_heap() {
        heap_storage_.reserve(N + 1);
        try {
            for (size_type i = 0; i < size_; ++i) {
                heap_storage_.push_back(std::move(stack_data()[i]));
            }
        }
        catch (...) {
            heap_storage_.clear();
            throw;
        }
        for (size_type i = 0; i < size_; ++i) {
            std::destroy_at(stack_data() + i);
        }
        on_heap_ = true;
    }

    /**
     * @brief 把 size_ <= N 个堆元素迁回栈
     *
     * 构造中途抛异常时销毁已构造的栈元素，容器仍留在堆上
     */
    void migrate_to_stack() {
        auto *base = stack_data();
        size_type constructed = 0;
        try {
            for (; constructed < size_; ++constructed) {
                std::construct_at(base + constructed, std::move(heap_storage_[constructed]));
            }
        }
        catch (...) {
            for (size_type i = 0; i < constructed; ++i) {
                std::destroy_at(base + i);
            }
            throw;
        }
        std::vector<T>().swap(heap_storage_);
        on_heap_ = false;
    }

    /// 从已构造元素的容器搬走全部元素（other 搬完后为空，栈元素被析构）
    void move_from(SmallVector &other) {
        if (other.on_heap_) {
            heap_storage_ = std::move(other.heap_storage_);
            on_heap_ = true;
            size_ = other.size_;
        }
        else {
            auto *dst = stack_data();
            auto *src = other.stack_data();
            size_type constructed = 0;
            try {
                for (; constructed < other.size_; ++constructed) {
                    std::construct_at(dst + constructed, std::move(src[constructed]));
                }
            }
            catch (...) {
                for (size_type i = 0; i < constructed; ++i) {
                    std::destroy_at(dst + i);
                }
                throw;
            }
            size_ = other.size_;
            other.destroy_stack_elements();
        }
        other.size_ = 0;
        other.on_heap_ = false;
    }

    /// 在 idx 处插入一个已构造好的元素，返回新元素迭代器
    iterator insert_value(size_type idx, T &&value) {
        if (!on_heap_ && size_ < N) {
            auto *base = stack_data();
            if (idx == size_) {
                std::construct_at(base + size_, std::move(value));
                ++size_;
            }
            else {
                const size_type old_size = size_;
                // 先把最后一个元素搬到尾部空位，再整体后移，避免多次构造
                std::construct_at(base + old_size, std::move(base[old_size - 1]));
                // 尾部元素已构造完成，先计入 size：后面移位抛异常时也不会漏析构它
                ++size_;
                for (size_type i = old_size - 1; i > idx; --i) {
                    base[i] = std::move(base[i - 1]);
                }
                base[idx] = std::move(value);
            }
            return begin() + static_cast<difference_type>(idx);
        }
        if (!on_heap_) {
            migrate_to_heap();
        }
        heap_storage_.insert(heap_storage_.begin() + static_cast<difference_type>(idx), std::move(value));
        size_ = heap_storage_.size();
        return begin() + static_cast<difference_type>(idx);
    }

    alignas(T) std::byte stack_storage_[sizeof(T) * N];
    std::vector<T> heap_storage_;
    size_type size_ = 0;
    bool on_heap_ = false;
};

/// 编译期确认迭代器满足标准库的连续迭代器要求
static_assert(std::contiguous_iterator<SmallVector<int, 4>::iterator>);
static_assert(std::contiguous_iterator<SmallVector<int, 4>::const_iterator>);

/**
 * @brief 按元素比较两个 SmallVector
 *
 * 栈容量不同的实例之间也能比较（比较的是内容，与 N 无关）。
 */
template<typename T, std::size_t N, std::size_t M>
bool operator==(const SmallVector<T, N> &lhs, const SmallVector<T, M> &rhs) {
    return lhs.size() == rhs.size() && std::equal(lhs.begin(), lhs.end(), rhs.begin());
}

template<typename T, std::size_t N, std::size_t M>
auto operator<=>(const SmallVector<T, N> &lhs, const SmallVector<T, M> &rhs) {
    return std::lexicographical_compare_three_way(lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
}

} // namespace usbipdcpp
