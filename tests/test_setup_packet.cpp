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

// ============== 极端情况测试 ==============

TEST(TestSetupPacket, AllZeros) {
    std::array<std::uint8_t, 8> data = {0, 0, 0, 0, 0, 0, 0, 0};
    SetupPacket packet = SetupPacket::parse(data);

    EXPECT_EQ(packet.request_type, 0);
    EXPECT_EQ(packet.request, 0);
    EXPECT_EQ(packet.value, 0);
    EXPECT_EQ(packet.index, 0);
    EXPECT_EQ(packet.length, 0);
}

TEST(TestSetupPacket, AllOnes) {
    std::array<std::uint8_t, 8> data = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    SetupPacket packet = SetupPacket::parse(data);

    EXPECT_EQ(packet.request_type, 0xFF);
    EXPECT_EQ(packet.request, 0xFF);
    EXPECT_EQ(packet.value, 0xFFFF);
    EXPECT_EQ(packet.index, 0xFFFF);
    EXPECT_EQ(packet.length, 0xFFFF);
}

TEST(TestSetupPacket, MaxValues) {
    SetupPacket packet{
            .request_type = 0xFF,
            .request = 0xFF,
            .value = 0xFFFF,
            .index = 0xFFFF,
            .length = 0xFFFF
    };

    auto bytes = packet.to_bytes();
    auto parsed = SetupPacket::parse(bytes);

    EXPECT_EQ(packet, parsed);
}

TEST(TestSetupPacket, RoundTripAllDirections) {
    // 测试所有方向组合
    for (int dir = 0; dir <= 0x80; dir += 0x80) {
        SetupPacket original{
                .request_type = static_cast<std::uint8_t>(dir | 0x01), // Device/Interface
                .request = 0x06,
                .value = 0x0100,
                .index = 0x0000,
                .length = 0x0012
        };

        auto bytes = original.to_bytes();
        auto parsed = SetupPacket::parse(bytes);
        EXPECT_EQ(original, parsed);
    }
}

TEST(TestSetupPacket, IsIn) {
    SetupPacket in_packet{
            .request_type = 0x80,
            .request = 0x06,
            .value = 0,
            .index = 0,
            .length = 64
    };
    EXPECT_TRUE(in_packet.is_in());

    SetupPacket out_packet{
            .request_type = 0x00,
            .request = 0x09,
            .value = 0,
            .index = 0,
            .length = 0
    };
    EXPECT_FALSE(out_packet.is_in());
}

TEST(TestSetupPacket, DifferentRequestTypes) {
    // Device-to-host, Standard, Device (IN direction with non-zero length)
    SetupPacket std_device{.request_type = 0x80, .request = 0x06, .value = 0, .index = 0, .length = 64};
    EXPECT_TRUE(std_device.is_in());

    // Device-to-host, Class, Interface (IN direction with non-zero length)
    SetupPacket class_interface{.request_type = 0xA1, .request = 0x01, .value = 0, .index = 0, .length = 64};
    EXPECT_TRUE(class_interface.is_in());

    // Host-to-device, Vendor, Endpoint (OUT direction)
    SetupPacket vendor_endpoint{.request_type = 0x42, .request = 0xFF, .value = 0, .index = 0, .length = 0};
    EXPECT_FALSE(vendor_endpoint.is_in());
}
