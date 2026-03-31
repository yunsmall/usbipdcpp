#include <gtest/gtest.h>

#include "SetupPacket.h"
#include "constant.h"

using namespace usbipdcpp;

TEST(TestSetupPacket, Parse) {
    std::array<std::uint8_t, 8> data = {0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x12, 0x00};

    SetupPacket packet = SetupPacket::parse(data);

    EXPECT_EQ(packet.request_type, 0x80);
    EXPECT_EQ(packet.request, 0x06);
    EXPECT_EQ(packet.value, 0x0100);
    EXPECT_EQ(packet.index, 0x0000);
    EXPECT_EQ(packet.length, 0x0012);
}

TEST(TestSetupPacket, ToBytes) {
    SetupPacket packet{
            .request_type = 0x80,
            .request = 0x06,
            .value = 0x0100,
            .index = 0x0000,
            .length = 0x0012
    };

    auto bytes = packet.to_bytes();

    EXPECT_EQ(bytes[0], 0x80);
    EXPECT_EQ(bytes[1], 0x06);
    EXPECT_EQ(bytes[2], 0x00); // value low byte
    EXPECT_EQ(bytes[3], 0x01); // value high byte
    EXPECT_EQ(bytes[4], 0x00); // index low byte
    EXPECT_EQ(bytes[5], 0x00); // index high byte
    EXPECT_EQ(bytes[6], 0x12); // length low byte
    EXPECT_EQ(bytes[7], 0x00); // length high byte
}

TEST(TestSetupPacket, RoundTrip) {
    SetupPacket original{
            .request_type = 0x21,
            .request = 0x09,
            .value = 0x0200,
            .index = 0x0001,
            .length = 0x0040
    };

    auto bytes = original.to_bytes();
    auto parsed = SetupPacket::parse(bytes);

    EXPECT_EQ(original, parsed);
}

TEST(TestSetupPacket, IsClearHaltCmd) {
    // ClearFeature to endpoint with value 0
    SetupPacket clear_halt{
            .request_type = 0x02, // Endpoint recipient
            .request = static_cast<std::uint8_t>(StandardRequest::ClearFeature),
            .value = 0,
            .index = 0x81,
            .length = 0
    };
    EXPECT_TRUE(clear_halt.is_clear_halt_cmd());

    // Not a clear halt (wrong value)
    SetupPacket not_clear_halt{
            .request_type = 0x02,
            .request = static_cast<std::uint8_t>(StandardRequest::ClearFeature),
            .value = 1,
            .index = 0x81,
            .length = 0
    };
    EXPECT_FALSE(not_clear_halt.is_clear_halt_cmd());
}

TEST(TestSetupPacket, IsSetInterfaceCmd) {
    SetupPacket set_interface{
            .request_type = 0x01, // Interface recipient
            .request = static_cast<std::uint8_t>(StandardRequest::SetInterface),
            .value = 0,
            .index = 0,
            .length = 0
    };
    EXPECT_TRUE(set_interface.is_set_interface_cmd());

    SetupPacket not_set_interface{
            .request_type = 0x00, // Device recipient
            .request = static_cast<std::uint8_t>(StandardRequest::SetInterface),
            .value = 0,
            .index = 0,
            .length = 0
    };
    EXPECT_FALSE(not_set_interface.is_set_interface_cmd());
}

TEST(TestSetupPacket, IsSetConfigurationCmd) {
    SetupPacket set_config{
            .request_type = 0x00, // Device recipient
            .request = static_cast<std::uint8_t>(StandardRequest::SetConfiguration),
            .value = 1,
            .index = 0,
            .length = 0
    };
    EXPECT_TRUE(set_config.is_set_configuration_cmd());

    SetupPacket not_set_config{
            .request_type = 0x01, // Interface recipient
            .request = static_cast<std::uint8_t>(StandardRequest::SetConfiguration),
            .value = 1,
            .index = 0,
            .length = 0
    };
    EXPECT_FALSE(not_set_config.is_set_configuration_cmd());
}

TEST(TestSetupPacket, IsOut) {
    SetupPacket out_packet{
            .request_type = 0x00, // Direction OUT
            .request = 0x09,
            .value = 0,
            .index = 0,
            .length = 0
    };
    EXPECT_TRUE(out_packet.is_out());

    SetupPacket in_packet{
            .request_type = 0x80, // Direction IN
            .request = 0x06,
            .value = 0,
            .index = 0,
            .length = 64
    };
    EXPECT_FALSE(in_packet.is_out());
}
