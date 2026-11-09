/**
 * @file test_iso_pattern.cpp
 * @brief libusb e2e 的 iso 包填充/校验模式测试
 *
 * 这套格式是 e2e 用来判断“收到的数据确实是设备按槽位填的”的唯一依据，
 * 设备侧和客户端共用；填充与校验必须严格互逆，否则 e2e 会假通过或假失败。
 */

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "e2e/libusb/iso_pattern.hpp"

namespace {

using usbipdcpp::e2e::iso::fill_packet;
using usbipdcpp::e2e::iso::verify_packet;

// 未写入的字节用 0xee 哨兵初始化：设备/客户端漏写时能被校验发现
std::vector<std::uint8_t> make_packet(std::uint32_t length, std::uint32_t index) {
    std::vector<std::uint8_t> data(length, 0xee);
    if (length > 0) {
        fill_packet(data.data(), length, index);
    }
    return data;
}

}  // namespace

TEST(IsoPattern, FillVerifyRoundTrip) {
    for (std::uint32_t length = 0; length <= 64; ++length) {
        auto data = make_packet(length, 12345);
        EXPECT_TRUE(verify_packet(data.data(), length)) << "length=" << length;
    }
    // e2e 里实际使用的几个大长度
    for (std::uint32_t length: {100u, 256u, 512u, 1024u, 8192u}) {
        auto data = make_packet(length, 7);
        EXPECT_TRUE(verify_packet(data.data(), length)) << "length=" << length;
    }
}

TEST(IsoPattern, DetectsCorruptedByte) {
    for (std::uint32_t length = 1; length <= 64; ++length) {
        if (length == 4) {
            // 4 字节包只携带包序号，没有冗余可校验：改序号后仍是合法包
            continue;
        }
        auto data = make_packet(length, 100);
        // 4~7 字节改派生字节，>=8 字节改校验字，<4 字节改长度派生字节
        const std::uint32_t position = length >= 8 ? 5 : (length >= 5 ? 4 : 0);
        data[position] ^= 0x01;
        EXPECT_FALSE(verify_packet(data.data(), length)) << "length=" << length;
    }
}

TEST(IsoPattern, DetectsUnwrittenSlot) {
    for (std::uint32_t length = 1; length <= 64; ++length) {
        if (length == 4) {
            // 4 字节包只装得下包序号本身，任意 4 字节都是合法包，没有可检出的冗余
            continue;
        }
        std::vector<std::uint8_t> data(length, 0xee);
        EXPECT_FALSE(verify_packet(data.data(), length)) << "length=" << length;
    }
    // 零长度包没有内容，任何字节都不读，视为匹配
    const std::uint8_t sentinel = 0xee;
    EXPECT_TRUE(verify_packet(&sentinel, 0));
}

TEST(IsoPattern, DifferentIndexChangesContent) {
    // 长度 >= 4 时包序号在头部，不同序号必须产生不同内容
    for (std::uint32_t length: {4u, 7u, 8u, 1024u}) {
        auto first = make_packet(length, 1);
        auto second = make_packet(length, 2);
        EXPECT_NE(first, second) << "length=" << length;
    }
}
