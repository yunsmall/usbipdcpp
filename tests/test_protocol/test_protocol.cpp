#include <gtest/gtest.h>

#include "protocol.h"
#include "test_utils.h"

#include <thread>

#include <spdlog/spdlog.h>

using namespace usbipdcpp;
using namespace usbipdcpp::test;

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
    ASSERT_TRUE(received_header==origin_header);
}

TEST(TestProtocol, UsbIpCmdSubmitReadSocket) {
    std::uint32_t transfer_buffer_length = 100;
    UsbIpCommand::UsbIpCmdSubmit origin{
            .header = UsbIpHeaderBasic{
                    .command = USBIP_CMD_SUBMIT,
                    .seqnum = 0x1234,
                    .devid = 0x5678,
                    .direction = UsbIpDirection::Out,
                    .ep = 0x80
            },
            .transfer_flags = 0x1234,
            .transfer_buffer_length = transfer_buffer_length,
            .start_frame = 0x8765,
            .number_of_packets = 0,
            .interval = 0x1111,
            .setup = SetupPacket::parse({1, 2, 3, 4, 5, 6, 7, 8}),
            .data = data_type(transfer_buffer_length, 0),
            .iso_packet_descriptor = {}
    };
    auto received = reread_from_socket_with_command<UsbIpCommand::UsbIpCmdSubmit>(origin, USBIP_CMD_SUBMIT);


    ASSERT_TRUE(received==origin);
}

TEST(TestProtocol, UsbIpCmdSubmitISOReadSocket) {
    std::uint32_t transfer_buffer_length = 100;
    data_type data(transfer_buffer_length, 0);
    for (std::size_t i = 0; i < transfer_buffer_length; i++) {
        data[i] = i;
    }
    std::vector<UsbIpIsoPacketDescriptor> iso_packet_descriptors{
            UsbIpIsoPacketDescriptor{
                    .offset = 0,
                    .length = 25,
                    .actual_length = 10,
                    .status = 0
            },
            UsbIpIsoPacketDescriptor{
                    .offset = 25,
                    .length = transfer_buffer_length - 25,
                    .actual_length = 15,
                    .status = 0
            }
    };
    UsbIpCommand::UsbIpCmdSubmit origin{
            .header = UsbIpHeaderBasic{
                    .command = USBIP_CMD_SUBMIT,
                    .seqnum = 0x1234,
                    .devid = 0x5678,
                    .direction = UsbIpDirection::Out,
                    .ep = 0x80
            },
            .transfer_flags = 0x1234,
            .transfer_buffer_length = transfer_buffer_length,
            .start_frame = 0x8765,
            .number_of_packets = static_cast<std::uint32_t>(iso_packet_descriptors.size()),
            .interval = 0x1111,
            .setup = SetupPacket::parse({1, 2, 3, 4, 5, 6, 7, 8}),
            .data = data,
            .iso_packet_descriptor = iso_packet_descriptors
    };
    auto received = reread_from_socket_with_command<UsbIpCommand::UsbIpCmdSubmit>(origin, USBIP_CMD_SUBMIT);


    ASSERT_TRUE(received==origin);
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

    ASSERT_TRUE(received==origin);
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
    EXPECT_EQ(received, original);
}

TEST(TestProtocol, UsbIpCmdSubmitEmptyData) {
    UsbIpCommand::UsbIpCmdSubmit origin{
            .header = UsbIpHeaderBasic{
                    .command = USBIP_CMD_SUBMIT,
                    .seqnum = 0x1234,
                    .devid = 0x5678,
                    .direction = UsbIpDirection::In,
                    .ep = 0x00
            },
            .transfer_flags = 0,
            .transfer_buffer_length = 0,
            .start_frame = 0,
            .number_of_packets = 0,
            .interval = 0,
            .setup = SetupPacket{.request_type = 0x80, .request = 0x06, .value = 0, .index = 0, .length = 0},
            .data = {},
            .iso_packet_descriptor = {}
    };
    auto received = reread_from_socket_with_command<UsbIpCommand::UsbIpCmdSubmit>(origin, USBIP_CMD_SUBMIT);
    EXPECT_TRUE(received == origin);
}

TEST(TestProtocol, UsbIpCmdSubmitLargeData) {
    // 大数据量测试
    std::uint32_t transfer_buffer_length = 65536;
    data_type data(transfer_buffer_length, 0xAB);

    UsbIpCommand::UsbIpCmdSubmit origin{
            .header = UsbIpHeaderBasic{
                    .command = USBIP_CMD_SUBMIT,
                    .seqnum = 0x1234,
                    .devid = 0x5678,
                    .direction = UsbIpDirection::Out,
                    .ep = 0x02
            },
            .transfer_flags = 0,
            .transfer_buffer_length = transfer_buffer_length,
            .start_frame = 0,
            .number_of_packets = 0,
            .interval = 0,
            .setup = SetupPacket{.request_type = 0x00, .request = 0x00, .value = 0, .index = 0, .length = 0},
            .data = data,
            .iso_packet_descriptor = {}
    };
    auto received = reread_from_socket_with_command<UsbIpCommand::UsbIpCmdSubmit>(origin, USBIP_CMD_SUBMIT);
    EXPECT_TRUE(received == origin);
}

TEST(TestProtocol, UsbIpCmdSubmitManyIsoDescriptors) {
    // 多个等时包描述符
    std::uint32_t transfer_buffer_length = 10240;
    data_type data(transfer_buffer_length, 0xCD);

    std::vector<UsbIpIsoPacketDescriptor> iso_packet_descriptors;
    for (int i = 0; i < 10; ++i) {
        iso_packet_descriptors.push_back(UsbIpIsoPacketDescriptor{
                .offset = static_cast<std::uint32_t>(i * 1024),
                .length = 1024,
                .actual_length = 1024,
                .status = 0
        });
    }

    UsbIpCommand::UsbIpCmdSubmit origin{
            .header = UsbIpHeaderBasic{
                    .command = USBIP_CMD_SUBMIT,
                    .seqnum = 0x1234,
                    .devid = 0x5678,
                    .direction = UsbIpDirection::Out,
                    .ep = 0x81
            },
            .transfer_flags = 0,
            .transfer_buffer_length = transfer_buffer_length,
            .start_frame = 1000,
            .number_of_packets = static_cast<std::uint32_t>(iso_packet_descriptors.size()),
            .interval = 1,
            .setup = SetupPacket{.request_type = 0x00, .request = 0x00, .value = 0, .index = 0, .length = 0},
            .data = data,
            .iso_packet_descriptor = iso_packet_descriptors
    };
    auto received = reread_from_socket_with_command<UsbIpCommand::UsbIpCmdSubmit>(origin, USBIP_CMD_SUBMIT);
    EXPECT_TRUE(received == origin);
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
    EXPECT_TRUE(received == origin);
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
