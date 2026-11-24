// SocketTransferSource 的边界测试：真实 TCP 回环下的精确读/分片读、
// remaining 记账、等时描述符读取、skip/drain 的流边界，以及 EOF 错误路径。
// MemoryTransferSource 只覆盖设备层的同步消费，测不到这些 socket 行为

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "test_utils.h"

#include "usbipdcpp/TransferSource.h"
#include "usbipdcpp/protocol.h"

using namespace usbipdcpp;
using namespace usbipdcpp::test;

namespace {

/// 把若干字节写到对端（SocketPair 的 send_sock），接收端由 recv_sock 读
void send_bytes(SocketPair &pair, const std::vector<std::uint8_t> &data) {
    asio::write(pair.send_sock, asio::buffer(data));
}

std::vector<std::uint8_t> iso_bytes(const std::vector<UsbIpIsoPacketDescriptor> &descriptors) {
    std::vector<std::uint8_t> bytes;
    for (const auto &descriptor: descriptors) {
        auto encoded = descriptor.to_bytes();
        bytes.insert(bytes.end(), encoded.begin(), encoded.end());
    }
    return bytes;
}

std::vector<std::uint8_t> sequential_bytes(std::size_t count, std::uint8_t seed = 0) {
    std::vector<std::uint8_t> bytes(count);
    for (std::size_t i = 0; i < count; ++i) {
        bytes[i] = static_cast<std::uint8_t>(seed + i);
    }
    return bytes;
}

} // namespace

TEST(TestSocketTransferSource, ReadConsumesExactPayloadAndKeepsStreamBoundary) {
    SocketPair pair;
    // payload 只有 4 字节，后面 6 字节属于下一条命令/描述符，不能被 read 吃掉
    auto bytes = sequential_bytes(10);
    send_bytes(pair, bytes);

    SocketTransferSource source(pair.recv_sock, 4, 0);
    EXPECT_EQ(source.remaining(), 4u);

    std::array<std::byte, 4> first{};
    std::error_code ec;
    EXPECT_EQ(source.read(std::span(first), ec), 4u);
    EXPECT_FALSE(ec);
    EXPECT_EQ(source.remaining(), 0u);
    for (std::size_t i = 0; i < first.size(); ++i) {
        EXPECT_EQ(static_cast<std::uint8_t>(first[i]), bytes[i]);
    }

    // remaining 为 0 时 read 不碰 socket
    std::array<std::byte, 4> again{};
    EXPECT_EQ(source.read(std::span(again), ec), 0u);
    EXPECT_FALSE(ec);

    // 剩余 6 字节仍在流里，直接从 socket 读出来验证没有被误消费
    std::array<std::uint8_t, 6> rest{};
    asio::read(pair.recv_sock, asio::buffer(rest));
    for (std::size_t i = 0; i < rest.size(); ++i) {
        EXPECT_EQ(rest[i], bytes[4 + i]);
    }
}

TEST(TestSocketTransferSource, ReadSomeHandlesShortReadsAndAccounting) {
    SocketPair pair;
    SocketTransferSource source(pair.recv_sock, 8, 0);

    // 先只给 3 字节：read_some 必须返回可用的 3 字节并正确扣减 remaining
    auto first = sequential_bytes(3, 0x10);
    send_bytes(pair, first);
    std::array<std::byte, 8> buffer{};
    std::error_code ec;
    auto got = source.read_some(std::span(buffer), ec);
    EXPECT_EQ(got, 3u);
    EXPECT_FALSE(ec);
    EXPECT_EQ(source.remaining(), 5u);

    // 再给 5 字节，读满剩余部分
    auto second = sequential_bytes(5, 0x20);
    send_bytes(pair, second);
    got = source.read_some(std::span(buffer.data() + 3, 5), ec);
    EXPECT_EQ(got, 5u);
    EXPECT_FALSE(ec);
    EXPECT_EQ(source.remaining(), 0u);
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(static_cast<std::uint8_t>(buffer[i]), first[i]);
    }
    for (std::size_t i = 0; i < 5; ++i) {
        EXPECT_EQ(static_cast<std::uint8_t>(buffer[3 + i]), second[i]);
    }
}

TEST(TestSocketTransferSource, IsoDescriptorsReadInOrderAndRespectCount) {
    SocketPair pair;
    const std::vector<UsbIpIsoPacketDescriptor> expected = {
            {.offset = 0, .length = 64, .actual_length = 64, .status = 0},
            {.offset = 64, .length = 128, .actual_length = 100, .status = 0x20},
    };
    send_bytes(pair, iso_bytes(expected));

    SocketTransferSource source(pair.recv_sock, 0, 2);
    EXPECT_EQ(source.iso_packets_remaining(), 2);

    std::array<UsbIpIsoPacketDescriptor, 1> first{};
    std::error_code ec;
    source.read_iso_descriptors(std::span(first), ec);
    EXPECT_FALSE(ec);
    EXPECT_EQ(source.iso_packets_remaining(), 1);
    EXPECT_EQ(first[0].offset, expected[0].offset);
    EXPECT_EQ(first[0].length, expected[0].length);
    EXPECT_EQ(first[0].actual_length, expected[0].actual_length);
    EXPECT_EQ(first[0].status, expected[0].status);

    std::array<UsbIpIsoPacketDescriptor, 2> rest{};
    source.read_iso_descriptors(std::span(rest), ec);
    EXPECT_FALSE(ec);
    EXPECT_EQ(source.iso_packets_remaining(), 0);
    EXPECT_EQ(rest[0].actual_length, expected[1].actual_length);
    EXPECT_EQ(rest[0].status, expected[1].status);

    // 已读完时再要描述符不应读 socket，也不应改计数
    source.read_iso_descriptors(std::span(rest), ec);
    EXPECT_FALSE(ec);
    EXPECT_EQ(source.iso_packets_remaining(), 0);
}

TEST(TestSocketTransferSource, SkipRemainingConsumesMultiChunkPayload) {
    SocketPair pair;
    // 超过内部 4096 字节 scratch，覆盖循环快路径
    auto bytes = sequential_bytes(10000, 0x40);
    send_bytes(pair, bytes);

    SocketTransferSource source(pair.recv_sock, 10000, 0);
    std::error_code ec;
    source.skip_remaining(ec);
    EXPECT_FALSE(ec);
    EXPECT_EQ(source.remaining(), 0u);

    // socket 流已经被精确消费到 payload 末尾
    std::array<std::byte, 1> one{};
    EXPECT_EQ(source.read(std::span(one), ec), 0u);
    EXPECT_FALSE(ec);
}

TEST(TestSocketTransferSource, MarkConsumedClampsToRemaining) {
    SocketPair pair;
    SocketTransferSource source(pair.recv_sock, 100, 0);

    source.mark_consumed(30);
    EXPECT_EQ(source.remaining(), 70u);
    source.mark_consumed(0);
    EXPECT_EQ(source.remaining(), 70u);
    // 超过剩余量必须钳位，不能下溢成巨大值
    source.mark_consumed(1000);
    EXPECT_EQ(source.remaining(), 0u);
}

TEST(TestSocketTransferSource, DrainSkipsPayloadThenReadsLeftoverIsoDescriptors) {
    SocketPair pair;
    auto payload = sequential_bytes(5, 0);
    const std::vector<UsbIpIsoPacketDescriptor> descriptors = {
            {.offset = 0, .length = 2, .actual_length = 2, .status = 0},
            {.offset = 2, .length = 3, .actual_length = 3, .status = 0},
    };
    std::vector<std::uint8_t> stream = payload;
    auto encoded = iso_bytes(descriptors);
    stream.insert(stream.end(), encoded.begin(), encoded.end());
    const std::vector<std::uint8_t> tail = {'N', 'E', 'X', 'T'};
    stream.insert(stream.end(), tail.begin(), tail.end());
    send_bytes(pair, stream);

    SocketTransferSource source(pair.recv_sock, 5, 2);
    std::array<std::byte, 2> head{};
    std::error_code ec;
    EXPECT_EQ(source.read(std::span(head), ec), 2u);
    EXPECT_FALSE(ec);
    EXPECT_EQ(source.remaining(), 3u);

    source.drain(ec);
    EXPECT_FALSE(ec);
    EXPECT_EQ(source.remaining(), 0u);
    EXPECT_EQ(source.iso_packets_remaining(), 0);

    // drain 不能多读：描述符之后的字节仍属于下一条命令
    std::array<std::uint8_t, 4> rest{};
    asio::read(pair.recv_sock, asio::buffer(rest));
    EXPECT_EQ(std::vector<std::uint8_t>(rest.begin(), rest.end()), tail);

    // 已经 drain 干净时再调用是空操作
    source.drain(ec);
    EXPECT_FALSE(ec);
}

TEST(TestSocketTransferSource, DrainReadsDescriptorsEvenWhenPayloadWasFullyConsumed) {
    SocketPair pair;
    const std::vector<UsbIpIsoPacketDescriptor> descriptors = {
            {.offset = 0, .length = 64, .actual_length = 64, .status = 0},
    };
    send_bytes(pair, iso_bytes(descriptors));

    SocketTransferSource source(pair.recv_sock, 0, 1);
    std::error_code ec;
    source.drain(ec);
    EXPECT_FALSE(ec);
    EXPECT_EQ(source.iso_packets_remaining(), 0);
}

TEST(TestSocketTransferSource, ReadAndDrainPropagateSocketEof) {
    {
        SocketPair pair;
        SocketTransferSource source(pair.recv_sock, 10, 0);
        pair.send_sock.close();

        std::array<std::byte, 10> buffer{};
        std::error_code ec;
        EXPECT_EQ(source.read(std::span(buffer), ec), 0u);
        EXPECT_TRUE(ec);
        // 读失败没有消费任何字节
        EXPECT_EQ(source.remaining(), 10u);
    }
    {
        SocketPair pair;
        SocketTransferSource source(pair.recv_sock, 10, 1);
        pair.send_sock.close();

        std::error_code ec;
        source.drain(ec);
        EXPECT_TRUE(ec);
    }
}

TEST(TestSocketTransferSource, SocketPointerIsTheUnderlyingSocket) {
    SocketPair pair;
    SocketTransferSource source(pair.recv_sock, 0, 0);
    EXPECT_EQ(source.socket(), &pair.recv_sock);
}
