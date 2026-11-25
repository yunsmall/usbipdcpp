// bulk OUT 分块大小对齐逻辑：配置值必须落在 wMaxPacketSize 的整数倍上，
// 否则每个分块末尾都会出现短包，设备固件会把分块边界误当成传输结尾

#include <gtest/gtest.h>

#include "usbipdcpp/LibusbHandler/LibusbDeviceHandler.h"
#include "usbipdcpp/LibusbHandler/LibusbTransferOperator.h"

using namespace usbipdcpp;

TEST(TestLibusbBulkChunkAlignment, AlreadyAlignedKeepsValue) {
    EXPECT_EQ(LibusbDeviceHandler::align_bulk_out_chunk(1024, 512), 1024u);
    EXPECT_EQ(LibusbDeviceHandler::align_bulk_out_chunk(1536, 512), 1536u);
    EXPECT_EQ(LibusbDeviceHandler::align_bulk_out_chunk(512, 512), 512u);
    EXPECT_EQ(LibusbDeviceHandler::align_bulk_out_chunk(2048, 1024), 2048u);
}

TEST(TestLibusbBulkChunkAlignment, DownRoundsToPacketMultiple) {
    EXPECT_EQ(LibusbDeviceHandler::align_bulk_out_chunk(1000, 512), 512u);
    EXPECT_EQ(LibusbDeviceHandler::align_bulk_out_chunk(1023, 512), 512u);
    EXPECT_EQ(LibusbDeviceHandler::align_bulk_out_chunk(1535, 512), 1024u);
    EXPECT_EQ(LibusbDeviceHandler::align_bulk_out_chunk(3000, 1024), 2048u);
}

TEST(TestLibusbBulkChunkAlignment, LessThanOnePacketUsesOnePacket) {
    EXPECT_EQ(LibusbDeviceHandler::align_bulk_out_chunk(1, 512), 512u);
    EXPECT_EQ(LibusbDeviceHandler::align_bulk_out_chunk(511, 512), 512u);
    EXPECT_EQ(LibusbDeviceHandler::align_bulk_out_chunk(1, 1024), 1024u);
}

TEST(TestLibusbBulkChunkAlignment, DisabledOrUnknownPacketSize) {
    // 0 = 未启用分块，保持 0 让调用方按“不分块”处理
    EXPECT_EQ(LibusbDeviceHandler::align_bulk_out_chunk(0, 512), 0u);
    // 描述符没给出有效 wMaxPacketSize 时无法对齐，原样返回
    EXPECT_EQ(LibusbDeviceHandler::align_bulk_out_chunk(1000, 0), 1000u);
}

TEST(TestLibusbBulkChunkAlignment, FullAndSuperSpeedPacketSizes) {
    // 全速 bulk maxpacket=64
    EXPECT_EQ(LibusbDeviceHandler::align_bulk_out_chunk(100, 64), 64u);
    EXPECT_EQ(LibusbDeviceHandler::align_bulk_out_chunk(128, 64), 128u);
    EXPECT_EQ(LibusbDeviceHandler::align_bulk_out_chunk(1, 64), 64u);
    // 超速 bulk maxpacket=1024
    EXPECT_EQ(LibusbDeviceHandler::align_bulk_out_chunk(1000, 1024), 1024u);
    EXPECT_EQ(LibusbDeviceHandler::align_bulk_out_chunk(4096, 1024), 1024u * 4);
}

// ============== 分块路径借用的 libusb_transfer 池 ==============

TEST(TestLibusbTransferPool, BorrowReturnReusesObject) {
    LibusbTransferOperator op;
    libusb_transfer *first = op.alloc_transfer();
    ASSERT_NE(first, nullptr);
    EXPECT_EQ(first->buffer, nullptr);
    op.free_transfer(first);

    libusb_transfer *second = op.alloc_transfer();
    // 池子 LIFO：刚归还的对象会先被借出
    EXPECT_EQ(second, first);
    EXPECT_EQ(second->buffer, nullptr);
    op.free_transfer(second);
}

TEST(TestLibusbTransferPool, ExhaustAndRefill) {
    LibusbTransferOperator op;
    std::vector<libusb_transfer *> transfers;
    // 池大小 64；借光后仍必须回退到 libusb_alloc_transfer，不能返回空
    for (int i = 0; i < 80; ++i) {
        auto *trx = op.alloc_transfer();
        ASSERT_NE(trx, nullptr) << "第 " << i << " 个";
        EXPECT_EQ(trx->buffer, nullptr);
        transfers.push_back(trx);
    }
    for (auto *trx: transfers) {
        op.free_transfer(trx);
    }
    // 全部归还后再借 80 个仍应成功（池 + 回退）
    transfers.clear();
    for (int i = 0; i < 80; ++i) {
        auto *trx = op.alloc_transfer();
        ASSERT_NE(trx, nullptr);
        transfers.push_back(trx);
    }
    for (auto *trx: transfers) {
        op.free_transfer(trx);
    }
}

TEST(TestLibusbTransferPool, FreeTransferDoesNotFreeCallerBuffer) {
    LibusbTransferOperator op;
    std::vector<unsigned char> buffer(64, 0x5a);
    auto *trx = op.alloc_transfer();
    ASSERT_NE(trx, nullptr);
    trx->buffer = buffer.data();
    trx->length = static_cast<int>(buffer.size());
    op.free_transfer(trx);

    // buffer 归调用方（分块槽位）所有：free_transfer 只能归还 transfer，
    // 不能 free 这块内存；ASan 下错误实现会直接报 heap-use-after-free
    EXPECT_EQ(buffer.front(), 0x5a);
    EXPECT_EQ(buffer.back(), 0x5a);
}

TEST(TestLibusbTransferPool, NormalHandleAndBorrowedTransferSharePoolSafely) {
    LibusbTransferOperator op;
    UsbIpHeaderBasic header{};
    SetupPacket setup{};
    auto *normal = static_cast<libusb_transfer *>(op.alloc_transfer_handle(64, 0, header, setup));
    ASSERT_NE(normal, nullptr);
    ASSERT_NE(normal->buffer, nullptr);
    op.free_transfer_handle(normal);

    // 普通路径归还的 transfer 可能被分块路径借到；alloc_transfer 必须清掉
    // 上一个用户留下的 dangling buffer 指针
    auto *borrowed = op.alloc_transfer();
    ASSERT_NE(borrowed, nullptr);
    EXPECT_EQ(borrowed->buffer, nullptr);
    op.free_transfer(borrowed);

    // 反过来：分块路径归还后，普通路径重新 alloc 必须重新分配自己的 buffer，
    // 不能继续用槽位内存
    std::vector<unsigned char> slot_buffer(8, 0x11);
    borrowed = op.alloc_transfer();
    ASSERT_NE(borrowed, nullptr);
    borrowed->buffer = slot_buffer.data();
    op.free_transfer(borrowed);

    normal = static_cast<libusb_transfer *>(op.alloc_transfer_handle(32, 0, header, setup));
    ASSERT_NE(normal, nullptr);
    EXPECT_NE(normal->buffer, nullptr);
    EXPECT_NE(normal->buffer, slot_buffer.data());
    op.free_transfer_handle(normal);
}
