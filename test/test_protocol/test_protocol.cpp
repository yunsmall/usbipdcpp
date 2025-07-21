#include <gtest/gtest.h>

#include "protocol.h"
#include "utils.h"

#include <thread>

#include <spdlog/spdlog.h>

using namespace usbipcpp;
using namespace usbipcpp::test;

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
    for (auto [i,byte]: as_byte | std::views::enumerate) {
        ASSERT_EQ(byte, target_data[i]);
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
            .number_of_packets = 0x4321,
            .interval = 0x1111,
            .setup = {1, 2, 3, 4, 5, 6, 7, 8},
            .data = data_type(transfer_buffer_length, 0),
            .iso_packet_descriptor = {}
    };
    auto received = reread_from_socket_with_command<UsbIpCommand::UsbIpCmdSubmit>(origin, USBIP_CMD_SUBMIT);


    ASSERT_TRUE(received==origin);
}
