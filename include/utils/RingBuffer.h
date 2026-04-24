#pragma once

#include <vector>
#include <cstdint>
#include <cstddef>

namespace usbipdcpp {

/**
 * @brief 环形缓冲区，支持延迟分配
 */
class RingBuffer {
public:
    explicit RingBuffer(std::size_t capacity = 64 * 1024);

    /**
     * @brief 写入数据
     * @param data 数据指针
     * @param size 数据大小
     * @return 实际写入的字节数
     */
    std::size_t write(const std::uint8_t *data, std::size_t size);

    /**
     * @brief 读取数据
     * @param data 数据缓冲区
     * @param max_size 最大读取字节数
     * @return 实际读取的字节数
     */
    std::size_t read(std::uint8_t *data, std::size_t max_size);

    /**
     * @brief 查看数据（不移除）
     * @param data 数据缓冲区
     * @param max_size 最大查看字节数
     * @return 实际查看的字节数
     */
    std::size_t peek(std::uint8_t *data, std::size_t max_size) const;

    /**
     * @brief 获取当前数据量
     * @return 缓冲区中已使用字节数
     */
    [[nodiscard]] std::size_t size() const;

    /**
     * @brief 获取缓冲区容量
     * @return 缓冲区总容量
     */
    [[nodiscard]] std::size_t capacity() const;

    /**
     * @brief 获取缓冲区剩余空间
     * @return 缓冲区可用字节数
     */
    [[nodiscard]] std::size_t available() const;

    /**
     * @brief 检查缓冲区是否为空
     * @return true 表示为空
     */
    [[nodiscard]] bool empty() const;

    /**
     * @brief 检查缓冲区是否已满
     * @return true 表示已满
     */
    [[nodiscard]] bool full() const;

    /**
     * @brief 清空缓冲区
     */
    void clear();

    /**
     * @brief 调整缓冲区容量
     * @param new_capacity 新容量
     */
    void resize(std::size_t new_capacity);

private:
    std::vector<std::uint8_t> buffer_;
    std::size_t capacity_;
    std::size_t head_ = 0;
    std::size_t tail_ = 0;
    std::size_t count_ = 0;
};

}
