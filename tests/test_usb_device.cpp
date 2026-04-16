#include <gtest/gtest.h>

#include "Endpoint.h"
#include "Device.h"
#include "utils/StringPool.h"

using namespace usbipdcpp;

TEST(TestUsbEndpoint, Direction) {
    UsbEndpoint ep_in{.address = 0x81, .attributes = 0x03, .max_packet_size = 8, .interval = 10};
    EXPECT_TRUE(ep_in.is_in());
    EXPECT_EQ(ep_in.direction(), UsbEndpoint::Direction::In);

    UsbEndpoint ep_out{.address = 0x01, .attributes = 0x03, .max_packet_size = 8, .interval = 10};
    EXPECT_FALSE(ep_out.is_in());
    EXPECT_EQ(ep_out.direction(), UsbEndpoint::Direction::Out);
}

TEST(TestUsbEndpoint, IsEp0) {
    UsbEndpoint ep0_in = UsbEndpoint::get_default_ep0_in();
    EXPECT_TRUE(ep0_in.is_ep0());

    UsbEndpoint ep0_out = UsbEndpoint::get_default_ep0_out();
    EXPECT_TRUE(ep0_out.is_ep0());

    UsbEndpoint ep_other{.address = 0x81, .attributes = 0x03, .max_packet_size = 8, .interval = 10};
    EXPECT_FALSE(ep_other.is_ep0());
}

TEST(TestUsbEndpoint, CustomMaxPacketSize) {
    std::uint16_t custom_size = 64;
    UsbEndpoint ep0 = UsbEndpoint::get_ep0_in(custom_size);
    EXPECT_EQ(ep0.max_packet_size, custom_size);
}

TEST(TestUsbInterface, BasicInterface) {
    UsbInterface intf{
            .interface_class = static_cast<std::uint8_t>(ClassCode::HID),
            .interface_subclass = 0x00,
            .interface_protocol = 0x00,
            .endpoints = {
                    UsbEndpoint{.address = 0x81, .attributes = 0x03, .max_packet_size = 8, .interval = 10}
            }
    };

    EXPECT_EQ(intf.interface_class, static_cast<std::uint8_t>(ClassCode::HID));
    EXPECT_EQ(intf.endpoints.size(), 1);
}

TEST(TestUsbDevice, BasicDevice) {
    StringPool string_pool;

    auto device = std::make_shared<UsbDevice>(UsbDevice{
            .path = "/test/device",
            .busid = "1-1",
            .bus_num = 1,
            .dev_num = 1,
            .speed = static_cast<std::uint32_t>(UsbSpeed::Full),
            .vendor_id = 0x1234,
            .product_id = 0x5678,
            .device_bcd = 0x0100,
            .device_class = 0x00,
            .device_subclass = 0x00,
            .device_protocol = 0x00,
            .configuration_value = 1,
            .num_configurations = 1,
            .interfaces = {},
            .ep0_in = UsbEndpoint::get_default_ep0_in(),
            .ep0_out = UsbEndpoint::get_default_ep0_out()
    });

    EXPECT_EQ(device->busid, "1-1");
    EXPECT_EQ(device->vendor_id, 0x1234);
    EXPECT_EQ(device->product_id, 0x5678);
}

TEST(TestUsbDevice, FindEndpoint) {
    std::vector<UsbInterface> interfaces = {
            UsbInterface{
                    .interface_class = static_cast<std::uint8_t>(ClassCode::HID),
                    .interface_subclass = 0x00,
                    .interface_protocol = 0x00,
                    .endpoints = {
                            UsbEndpoint{.address = 0x81, .attributes = 0x03, .max_packet_size = 8, .interval = 10},
                            UsbEndpoint{.address = 0x01, .attributes = 0x02, .max_packet_size = 64, .interval = 0}
                    }
            }
    };

    auto device = std::make_shared<UsbDevice>(UsbDevice{
            .path = "/test/device",
            .busid = "1-1",
            .bus_num = 1,
            .dev_num = 1,
            .speed = static_cast<std::uint32_t>(UsbSpeed::Full),
            .vendor_id = 0x1234,
            .product_id = 0x5678,
            .device_bcd = 0x0100,
            .device_class = 0x00,
            .device_subclass = 0x00,
            .device_protocol = 0x00,
            .configuration_value = 1,
            .num_configurations = 1,
            .interfaces = interfaces,
            .ep0_in = UsbEndpoint::get_default_ep0_in(),
            .ep0_out = UsbEndpoint::get_default_ep0_out()
    });

    // 查找存在的端点
    auto result = device->find_ep(0x81);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->first.address, 0x81);

    // 查找不存在的端点
    auto not_found = device->find_ep(0x82);
    EXPECT_FALSE(not_found.has_value());
}

// ============== 极端情况测试 ==============

TEST(TestUsbEndpoint, AllEndpointTypes) {
    // 控制端点
    UsbEndpoint ctrl{.address = 0x00, .attributes = 0x00, .max_packet_size = 64, .interval = 0};
    EXPECT_EQ(ctrl.attributes & 0x03, 0x00); // Control

    // 等时端点
    UsbEndpoint iso{.address = 0x81, .attributes = 0x01, .max_packet_size = 1024, .interval = 1};
    EXPECT_EQ(iso.attributes & 0x03, 0x01); // Isochronous

    // 批量端点
    UsbEndpoint bulk{.address = 0x82, .attributes = 0x02, .max_packet_size = 512, .interval = 0};
    EXPECT_EQ(bulk.attributes & 0x03, 0x02); // Bulk

    // 中断端点
    UsbEndpoint intr{.address = 0x83, .attributes = 0x03, .max_packet_size = 64, .interval = 10};
    EXPECT_EQ(intr.attributes & 0x03, 0x03); // Interrupt
}

TEST(TestUsbEndpoint, MaxEndpointAddress) {
    // 最大有效端点地址 (0x0F | 0x80 = 0x8F)
    UsbEndpoint ep_in{.address = 0x8F, .attributes = 0x02, .max_packet_size = 512, .interval = 0};
    EXPECT_TRUE(ep_in.is_in());
    EXPECT_EQ(ep_in.address & 0x0F, 15);

    UsbEndpoint ep_out{.address = 0x0F, .attributes = 0x02, .max_packet_size = 512, .interval = 0};
    EXPECT_FALSE(ep_out.is_in());
    EXPECT_EQ(ep_out.address & 0x0F, 15);
}

TEST(TestUsbDevice, EmptyInterfaces) {
    auto device = std::make_shared<UsbDevice>(UsbDevice{
            .path = "/test/device",
            .busid = "1-1",
            .bus_num = 1,
            .dev_num = 1,
            .speed = static_cast<std::uint32_t>(UsbSpeed::High),
            .vendor_id = 0x1234,
            .product_id = 0x5678,
            .device_bcd = 0x0200,
            .device_class = 0xFF, // Vendor-specific
            .device_subclass = 0xFF,
            .device_protocol = 0xFF,
            .configuration_value = 1,
            .num_configurations = 1,
            .interfaces = {},
            .ep0_in = UsbEndpoint::get_default_ep0_in(),
            .ep0_out = UsbEndpoint::get_default_ep0_out()
    });

    // 无接口设备
    EXPECT_TRUE(device->interfaces.empty());

    // 查找端点应该找不到（除了 ep0）
    auto result = device->find_ep(0x81);
    EXPECT_FALSE(result.has_value());
}

TEST(TestUsbDevice, MultipleInterfacesFindEndpoint) {
    std::vector<UsbInterface> interfaces = {
            UsbInterface{
                    .interface_class = static_cast<std::uint8_t>(ClassCode::HID),
                    .interface_subclass = 0x00,
                    .interface_protocol = 0x00,
                    .endpoints = {
                            UsbEndpoint{.address = 0x81, .attributes = 0x03, .max_packet_size = 8, .interval = 10}
                    }
            },
            UsbInterface{
                    .interface_class = static_cast<std::uint8_t>(ClassCode::CDCData),
                    .interface_subclass = 0x00,
                    .interface_protocol = 0x00,
                    .endpoints = {
                            UsbEndpoint{.address = 0x82, .attributes = 0x02, .max_packet_size = 64, .interval = 0},
                            UsbEndpoint{.address = 0x02, .attributes = 0x02, .max_packet_size = 64, .interval = 0}
                    }
            }
    };

    auto device = std::make_shared<UsbDevice>(UsbDevice{
            .path = "/test/device",
            .busid = "1-1",
            .bus_num = 1,
            .dev_num = 1,
            .speed = static_cast<std::uint32_t>(UsbSpeed::Full),
            .vendor_id = 0x1234,
            .product_id = 0x5678,
            .device_bcd = 0x0100,
            .device_class = 0x00,
            .device_subclass = 0x00,
            .device_protocol = 0x00,
            .configuration_value = 1,
            .num_configurations = 1,
            .interfaces = interfaces,
            .ep0_in = UsbEndpoint::get_default_ep0_in(),
            .ep0_out = UsbEndpoint::get_default_ep0_out()
    });

    // 在第一个接口找到
    auto result1 = device->find_ep(0x81);
    ASSERT_TRUE(result1.has_value());
    EXPECT_EQ(result1->first.address, 0x81);
    ASSERT_TRUE(result1->second.has_value());
    EXPECT_EQ(result1->second->interface_class, static_cast<std::uint8_t>(ClassCode::HID));

    // 在第二个接口找到
    auto result2 = device->find_ep(0x82);
    ASSERT_TRUE(result2.has_value());
    EXPECT_EQ(result2->first.address, 0x82);
    ASSERT_TRUE(result2->second.has_value());
    EXPECT_EQ(result2->second->interface_class, static_cast<std::uint8_t>(ClassCode::CDCData));
}

TEST(TestUsbDevice, AllSpeeds) {
    std::vector<UsbSpeed> speeds = {
            UsbSpeed::Low,
            UsbSpeed::Full,
            UsbSpeed::High,
            UsbSpeed::Super,
            UsbSpeed::SuperPlus
    };

    for (auto speed : speeds) {
        auto device = std::make_shared<UsbDevice>(UsbDevice{
                .path = "/test/device",
                .busid = "1-1",
                .bus_num = 1,
                .dev_num = 1,
                .speed = static_cast<std::uint32_t>(speed),
                .vendor_id = 0x1234,
                .product_id = 0x5678,
                .device_bcd = 0x0100,
                .device_class = 0x00,
                .device_subclass = 0x00,
                .device_protocol = 0x00,
                .configuration_value = 1,
                .num_configurations = 1,
                .interfaces = {},
                .ep0_in = UsbEndpoint::get_default_ep0_in(),
                .ep0_out = UsbEndpoint::get_default_ep0_out()
        });

        EXPECT_EQ(device->speed, static_cast<std::uint32_t>(speed));
    }
}
