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
