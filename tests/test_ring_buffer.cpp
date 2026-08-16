#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>

#include "usbipdcpp/utils/RingBuffer.h"

using namespace usbipdcpp;

TEST(RingBuffer, WriteRead) {
    RingBuffer rb(8);
    const std::array<std::uint8_t, 5> data = {1, 2, 3, 4, 5};

    EXPECT_EQ(rb.write(data.data(), data.size()), 5u);
    EXPECT_EQ(rb.size(), 5u);
    EXPECT_EQ(rb.available(), 3u);
    EXPECT_FALSE(rb.empty());
    EXPECT_FALSE(rb.full());

    std::array<std::uint8_t, 8> out{};
    EXPECT_EQ(rb.read(out.data(), 5), 5u);
    EXPECT_TRUE(std::equal(out.begin(), out.begin() + 5, data.begin()));
    EXPECT_TRUE(rb.empty());
    EXPECT_EQ(rb.size(), 0u);
}

TEST(RingBuffer, WrapAround) {
    RingBuffer rb(4);
    std::array<std::uint8_t, 4> out{};

    // 写满
    const std::array<std::uint8_t, 4> d1 = {1, 2, 3, 4};
    EXPECT_EQ(rb.write(d1.data(), 4), 4u);
    EXPECT_TRUE(rb.full());
    // 满时再写返回 0
    EXPECT_EQ(rb.write(d1.data(), 1), 0u);

    // 读走 2 个，再写 2 个，tail 绕回
    EXPECT_EQ(rb.read(out.data(), 2), 2u);
    const std::array<std::uint8_t, 2> d2 = {5, 6};
    EXPECT_EQ(rb.write(d2.data(), 2), 2u);

    EXPECT_EQ(rb.read(out.data(), 4), 4u);
    const std::array<std::uint8_t, 4> expect = {3, 4, 5, 6};
    EXPECT_TRUE(std::equal(out.begin(), out.end(), expect.begin()));
}

TEST(RingBuffer, ZeroCapacity) {
    RingBuffer rb(0);
    const std::array<std::uint8_t, 1> data = {1};
    EXPECT_EQ(rb.write(data.data(), 1), 0u);
    EXPECT_EQ(rb.size(), 0u);
    EXPECT_TRUE(rb.empty());
    EXPECT_TRUE(rb.full()); // 0 容量恒满
}

TEST(RingBuffer, PartialWrite) {
    RingBuffer rb(4);
    const std::array<std::uint8_t, 8> data = {1, 2, 3, 4, 5, 6, 7, 8};
    // 容量 4，只写入前 4 个
    EXPECT_EQ(rb.write(data.data(), data.size()), 4u);
    EXPECT_TRUE(rb.full());

    std::array<std::uint8_t, 8> out{};
    EXPECT_EQ(rb.read(out.data(), 8), 4u);
    EXPECT_EQ(out[0], 1);
    EXPECT_EQ(out[3], 4);
}

TEST(RingBuffer, PeekDoesNotConsume) {
    RingBuffer rb(4);
    const std::array<std::uint8_t, 2> data = {9, 8};
    rb.write(data.data(), 2);

    std::array<std::uint8_t, 2> peeked{};
    EXPECT_EQ(rb.peek(peeked.data(), 2), 2u);
    EXPECT_EQ(rb.size(), 2u); // 未消费
    EXPECT_EQ(peeked[0], 9);
    EXPECT_EQ(peeked[1], 8);

    // peek 超过现有数据只返回已有部分
    std::array<std::uint8_t, 8> big{};
    EXPECT_EQ(rb.peek(big.data(), 8), 2u);
}

TEST(RingBuffer, Clear) {
    RingBuffer rb(4);
    const std::array<std::uint8_t, 3> data = {1, 2, 3};
    rb.write(data.data(), 3);
    rb.clear();
    EXPECT_TRUE(rb.empty());
    EXPECT_EQ(rb.size(), 0u);
    EXPECT_EQ(rb.available(), 4u);

    // 清空后可正常复用
    EXPECT_EQ(rb.write(data.data(), 3), 3u);
    EXPECT_EQ(rb.size(), 3u);
}

TEST(RingBuffer, ResizePreservesData) {
    RingBuffer rb(4);
    const std::array<std::uint8_t, 4> data = {1, 2, 3, 4};
    rb.write(data.data(), 4);

    // 扩大容量，保留全部数据
    rb.resize(8);
    EXPECT_EQ(rb.capacity(), 8u);
    EXPECT_EQ(rb.size(), 4u);
    std::array<std::uint8_t, 8> out{};
    EXPECT_EQ(rb.read(out.data(), 8), 4u);
    EXPECT_EQ(out[0], 1);
    EXPECT_EQ(out[3], 4);
}

TEST(RingBuffer, ResizeShrinkTruncates) {
    RingBuffer rb(4);
    const std::array<std::uint8_t, 4> data = {1, 2, 3, 4};
    rb.write(data.data(), 4);

    // 缩小容量，保留最旧的数据（FIFO 顺序截断）
    rb.resize(2);
    EXPECT_EQ(rb.capacity(), 2u);
    EXPECT_EQ(rb.size(), 2u);
    std::array<std::uint8_t, 4> out{};
    EXPECT_EQ(rb.read(out.data(), 4), 2u);
    EXPECT_EQ(out[0], 1);
    EXPECT_EQ(out[1], 2);

    // 缩小后可继续写入
    const std::array<std::uint8_t, 2> more = {5, 6};
    EXPECT_EQ(rb.write(more.data(), 2), 2u);
    EXPECT_EQ(rb.read(out.data(), 4), 2u);
    EXPECT_EQ(out[0], 5);
    EXPECT_EQ(out[1], 6);
}

TEST(RingBuffer, ResizeToZero) {
    RingBuffer rb(4);
    const std::array<std::uint8_t, 4> data = {1, 2, 3, 4};
    rb.write(data.data(), 4);

    rb.resize(0);
    EXPECT_EQ(rb.capacity(), 0u);
    EXPECT_TRUE(rb.empty());
    EXPECT_EQ(rb.write(data.data(), 4), 0u);

    // 从 0 扩回可正常使用
    rb.resize(4);
    EXPECT_EQ(rb.write(data.data(), 4), 4u);
    EXPECT_EQ(rb.size(), 4u);
}
