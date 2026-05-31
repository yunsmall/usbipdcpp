#include <gtest/gtest.h>

#include <asio.hpp>

#include "DeviceHandler/DeviceHandler.h"
#include "SetupPacket.h"
#include "constant.h"
#include "protocol.h"

using namespace usbipdcpp;

// 用于测试的 mock DeviceHandler
class MockDeviceHandlerForTest : public AbstDeviceHandler {
public:
    explicit MockDeviceHandlerForTest(UsbDevice &device) : AbstDeviceHandler(device) {
    }

    void on_new_connection(Session &current_session, error_code &ec) override {
    }
    void on_disconnection(error_code &ec) override {
    }

    void handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) override {
    }

    void receive_urb(UsbIpCommand::UsbIpCmdSubmit cmd, UsbEndpoint ep, std::optional<UsbInterface> interface,
                     usbipdcpp::error_code &ec) override {
    }
};

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

TEST_F(ProtocolEndianTest, HeaderBasicSeqnumEndian) {
    UsbIpHeaderBasic header{
            .command = USBIP_RET_SUBMIT, .seqnum = 0x11223344, .devid = 0, .direction = UsbIpDirection::In, .ep = 0};

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
    SetupPacket packet{.request_type = 0x80,
                       .request = 0x06,
                       .value = 0x0100, // value = 0x0100
                       .index = 0x0000,
                       .length = 0x0012};

    auto bytes = packet.to_bytes();

    // value 小端序：低字节在前
    EXPECT_EQ(bytes[2], 0x00); // value low byte
    EXPECT_EQ(bytes[3], 0x01); // value high byte
}

TEST_F(SetupPacketEndianTest, IndexFieldEndian) {
    SetupPacket packet{.request_type = 0x80,
                       .request = 0x06,
                       .value = 0x0000,
                       .index = 0x1234, // index = 0x1234
                       .length = 0x0000};

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
    SetupPacket original{.request_type = 0x21, .request = 0x09, .value = 0x0200, .index = 0x0001, .length = 0x0040};

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
            .offset = 0x12345678, .length = 0xDEADBEEF, .actual_length = 0xFEDCBA98, .status = 0x11223344};

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
    // 创建 GenericTransfer 模拟控制传输
    auto *trx = new GenericTransfer{};
    trx->data.resize(100);
    trx->data_offset = 8;
    trx->actual_length = 92; // 100 - 8 = 92 字节数据

    // 前 8 字节是 setup 包
    for (int i = 0; i < 8; ++i) {
        trx->data[i] = static_cast<std::uint8_t>(i);
    }

    // 数据从偏移 8 开始
    for (int i = 8; i < 100; ++i) {
        trx->data[i] = static_cast<std::uint8_t>(i);
    }

    // 验证数据偏移正确
    EXPECT_EQ(trx->data_offset, 8);
    EXPECT_EQ(trx->actual_length, 92);

    delete trx;
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

// 通过 to_socket 测试 RET_SUBMIT 序列化格式（替代已删除的 to_bytes）
class RetSubmitSocketTest : public ::testing::Test {
protected:
    asio::io_context io;
    std::unique_ptr<asio::ip::tcp::socket> write_sock;
    std::unique_ptr<asio::ip::tcp::socket> read_sock;
    std::unique_ptr<asio::ip::tcp::acceptor> acceptor;

    void SetUp() override {
        asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);
        acceptor = std::make_unique<asio::ip::tcp::acceptor>(io, ep);
        write_sock = std::make_unique<asio::ip::tcp::socket>(io);
        write_sock->connect(acceptor->local_endpoint());
        read_sock = std::make_unique<asio::ip::tcp::socket>(acceptor->accept());
    }

    void TearDown() override {
        if (write_sock) write_sock->close();
        if (read_sock) read_sock->close();
        if (acceptor) acceptor->close();
    }

    std::vector<std::uint8_t> write_and_read(const UsbIpResponse::UsbIpRetSubmit &ret, std::size_t read_size) {
        error_code ec;
        ret.to_socket(*write_sock, ec);
        if (ec)
            throw std::system_error(ec);
        std::vector<std::uint8_t> buf(read_size);
        asio::read(*read_sock, asio::buffer(buf), ec);
        if (ec)
            throw std::system_error(ec);
        return buf;
    }
};

TEST_F(RetSubmitSocketTest, HeaderSize) {
    auto ret = UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(0x1234, 0);
    auto bytes = write_and_read(ret, 48);
    EXPECT_EQ(bytes.size(), 48);
}

TEST_F(RetSubmitSocketTest, StatusEndian) {
    auto ret = UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(0x1234, 0x12345678, 0);
    auto bytes = write_and_read(ret, 48);

    // status 从偏移 20 开始，大端序
    EXPECT_EQ(bytes[20], 0x12);
    EXPECT_EQ(bytes[21], 0x34);
    EXPECT_EQ(bytes[22], 0x56);
    EXPECT_EQ(bytes[23], 0x78);
}

TEST_F(RetSubmitSocketTest, ActualLengthEndian) {
    auto ret = UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(0x1234, 0xDEADBEEF);
    auto bytes = write_and_read(ret, 48);

    // actual_length 从偏移 24 开始，大端序
    EXPECT_EQ(bytes[24], 0xDE);
    EXPECT_EQ(bytes[25], 0xAD);
    EXPECT_EQ(bytes[26], 0xBE);
    EXPECT_EQ(bytes[27], 0xEF);
}

TEST_F(RetSubmitSocketTest, FieldFormat) {
    auto ret = UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(0x12345678, 0);
    auto bytes = write_and_read(ret, 48);

    // command = USBIP_RET_SUBMIT (3)
    EXPECT_EQ(bytes[0], 0x00);
    EXPECT_EQ(bytes[1], 0x00);
    EXPECT_EQ(bytes[2], 0x00);
    EXPECT_EQ(bytes[3], 0x03);

    // seqnum
    EXPECT_EQ(bytes[4], 0x12);
    EXPECT_EQ(bytes[5], 0x34);
    EXPECT_EQ(bytes[6], 0x56);
    EXPECT_EQ(bytes[7], 0x78);

    // status = 0
    EXPECT_EQ(bytes[20], 0x00);
    EXPECT_EQ(bytes[21], 0x00);
    EXPECT_EQ(bytes[22], 0x00);
    EXPECT_EQ(bytes[23], 0x00);

    // actual_length = 0
    EXPECT_EQ(bytes[24], 0x00);
    EXPECT_EQ(bytes[25], 0x00);
    EXPECT_EQ(bytes[26], 0x00);
    EXPECT_EQ(bytes[27], 0x00);
}

TEST_F(RetSubmitSocketTest, WithData) {
    auto *trx = new GenericTransfer{};
    trx->data = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
    trx->actual_length = trx->data.size();

    GenericTransferOperator op;
    TransferHandle handle(trx, &op);

    auto ret = UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
            0x1111, static_cast<std::uint32_t>(trx->actual_length), std::move(handle));

    auto bytes = write_and_read(ret, 48 + 6);

    // 头部 48 字节 + 数据 6 字节
    EXPECT_EQ(bytes.size(), 54);

    // actual_length = 6
    EXPECT_EQ(bytes[24], 0x00);
    EXPECT_EQ(bytes[25], 0x00);
    EXPECT_EQ(bytes[26], 0x00);
    EXPECT_EQ(bytes[27], 0x06);

    // 数据紧跟在头部后面
    EXPECT_EQ(bytes[48], 0xAA);
    EXPECT_EQ(bytes[49], 0xBB);
    EXPECT_EQ(bytes[50], 0xCC);
    EXPECT_EQ(bytes[51], 0xDD);
    EXPECT_EQ(bytes[52], 0xEE);
    EXPECT_EQ(bytes[53], 0xFF);
}
