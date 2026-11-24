// bulk OUT 分块大小对齐逻辑：配置值必须落在 wMaxPacketSize 的整数倍上，
// 否则每个分块末尾都会出现短包，设备固件会把分块边界误当成传输结尾

#include <gtest/gtest.h>

#include "usbipdcpp/LibusbHandler/LibusbDeviceHandler.h"

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
    EXPECT_EQ(LibusbDeviceHandler::align_bulk_out_chunk(4096, 1024), 4096u);
}
