#include "utils/RingBuffer.h"

#include <algorithm>

namespace usbipdcpp {

RingBuffer::RingBuffer(std::size_t capacity) :
    capacity_(capacity) {
    // 延迟分配：不在构造时分配内存，由 resize 或 write 触发
}

std::size_t RingBuffer::write(const std::uint8_t *data, std::size_t size) {
    if (capacity_ == 0) {
        return 0;  // 容量为0时不写入
    }

    std::size_t available = capacity_ - count_;
    std::size_t to_write = std::min(size, available);

    // 延迟分配：首次写入时才分配内存
    if (buffer_.size() < capacity_) {
        buffer_.resize(capacity_);
    }

    for (std::size_t i = 0; i < to_write; ++i) {
        buffer_[tail_] = data[i];
        tail_ = (tail_ + 1) % capacity_;
        ++count_;
    }

    return to_write;
}

std::size_t RingBuffer::read(std::uint8_t *data, std::size_t max_size) {
    std::size_t to_read = std::min(max_size, count_);

    for (std::size_t i = 0; i < to_read; ++i) {
        data[i] = buffer_[head_];
        head_ = (head_ + 1) % capacity_;
        --count_;
    }

    return to_read;
}

std::size_t RingBuffer::peek(std::uint8_t *data, std::size_t max_size) const {
    std::size_t to_peek = std::min(max_size, count_);
    std::size_t pos = head_;

    for (std::size_t i = 0; i < to_peek; ++i) {
        data[i] = buffer_[pos];
        pos = (pos + 1) % capacity_;
    }

    return to_peek;
}

std::size_t RingBuffer::size() const {
    return count_;
}

std::size_t RingBuffer::capacity() const {
    return capacity_;
}

std::size_t RingBuffer::available() const {
    return capacity_ - count_;
}

bool RingBuffer::empty() const {
    return count_ == 0;
}

bool RingBuffer::full() const {
    return count_ == capacity_;
}

void RingBuffer::clear() {
    head_ = 0;
    tail_ = 0;
    count_ = 0;
}

void RingBuffer::resize(std::size_t new_capacity) {
    if (new_capacity == 0) {
        // 清空并释放内存
        buffer_.clear();
        buffer_.shrink_to_fit();
        capacity_ = 0;
        head_ = 0;
        tail_ = 0;
        count_ = 0;
        return;
    }

    // 读取现有数据
    std::vector<std::uint8_t> old_data(count_);
    read(old_data.data(), count_);

    // 释放旧内存，重新分配到精确大小
    buffer_.clear();
    buffer_.shrink_to_fit();
    buffer_.resize(new_capacity);
    capacity_ = new_capacity;
    head_ = 0;
    tail_ = 0;
    count_ = 0;

    // 写回数据（不超过新容量）
    if (!old_data.empty()) {
        write(old_data.data(), std::min(old_data.size(), new_capacity));
    }
}

}
