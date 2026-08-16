#include <gtest/gtest.h>

#include <cstdint>

#include "usbipdcpp/Interface.h"
#include "usbipdcpp/utils/StringPool.h"
#include "usbipdcpp/virtual_device/CdcAcmVirtualInterfaceHandler.h"

using namespace usbipdcpp;

// ========== CDC ACM LineCoding 序列化 ==========

TEST(VirtualDeviceHandlers, CdcLineCodingRoundTrip) {
    LineCoding coding{.dwDTERate = 115200, .bCharFormat = 1, .bParityType = 2, .bDataBits = 8};

    auto bytes = coding.to_bytes();
    ASSERT_EQ(bytes.size(), 7u);
    // 115200 = 0x1C200，小端
    EXPECT_EQ(bytes[0], 0x00);
    EXPECT_EQ(bytes[1], 0xC2);
    EXPECT_EQ(bytes[2], 0x01);
    EXPECT_EQ(bytes[3], 0x00);
    EXPECT_EQ(bytes[4], 1); // 停止位
    EXPECT_EQ(bytes[5], 2); // 偶校验
    EXPECT_EQ(bytes[6], 8); // 数据位

    auto restored = LineCoding::from_bytes({bytes.begin(), bytes.end()});
    EXPECT_EQ(restored.dwDTERate, 115200u);
    EXPECT_EQ(restored.bCharFormat, 1);
    EXPECT_EQ(restored.bParityType, 2);
    EXPECT_EQ(restored.bDataBits, 8);
}

TEST(VirtualDeviceHandlers, CdcLineCodingFromShortDataKeepsDefaults) {
    // 少于 7 字节时保持默认值，不越界
    auto coding = LineCoding::from_bytes({0x00, 0x01, 0x02});
    EXPECT_EQ(coding.dwDTERate, 115200u);
    EXPECT_EQ(coding.bCharFormat, 0);
    EXPECT_EQ(coding.bParityType, 0);
    EXPECT_EQ(coding.bDataBits, 8);
}

TEST(VirtualDeviceHandlers, CdcControlSignalStateRoundTrip) {
    ControlSignalState state{.dtr = true, .rts = false};
    auto restored = ControlSignalState::from_uint16(state.to_uint16());
    EXPECT_TRUE(restored.dtr);
    EXPECT_FALSE(restored.rts);

    state = {.dtr = false, .rts = true};
    restored = ControlSignalState::from_uint16(state.to_uint16());
    EXPECT_FALSE(restored.dtr);
    EXPECT_TRUE(restored.rts);

    state = {.dtr = true, .rts = true};
    restored = ControlSignalState::from_uint16(state.to_uint16());
    EXPECT_TRUE(restored.dtr);
    EXPECT_TRUE(restored.rts);
}

// ========== CDC ACM 类特定描述符 ==========

TEST(VirtualDeviceHandlers, CdcClassSpecificDescriptor) {
    StringPool pool;
    UsbInterface intf{.interface_class = 2, .interface_subclass = 2, .interface_protocol = 1};
    CdcAcmCommunicationInterfaceHandler handler(intf, pool);

    auto desc = handler.get_class_specific_descriptor();
    // Header(5) + CallManagement(5) + ACM(4) + Union(5) = 19 字节
    ASSERT_EQ(desc.size(), 19u);

    // Header: bLength=5, bDescriptorType=0x24(CS_INTERFACE), bDescriptorSubtype=0x00, bcdCDC=1.10 小端
    EXPECT_EQ(desc[0], 0x05);
    EXPECT_EQ(desc[1], 0x24);
    EXPECT_EQ(desc[2], 0x00);
    EXPECT_EQ(desc[3], 0x10);
    EXPECT_EQ(desc[4], 0x01);

    // CallManagement: subtype=0x01, bmCapabilities=0, bDataInterface=1
    EXPECT_EQ(desc[5], 0x05);
    EXPECT_EQ(desc[7], 0x01);
    EXPECT_EQ(desc[8], 0x00);
    EXPECT_EQ(desc[9], 0x01);

    // ACM: bLength=4, subtype=0x02, bmCapabilities=0x02
    EXPECT_EQ(desc[10], 0x04);
    EXPECT_EQ(desc[12], 0x02);
    EXPECT_EQ(desc[13], 0x02);

    // Union: bLength=5, subtype=0x06, bMasterInterface=0, bSlaveInterface=1
    EXPECT_EQ(desc[14], 0x05);
    EXPECT_EQ(desc[16], 0x06);
    EXPECT_EQ(desc[17], 0x00);
    EXPECT_EQ(desc[18], 0x01);
}

TEST(VirtualDeviceHandlers, CdcDataInterfaceHasNoClassDescriptor) {
    StringPool pool;
    UsbInterface intf{.interface_class = 0x0A, .interface_subclass = 0, .interface_protocol = 0};
    CdcAcmDataInterfaceHandler handler(intf, pool);

    EXPECT_TRUE(handler.get_class_specific_descriptor().empty());
}

// ========== CDC 串口状态通知 ==========

TEST(VirtualDeviceHandlers, CdcSerialStateNotificationFormat) {
    SerialStateNotification notif;
    notif.wIndex = 1;
    notif.data = 0x0300; // DCD | DSR

    auto bytes = notif.to_bytes();
    ASSERT_EQ(bytes.size(), 10u);
    EXPECT_EQ(bytes[0], 0xA1); // bmRequestType
    EXPECT_EQ(bytes[1], 0x20); // bNotification: SERIAL_STATE
    EXPECT_EQ(bytes[4], 1);    // wIndex 小端
    EXPECT_EQ(bytes[8], 0x00); // data 小端
    EXPECT_EQ(bytes[9], 0x03);
}
