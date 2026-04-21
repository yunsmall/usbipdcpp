#include <gtest/gtest.h>

#include "protocol.h"

using namespace usbipdcpp;

// ============== GenericTransfer 测试 ==============

TEST(TestGenericTransfer, FromHandle) {
    GenericTransfer trx;
    trx.data = {0x01, 0x02, 0x03, 0x04};
    trx.actual_length = 4;

    void* handle = &trx;
    auto* retrieved = GenericTransfer::from_handle(handle);

    EXPECT_EQ(retrieved->data.size(), 4);
    EXPECT_EQ(retrieved->data[0], 0x01);
    EXPECT_EQ(retrieved->actual_length, 4);
}

TEST(TestGenericTransfer, IsoDescriptors) {
    GenericTransfer trx;
    trx.iso_descriptors = {
        {.offset = 0, .length = 1024, .actual_length = 512, .status = 0},
        {.offset = 1024, .length = 1024, .actual_length = 1024, .status = 0}
    };

    EXPECT_EQ(trx.iso_descriptors.size(), 2);
    EXPECT_EQ(trx.iso_descriptors[0].length, 1024);
}

TEST(TestGenericTransfer, DataOffset) {
    GenericTransfer trx;
    trx.data.resize(100);
    trx.data_offset = 8;
    trx.actual_length = 92;

    EXPECT_EQ(trx.data.size(), 100);
    EXPECT_EQ(trx.data_offset, 8);
    EXPECT_EQ(trx.actual_length, 92);
}

// ============== 极端情况测试 ==============

TEST(TestGenericTransfer, EmptyData) {
    GenericTransfer trx;
    trx.data = {};
    trx.actual_length = 0;

    EXPECT_TRUE(trx.data.empty());
    EXPECT_EQ(trx.actual_length, 0);
}

TEST(TestGenericTransfer, LargeData) {
    GenericTransfer trx;
    trx.data.resize(1024 * 1024);  // 1MB
    trx.actual_length = trx.data.size();

    EXPECT_EQ(trx.data.size(), 1024 * 1024);
}

TEST(TestGenericTransfer, ManyIsoDescriptors) {
    GenericTransfer trx;
    constexpr int num_iso = 255;
    trx.iso_descriptors.resize(num_iso);
    for (int i = 0; i < num_iso; ++i) {
        trx.iso_descriptors[i] = {
            .offset = static_cast<std::uint32_t>(i * 1024),
            .length = 1024,
            .actual_length = 1024,
            .status = 0
        };
    }

    EXPECT_EQ(trx.iso_descriptors.size(), num_iso);
    EXPECT_EQ(trx.iso_descriptors.back().offset, static_cast<std::uint32_t>((num_iso - 1) * 1024));
}

TEST(TestGenericTransfer, ZeroDataOffset) {
    GenericTransfer trx;
    trx.data = {0x01, 0x02, 0x03};
    trx.data_offset = 0;
    trx.actual_length = 3;

    EXPECT_EQ(trx.data_offset, 0);
    EXPECT_EQ(trx.data[0], 0x01);
}

TEST(TestGenericTransfer, MaxValues) {
    GenericTransfer trx;
    trx.actual_length = std::numeric_limits<std::size_t>::max();
    trx.data_offset = std::numeric_limits<std::size_t>::max();

    EXPECT_EQ(trx.actual_length, std::numeric_limits<std::size_t>::max());
    EXPECT_EQ(trx.data_offset, std::numeric_limits<std::size_t>::max());
}

TEST(TestGenericTransfer, NullptrHandle) {
    void* handle = nullptr;
    auto* retrieved = GenericTransfer::from_handle(handle);
    EXPECT_EQ(retrieved, nullptr);
}

TEST(TestGenericTransfer, MoveSemantics) {
    GenericTransfer trx1;
    trx1.data = {1, 2, 3, 4, 5};
    trx1.actual_length = 5;

    GenericTransfer trx2 = std::move(trx1);

    EXPECT_EQ(trx2.data.size(), 5);
    EXPECT_EQ(trx2.actual_length, 5);
    // trx1 的状态未定义，但不应崩溃
}

TEST(TestGenericTransfer, ClearData) {
    GenericTransfer trx;
    trx.data = {1, 2, 3, 4, 5};
    trx.actual_length = 5;

    trx.data.clear();
    trx.actual_length = 0;

    EXPECT_TRUE(trx.data.empty());
    EXPECT_EQ(trx.actual_length, 0);
}

TEST(TestGenericTransfer, IsoDescriptorsStatusValues) {
    GenericTransfer trx;
    trx.iso_descriptors = {
        {.offset = 0, .length = 1024, .actual_length = 1024, .status = 0},    // 成功
        {.offset = 1024, .length = 1024, .actual_length = 0, .status = 1},    // 错误
        {.offset = 2048, .length = 1024, .actual_length = 512, .status = 0}   // 部分传输
    };

    EXPECT_EQ(trx.iso_descriptors[0].status, 0);
    EXPECT_EQ(trx.iso_descriptors[1].status, 1);
    EXPECT_EQ(trx.iso_descriptors[2].actual_length, 512);
}