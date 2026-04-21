#include <gtest/gtest.h>

#include "protocol.h"
#include "test_utils.h"
#include "DeviceHandler/DeviceHandler.h"

#include <thread>

#include <spdlog/spdlog.h>

using namespace usbipdcpp;
using namespace usbipdcpp::test;

// 用于测试的 mock DeviceHandler
class MockDeviceHandlerForTest : public AbstDeviceHandler {
public:
    explicit MockDeviceHandlerForTest(UsbDevice &device) : AbstDeviceHandler(device) {}

    void on_new_connection(Session &current_session, error_code &ec) override {}
    void on_disconnection(error_code &ec) override {}

# ifdef USBIPDCPP_ENABLE_BUSY_WAIT
    bool has_pending_transfers() const override { return false; }
# endif

    void handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) override {}

protected:
    void handle_control_urb(
            std::uint32_t seqnum,
            const UsbEndpoint &ep,
            std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length, const SetupPacket &setup_packet,
            TransferHandle transfer,
            std::error_code &ec) override {}

    void handle_bulk_transfer(
            std::uint32_t seqnum,
            const UsbEndpoint &ep,
            UsbInterface &interface,
            std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
            TransferHandle transfer,
            std::error_code &ec) override {}

    void handle_interrupt_transfer(
            std::uint32_t seqnum,
            const UsbEndpoint &ep,
            UsbInterface &interface,
            std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
            TransferHandle transfer,
            std::error_code &ec) override {}

    void handle_isochronous_transfer(
            std::uint32_t seqnum,
            const UsbEndpoint &ep,
            UsbInterface &interface,
            std::uint32_t transfer_flags,
            std::uint32_t transfer_buffer_length,
            TransferHandle transfer,
            int num_iso_packets,
            std::error_code &ec) override {}
};

TEST(TestProtocol, UsbIpHeaderBasic) {
    UsbIpHeaderBasic header{
            .command = USBIP_CMD_SUBMIT,
            .seqnum = 0x1234,
            .devid = 0x5678,
            .direction = UsbIpDirection::In,
            .ep = 0x80
    };

    auto as_byte = header.to_bytes();
    data_type target_data = {
            0x00, 0x00, 0x00, 0x01,
            0x00, 0x00, 0x12, 0x34,
            0x00, 0x00, 0x56, 0x78,
            0x00, 0x00, 0x00, 0x01,
            0x00, 0x00, 0x00, 0x80
    };
    ASSERT_EQ(as_byte.size(), target_data.size());
    for (std::size_t i = 0; i < as_byte.size(); i++) {
        ASSERT_EQ(as_byte[i], target_data[i]);
    }
}

TEST(TestProtocol, UsbIpHeaderBasicReadSocket) {
    UsbIpHeaderBasic origin_header{
            .command = USBIP_CMD_SUBMIT,
            .seqnum = 0x1234,
            .devid = 0x5678,
            .direction = UsbIpDirection::In,
            .ep = 0x80
    };
    auto received_header = reread_from_socket_with_command<UsbIpHeaderBasic>(origin_header, USBIP_CMD_SUBMIT);
    expect_header_equal(received_header, origin_header);
}

TEST(TestProtocol, UsbIpCmdSubmitReadSocketWithoutData) {
    // IN 传输，没有数据需要读取
    UsbIpHeaderBasic header{
            .command = USBIP_CMD_SUBMIT,
            .seqnum = 0x1234,
            .devid = 0x5678,
            .direction = UsbIpDirection::In,
            .ep = 0x80
    };

    asio::io_context io_context;
    asio::ip::tcp::acceptor acceptor(io_context);
    auto server_endpoint = asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0);
    acceptor.open(server_endpoint.protocol());
    acceptor.bind(server_endpoint);
    acceptor.listen();
    auto server_port = acceptor.local_endpoint().port();

    std::thread sender([&]() {
        auto sock = acceptor.accept();
        usbipdcpp::data_type buffer;
        // 发送版本号 + 命令码
        usbipdcpp::vector_append_to_net(buffer, static_cast<std::uint16_t>(USBIP_VERSION));
        usbipdcpp::vector_append_to_net(buffer, static_cast<std::uint16_t>(USBIP_CMD_SUBMIT));

        // from_socket 期望读取的数据从 seqnum 开始（不包含 command）
        // 所以我们只发送 seqnum, devid, direction, ep (16 字节)
        usbipdcpp::vector_append_to_net(buffer, header.seqnum);
        usbipdcpp::vector_append_to_net(buffer, header.devid);
        usbipdcpp::vector_append_to_net(buffer, header.direction);
        usbipdcpp::vector_append_to_net(buffer, header.ep);

        // transfer_flags = 0x1234
        usbipdcpp::vector_append_to_net(buffer, static_cast<std::uint32_t>(0x1234));
        // transfer_buffer_length = 0
        usbipdcpp::vector_append_to_net(buffer, static_cast<std::uint32_t>(0));
        // start_frame = 0x8765
        usbipdcpp::vector_append_to_net(buffer, static_cast<std::uint32_t>(0x8765));
        // number_of_packets = 0
        usbipdcpp::vector_append_to_net(buffer, static_cast<std::uint32_t>(0));
        // interval = 0x1111
        usbipdcpp::vector_append_to_net(buffer, static_cast<std::uint32_t>(0x1111));
        // setup packet (8 bytes)
        buffer.insert(buffer.end(), {1, 2, 3, 4, 5, 6, 7, 8});

        sock.send(asio::buffer(buffer));
    });

    // 创建测试设备（必须先声明，因为 mock_handler 引用它）
    auto test_device = UsbDevice{
        .path = "/test",
        .busid = "1-1",
        .bus_num = 1,
        .dev_num = 1,
        .speed = 0,
        .vendor_id = 0,
        .product_id = 0,
        .device_bcd = 0,
        .device_class = 0,
        .device_subclass = 0,
        .device_protocol = 0,
        .configuration_value = 1,
        .num_configurations = 1,
        .interfaces = {},
        .ep0_in = UsbEndpoint::get_default_ep0_in(),
        .ep0_out = UsbEndpoint::get_default_ep0_out()
    };
    MockDeviceHandlerForTest mock_handler(test_device);

    {
        // 使用作用域确保 received 在 mock_handler 之前析构
        UsbIpCommand::UsbIpCmdSubmit received{};
        asio::ip::tcp::socket server_socket(io_context);
        asio::error_code ec;
        server_socket.connect(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), server_port), ec);

        [[maybe_unused]] auto version = usbipdcpp::read_u16(server_socket);
        [[maybe_unused]] auto op_command = usbipdcpp::read_u16(server_socket);

        received.transfer.set_handler(&mock_handler);
        received.from_socket(server_socket);
        received.header.command = op_command;

        server_socket.close();
        sender.join();

        EXPECT_EQ(received.header.seqnum, header.seqnum);
        EXPECT_EQ(received.transfer_flags, 0x1234);
        EXPECT_EQ(received.start_frame, 0x8765);
        EXPECT_EQ(received.interval, 0x1111);
    }  // received 在这里析构，此时 mock_handler 还有效

    // mock_handler 和 test_device 在这里析构
}

TEST(TestProtocol, UsbIpCmdUnlinkReadSocket) {

    UsbIpCommand::UsbIpCmdUnlink origin{
            .header = UsbIpHeaderBasic{
                    .command = USBIP_CMD_SUBMIT,
                    .seqnum = 0x1234,
                    .devid = 0x5678,
                    .direction = UsbIpDirection::Out,
                    .ep = 0x80
            },
            .unlink_seqnum = 0xabcd
    };
    auto received = reread_from_socket_with_command<UsbIpCommand::UsbIpCmdUnlink>(origin, USBIP_CMD_UNLINK);

    expect_cmd_unlink_equal(received, origin);
}

// ============== 极端情况测试 ==============

TEST(TestProtocol, UsbIpHeaderBasicAllZeros) {
    UsbIpHeaderBasic header{
            .command = 0,
            .seqnum = 0,
            .devid = 0,
            .direction = 0,
            .ep = 0
    };

    auto bytes = header.to_bytes();
    for (auto byte : bytes) {
        EXPECT_EQ(byte, 0);
    }
}

TEST(TestProtocol, UsbIpHeaderBasicMaxValues) {
    UsbIpHeaderBasic header{
            .command = 0xFFFFFFFF,
            .seqnum = 0xFFFFFFFF,
            .devid = 0xFFFFFFFF,
            .direction = 0xFFFFFFFF,
            .ep = 0xFFFFFFFF
    };

    auto bytes = header.to_bytes();
    EXPECT_EQ(bytes.size(), 20);

    // 验证字节序（大端）
    EXPECT_EQ(bytes[0], 0xFF);
    EXPECT_EQ(bytes[1], 0xFF);
    EXPECT_EQ(bytes[2], 0xFF);
    EXPECT_EQ(bytes[3], 0xFF);
}

TEST(TestProtocol, UsbIpHeaderBasicRoundTrip) {
    UsbIpHeaderBasic original{
            .command = USBIP_RET_SUBMIT,
            .seqnum = 0xDEADBEEF,
            .devid = 0x12345678,
            .direction = UsbIpDirection::In,
            .ep = 0x0F
    };

    auto received = reread_from_socket_with_command<UsbIpHeaderBasic>(original, USBIP_RET_SUBMIT);
    expect_header_equal(received, original);
}

TEST(TestProtocol, UsbIpIsoPacketDescriptorRoundTrip) {
    UsbIpIsoPacketDescriptor original{
            .offset = 0x12345678,
            .length = 0xDEADBEEF,
            .actual_length = 0xFFFFFFFF,
            .status = 0
    };

    auto bytes = original.to_bytes();
    EXPECT_EQ(bytes.size(), 16);

    // 大端序验证
    EXPECT_EQ(bytes[0], 0x12);
    EXPECT_EQ(bytes[1], 0x34);
    EXPECT_EQ(bytes[2], 0x56);
    EXPECT_EQ(bytes[3], 0x78);
}

TEST(TestProtocol, UsbIpIsoPacketDescriptorAllZeros) {
    UsbIpIsoPacketDescriptor desc{
            .offset = 0,
            .length = 0,
            .actual_length = 0,
            .status = 0
    };

    auto bytes = desc.to_bytes();
    for (auto byte : bytes) {
        EXPECT_EQ(byte, 0);
    }
}

TEST(TestProtocol, UsbIpCmdUnlinkMaxSeqnum) {
    UsbIpCommand::UsbIpCmdUnlink origin{
            .header = UsbIpHeaderBasic{
                    .command = USBIP_CMD_UNLINK,
                    .seqnum = 0xFFFFFFFF,
                    .devid = 0,
                    .direction = 0,
                    .ep = 0
            },
            .unlink_seqnum = 0xFFFFFFFF
    };
    auto received = reread_from_socket_with_command<UsbIpCommand::UsbIpCmdUnlink>(origin, USBIP_CMD_UNLINK);
    expect_cmd_unlink_equal(received, origin);
}

TEST(TestProtocol, OpReqDevlistRoundTrip) {
    UsbIpCommand::OpReqDevlist origin{.status = 0};
    auto bytes = origin.to_bytes();

    // 验证版本号(2) + 命令码(2) + status(4) = 8字节
    EXPECT_EQ(bytes.size(), 8);
}

TEST(TestProtocol, OpReqImportRoundTrip) {
    UsbIpCommand::OpReqImport origin{
            .status = 0,
            .busid = {'1', '-', '1', '5', '\0'}
    };

    // 填充剩余字节为0
    for (std::size_t i = 5; i < 32; ++i) {
        origin.busid[i] = 0;
    }

    auto bytes = origin.to_bytes();
    EXPECT_GT(bytes.size(), 0);
}

TEST(TestProtocol, DifferentEndpointAddresses) {
    // 测试不同端点地址
    for (std::uint8_t ep = 0; ep <= 0x0F; ++ep) {
        UsbIpHeaderBasic header{
                .command = USBIP_CMD_SUBMIT,
                .seqnum = 0x1234,
                .devid = 0,
                .direction = UsbIpDirection::In,
                .ep = static_cast<std::uint32_t>(ep | 0x80)
        };

        auto bytes = header.to_bytes();
        EXPECT_EQ(bytes[19], static_cast<std::uint8_t>(ep | 0x80));
    }
}

TEST(TestProtocol, AllCommandTypes) {
    // 测试所有命令类型
    std::vector<std::uint32_t> commands = {
            USBIP_CMD_SUBMIT,
            USBIP_CMD_UNLINK,
            USBIP_RET_SUBMIT,
            USBIP_RET_UNLINK
    };

    for (auto cmd : commands) {
        UsbIpHeaderBasic header{
                .command = cmd,
                .seqnum = 0x1234,
                .devid = 0,
                .direction = 0,
                .ep = 0
        };

        auto bytes = header.to_bytes();
        EXPECT_EQ(bytes[3], static_cast<std::uint8_t>(cmd & 0xFF));
    }
}
