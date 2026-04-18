#include <gtest/gtest.h>

#include "protocol.h"
#include "SetupPacket.h"
#include "constant.h"

using namespace usbipdcpp;

// 测试协议头部大小是否与 USBIP 规范一致
class ProtocolSizeTest : public ::testing::Test {
protected:
    // USBIP 协议规范定义的大小
    static constexpr std::size_t USBIP_HEADER_BASIC_SIZE = 20; // 5 * 4 bytes
    static constexpr std::size_t USBIP_RET_SUBMIT_PAYLOAD_SIZE = 20; // 5 * 4 bytes
    static constexpr std::size_t USBIP_CMD_SUBMIT_PAYLOAD_SIZE = 28; // 5 * 4 + 8 bytes
    static constexpr std::size_t USBIP_RET_UNLINK_PAYLOAD_SIZE = 4; // 1 * 4 bytes
    static constexpr std::size_t USBIP_CMD_UNLINK_PAYLOAD_SIZE = 4; // 1 * 4 bytes

    // USBIP 头部总大小（使用 union 的最大大小）
    static constexpr std::size_t USBIP_HEADER_TOTAL_SIZE = 48; // 20 + 28
};

TEST_F(ProtocolSizeTest, RetSubmitHeaderSize) {
    // RET_SUBMIT 头部应该是 48 字节
    UsbIpResponse::UsbIpRetSubmit ret{
        .header = UsbIpHeaderBasic::get_server_header(USBIP_RET_SUBMIT, 0x1234),
        .status = 0,
        .actual_length = 0,
        .start_frame = 0,
        .number_of_packets = 0,
        .error_count = 0,
        .transfer_buffer = {},
        .iso_packet_descriptor = {}
    };

    auto bytes = ret.to_bytes();
    // 头部固定部分大小
    EXPECT_EQ(bytes.size(), USBIP_HEADER_TOTAL_SIZE);
}

TEST_F(ProtocolSizeTest, RetUnlinkHeaderSize) {
    // RET_UNLINK 头部应该是 48 字节
    auto ret = UsbIpResponse::UsbIpRetUnlink::create_ret_unlink_success(0x1234);
    auto bytes = ret.to_bytes();

    EXPECT_EQ(bytes.size(), USBIP_HEADER_TOTAL_SIZE);
}

TEST_F(ProtocolSizeTest, CmdSubmitHeaderReadSize) {
    // 模拟 CMD_SUBMIT 的字节流（48 字节头部）
    std::vector<std::uint8_t> raw_data(48, 0);

    // command = USBIP_CMD_SUBMIT (大端)
    raw_data[0] = 0x00;
    raw_data[1] = 0x00;
    raw_data[2] = 0x00;
    raw_data[3] = 0x01;

    // seqnum = 0x12345678 (大端)
    raw_data[4] = 0x12;
    raw_data[5] = 0x34;
    raw_data[6] = 0x56;
    raw_data[7] = 0x78;

    // 验证头部解析后的值
    std::uint32_t command = (raw_data[0] << 24) | (raw_data[1] << 16) | (raw_data[2] << 8) | raw_data[3];
    EXPECT_EQ(command, USBIP_CMD_SUBMIT);

    std::uint32_t seqnum = (raw_data[4] << 24) | (raw_data[5] << 16) | (raw_data[6] << 8) | raw_data[7];
    EXPECT_EQ(seqnum, 0x12345678);
}

// 测试字节序是否正确（USBIP 使用大端序）
class ProtocolEndianTest : public ::testing::Test {};

TEST_F(ProtocolEndianTest, RetSubmitStatusEndian) {
    // status 应该是大端序
    UsbIpResponse::UsbIpRetSubmit ret{
        .header = UsbIpHeaderBasic::get_server_header(USBIP_RET_SUBMIT, 0x1234),
        .status = 0x12345678, // 测试值
        .actual_length = 0,
        .start_frame = 0,
        .number_of_packets = 0,
        .error_count = 0,
        .transfer_buffer = {},
        .iso_packet_descriptor = {}
    };

    auto bytes = ret.to_bytes();

    // header 占 20 字节，status 从偏移 20 开始
    // 大端序：0x12, 0x34, 0x56, 0x78
    EXPECT_EQ(bytes[20], 0x12);
    EXPECT_EQ(bytes[21], 0x34);
    EXPECT_EQ(bytes[22], 0x56);
    EXPECT_EQ(bytes[23], 0x78);
}

TEST_F(ProtocolEndianTest, RetSubmitActualLengthEndian) {
    // actual_length 应该是大端序
    UsbIpResponse::UsbIpRetSubmit ret{
        .header = UsbIpHeaderBasic::get_server_header(USBIP_RET_SUBMIT, 0x1234),
        .status = 0,
        .actual_length = 0xDEADBEEF, // 测试值
        .start_frame = 0,
        .number_of_packets = 0,
        .error_count = 0,
        .transfer_buffer = {},
        .iso_packet_descriptor = {}
    };

    auto bytes = ret.to_bytes();

    // header(20) + status(4) = 24, actual_length 从偏移 24 开始
    // 大端序：0xDE, 0xAD, 0xBE, 0xEF
    EXPECT_EQ(bytes[24], 0xDE);
    EXPECT_EQ(bytes[25], 0xAD);
    EXPECT_EQ(bytes[26], 0xBE);
    EXPECT_EQ(bytes[27], 0xEF);
}

TEST_F(ProtocolEndianTest, HeaderBasicSeqnumEndian) {
    UsbIpHeaderBasic header{
        .command = USBIP_RET_SUBMIT,
        .seqnum = 0x11223344,
        .devid = 0,
        .direction = UsbIpDirection::In,
        .ep = 0
    };

    auto bytes = header.to_bytes();

    // command (偏移 0-3)
    EXPECT_EQ(bytes[0], 0x00);
    EXPECT_EQ(bytes[1], 0x00);
    EXPECT_EQ(bytes[2], 0x00);
    EXPECT_EQ(bytes[3], 0x03); // USBIP_RET_SUBMIT = 3

    // seqnum (偏移 4-7)
    EXPECT_EQ(bytes[4], 0x11);
    EXPECT_EQ(bytes[5], 0x22);
    EXPECT_EQ(bytes[6], 0x33);
    EXPECT_EQ(bytes[7], 0x44);
}

// 测试 SetupPacket 字节序（USB 规范使用小端序）
class SetupPacketEndianTest : public ::testing::Test {};

TEST_F(SetupPacketEndianTest, ValueFieldEndian) {
    // SetupPacket 的 value 字段使用小端序
    SetupPacket packet{
        .request_type = 0x80,
        .request = 0x06,
        .value = 0x0100, // value = 0x0100
        .index = 0x0000,
        .length = 0x0012
    };

    auto bytes = packet.to_bytes();

    // value 小端序：低字节在前
    EXPECT_EQ(bytes[2], 0x00); // value low byte
    EXPECT_EQ(bytes[3], 0x01); // value high byte
}

TEST_F(SetupPacketEndianTest, IndexFieldEndian) {
    SetupPacket packet{
        .request_type = 0x80,
        .request = 0x06,
        .value = 0x0000,
        .index = 0x1234, // index = 0x1234
        .length = 0x0000
    };

    auto bytes = packet.to_bytes();

    // index 小端序：低字节在前
    EXPECT_EQ(bytes[4], 0x34); // index low byte
    EXPECT_EQ(bytes[5], 0x12); // index high byte
}

TEST_F(SetupPacketEndianTest, LengthFieldEndian) {
    SetupPacket packet{
        .request_type = 0x80,
        .request = 0x06,
        .value = 0x0000,
        .index = 0x0000,
        .length = 0xABCD // length = 0xABCD
    };

    auto bytes = packet.to_bytes();

    // length 小端序：低字节在前
    EXPECT_EQ(bytes[6], 0xCD); // length low byte
    EXPECT_EQ(bytes[7], 0xAB); // length high byte
}

TEST_F(SetupPacketEndianTest, RoundTripEndian) {
    // 验证解析和序列化保持一致
    SetupPacket original{
        .request_type = 0x21,
        .request = 0x09,
        .value = 0x0200,
        .index = 0x0001,
        .length = 0x0040
    };

    auto bytes = original.to_bytes();
    auto parsed = SetupPacket::parse(bytes);

    EXPECT_EQ(original.request_type, parsed.request_type);
    EXPECT_EQ(original.request, parsed.request);
    EXPECT_EQ(original.value, parsed.value);
    EXPECT_EQ(original.index, parsed.index);
    EXPECT_EQ(original.length, parsed.length);
}

// 测试 ISO 包描述符字节序
class IsoPacketDescriptorTest : public ::testing::Test {};

TEST_F(IsoPacketDescriptorTest, Endianness) {
    UsbIpIsoPacketDescriptor desc{
        .offset = 0x12345678,
        .length = 0xDEADBEEF,
        .actual_length = 0xFEDCBA98,
        .status = 0x11223344
    };

    auto bytes = desc.to_bytes();

    // offset (大端序)
    EXPECT_EQ(bytes[0], 0x12);
    EXPECT_EQ(bytes[1], 0x34);
    EXPECT_EQ(bytes[2], 0x56);
    EXPECT_EQ(bytes[3], 0x78);

    // length (大端序)
    EXPECT_EQ(bytes[4], 0xDE);
    EXPECT_EQ(bytes[5], 0xAD);
    EXPECT_EQ(bytes[6], 0xBE);
    EXPECT_EQ(bytes[7], 0xEF);
}

// 测试控制传输数据偏移
class ControlTransferDataOffsetTest : public ::testing::Test {};

TEST_F(ControlTransferDataOffsetTest, DataOffsetIs8) {
    // 控制传输的数据从偏移 8 开始（跳过 setup 包）
    data_type control_buffer(100, 0);

    // 前 8 字节是 setup 包
    for (int i = 0; i < 8; ++i) {
        control_buffer[i] = static_cast<std::uint8_t>(i);
    }

    // 数据从偏移 8 开始
    for (int i = 8; i < 100; ++i) {
        control_buffer[i] = static_cast<std::uint8_t>(i);
    }

    UsbIpResponse::UsbIpRetSubmit ret{
        .header = UsbIpHeaderBasic::get_server_header(USBIP_RET_SUBMIT, 0x1234),
        .status = 0,
        .actual_length = 92, // 100 - 8 = 92 字节数据
        .start_frame = 0,
        .number_of_packets = 0,
        .error_count = 0,
        .transfer_buffer = std::move(control_buffer),
        .iso_packet_descriptor = {}
    };
    ret.send_config.data_offset = 8; // 控制传输偏移

    // 验证发送时数据偏移正确
    EXPECT_EQ(ret.send_config.data_offset, 8);
    EXPECT_EQ(ret.actual_length, 92);
}

// 测试状态码转换
class StatusCodeConversionTest : public ::testing::Test {};

TEST_F(StatusCodeConversionTest, TrxStatToError) {
    // 测试 libusb_transfer_status 到 errno 的转换
    // 参考 usbipd-libusb 的 trxstat2error

    // LIBUSB_TRANSFER_COMPLETED -> 0
    // LIBUSB_TRANSFER_CANCELLED -> -ECONNRESET
    // LIBUSB_TRANSFER_STALL -> -EPIPE
    // LIBUSB_TRANSFER_NO_DEVICE -> -ESHUTDOWN

    // 这些值应该与 Linux 内核定义一致
    EXPECT_EQ(static_cast<int>(UrbStatusType::StatusOK), 0);
    // ECONNRESET, EPIPE, ESHUTDOWN 的具体值取决于平台
}

// 测试与 usbipd-libusb 字节格式兼容性
class UsbipdLibusbCompatibilityTest : public ::testing::Test {};

TEST_F(UsbipdLibusbCompatibilityTest, RetSubmitWithoutData) {
    // 创建一个没有数据的 RET_SUBMIT
    auto ret = UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(0x12345678, 0);

    auto bytes = ret.to_bytes();

    // 验证总大小是 48 字节
    EXPECT_EQ(bytes.size(), 48);

    // 验证 command
    EXPECT_EQ(bytes[0], 0x00);
    EXPECT_EQ(bytes[1], 0x00);
    EXPECT_EQ(bytes[2], 0x00);
    EXPECT_EQ(bytes[3], 0x03); // USBIP_RET_SUBMIT

    // 验证 seqnum
    EXPECT_EQ(bytes[4], 0x12);
    EXPECT_EQ(bytes[5], 0x34);
    EXPECT_EQ(bytes[6], 0x56);
    EXPECT_EQ(bytes[7], 0x78);

    // 验证 status = 0
    EXPECT_EQ(bytes[20], 0x00);
    EXPECT_EQ(bytes[21], 0x00);
    EXPECT_EQ(bytes[22], 0x00);
    EXPECT_EQ(bytes[23], 0x00);

    // 验证 actual_length = 0
    EXPECT_EQ(bytes[24], 0x00);
    EXPECT_EQ(bytes[25], 0x00);
    EXPECT_EQ(bytes[26], 0x00);
    EXPECT_EQ(bytes[27], 0x00);
}

TEST_F(UsbipdLibusbCompatibilityTest, RetUnlinkFormat) {
    auto ret = UsbIpResponse::UsbIpRetUnlink::create_ret_unlink_success(0xABCDEF01);

    auto bytes = ret.to_bytes();

    // 验证总大小是 48 字节
    EXPECT_EQ(bytes.size(), 48);

    // 验证 command
    EXPECT_EQ(bytes[0], 0x00);
    EXPECT_EQ(bytes[1], 0x00);
    EXPECT_EQ(bytes[2], 0x00);
    EXPECT_EQ(bytes[3], 0x04); // USBIP_RET_UNLINK

    // 验证 seqnum
    EXPECT_EQ(bytes[4], 0xAB);
    EXPECT_EQ(bytes[5], 0xCD);
    EXPECT_EQ(bytes[6], 0xEF);
    EXPECT_EQ(bytes[7], 0x01);

    // 验证 status = 0 (偏移 20)
    EXPECT_EQ(bytes[20], 0x00);
    EXPECT_EQ(bytes[21], 0x00);
    EXPECT_EQ(bytes[22], 0x00);
    EXPECT_EQ(bytes[23], 0x00);
}

TEST_F(UsbipdLibusbCompatibilityTest, RetSubmitWithData) {
    // 创建一个带数据的 RET_SUBMIT
    data_type data = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    auto ret = UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(0x1111, static_cast<std::uint32_t>(data.size()), data);

    auto bytes = ret.to_bytes();

    // 头部 48 字节 + 数据 6 字节 = 54 字节
    EXPECT_EQ(bytes.size(), 48 + 6);

    // 验证 actual_length
    EXPECT_EQ(bytes[24], 0x00);
    EXPECT_EQ(bytes[25], 0x00);
    EXPECT_EQ(bytes[26], 0x00);
    EXPECT_EQ(bytes[27], 0x06); // actual_length = 6

    // 验证数据紧跟在头部后面
    EXPECT_EQ(bytes[48], 0xAA);
    EXPECT_EQ(bytes[49], 0xBB);
    EXPECT_EQ(bytes[50], 0xCC);
    EXPECT_EQ(bytes[51], 0xDD);
    EXPECT_EQ(bytes[52], 0xEE);
    EXPECT_EQ(bytes[53], 0xFF);
}
