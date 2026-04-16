#include <gtest/gtest.h>

#include "protocol.h"
#include "test_utils.h"

using namespace usbipdcpp;
using namespace usbipdcpp::test;

TEST(TestUsbIpRetSubmit, CreateOkWithNoIso) {
    data_type data = {0x01, 0x02, 0x03, 0x04};
    auto ret = UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(0x1234, data);

    EXPECT_EQ(ret.header.seqnum, 0x1234);
    EXPECT_EQ(ret.status, 0);
    EXPECT_EQ(ret.actual_length, data.size());
    EXPECT_EQ(ret.transfer_buffer, data);
}

TEST(TestUsbIpRetSubmit, CreateOkWithoutData) {
    auto ret = UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(0x5678);

    EXPECT_EQ(ret.header.seqnum, 0x5678);
    EXPECT_EQ(ret.status, 0);
    EXPECT_EQ(ret.actual_length, 0);
    EXPECT_TRUE(ret.transfer_buffer.empty());
}

TEST(TestUsbIpRetSubmit, CreateEpipeNoIso) {
    data_type data = {0xAA, 0xBB};
    auto ret = UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_no_iso(0xABCD, data);

    EXPECT_EQ(ret.header.seqnum, 0xABCD);
    EXPECT_EQ(ret.status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
    EXPECT_EQ(ret.transfer_buffer, data);
}

TEST(TestUsbIpRetSubmit, CreateEpipeWithoutData) {
    auto ret = UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(0x1111);

    EXPECT_EQ(ret.header.seqnum, 0x1111);
    EXPECT_EQ(ret.status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
    EXPECT_TRUE(ret.transfer_buffer.empty());
}

TEST(TestUsbIpRetUnlink, CreateSuccess) {
    auto ret = UsbIpResponse::UsbIpRetUnlink::create_ret_unlink_success(0x9999);

    EXPECT_EQ(ret.header.seqnum, 0x9999);
    EXPECT_EQ(ret.status, 0);
}

TEST(TestUsbIpRetUnlink, ToBytes) {
    auto ret = UsbIpResponse::UsbIpRetUnlink::create_ret_unlink_success(0x1234);
    auto bytes = ret.to_bytes();

    // 检查基本大小（header + status + padding）
    EXPECT_GT(bytes.size(), 0);
}

TEST(TestOpRepImport, CreateOnFailure) {
    auto ret = UsbIpResponse::OpRepImport::create_on_failure_with_status(
            static_cast<std::uint32_t>(OperationStatuType::NoDev));

    EXPECT_EQ(ret.status, static_cast<std::uint32_t>(OperationStatuType::NoDev));
    EXPECT_FALSE(ret.device);
}

TEST(TestOpRepImport, CreateOnSuccess) {
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

    auto ret = UsbIpResponse::OpRepImport::create_on_success(device);

    EXPECT_EQ(ret.status, 0);
    EXPECT_TRUE(ret.device);
    EXPECT_EQ(ret.device->busid, "1-1");
}

TEST(TestOpRepDevlist, CreateFromDevices) {
    std::vector<std::shared_ptr<UsbDevice>> devices;

    devices.push_back(std::make_shared<UsbDevice>(UsbDevice{
            .path = "/test/device1",
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
    }));

    devices.push_back(std::make_shared<UsbDevice>(UsbDevice{
            .path = "/test/device2",
            .busid = "1-2",
            .bus_num = 1,
            .dev_num = 2,
            .speed = static_cast<std::uint32_t>(UsbSpeed::Low),
            .vendor_id = 0xABCD,
            .product_id = 0xEF01,
            .device_bcd = 0x0100,
            .device_class = 0x00,
            .device_subclass = 0x00,
            .device_protocol = 0x00,
            .configuration_value = 1,
            .num_configurations = 1,
            .interfaces = {},
            .ep0_in = UsbEndpoint::get_default_ep0_in(),
            .ep0_out = UsbEndpoint::get_default_ep0_out()
    }));

    auto ret = UsbIpResponse::OpRepDevlist::create_from_devices(devices);

    EXPECT_EQ(ret.status, 0);
    EXPECT_EQ(ret.device_count, 2);
    EXPECT_EQ(ret.devices.size(), 2);
}

TEST(TestOpRepDevlist, EmptyDeviceList) {
    std::vector<std::shared_ptr<UsbDevice>> devices;
    auto ret = UsbIpResponse::OpRepDevlist::create_from_devices(devices);

    EXPECT_EQ(ret.status, 0);
    EXPECT_EQ(ret.device_count, 0);
    EXPECT_TRUE(ret.devices.empty());
}

// ============== 极端情况测试 ==============

TEST(TestUsbIpRetSubmit, LargeData) {
    // 大数据量
    data_type large_data(65536, 0xAB);
    auto ret = UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(0x1234, large_data);

    EXPECT_EQ(ret.actual_length, large_data.size());
    EXPECT_EQ(ret.transfer_buffer.size(), large_data.size());
}

TEST(TestUsbIpRetSubmit, ZeroLengthData) {
    data_type empty_data;
    auto ret = UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(0x1234, empty_data);

    EXPECT_EQ(ret.actual_length, 0);
    EXPECT_TRUE(ret.transfer_buffer.empty());
}

TEST(TestUsbIpRetSubmit, WithIsoPacketDescriptors) {
    // 带等时包描述符
    std::vector<UsbIpIsoPacketDescriptor> iso_descs = {
            {.offset = 0, .length = 1024, .actual_length = 1024, .status = 0},
            {.offset = 1024, .length = 1024, .actual_length = 512, .status = 0},
    };

    UsbIpResponse::UsbIpRetSubmit ret{
            .header = UsbIpHeaderBasic::get_server_header(USBIP_RET_SUBMIT, 0x1234),
            .status = 0,
            .actual_length = 1536,
            .start_frame = 0,
            .number_of_packets = static_cast<std::uint32_t>(iso_descs.size()),
            .error_count = 0,
            .transfer_buffer = data_type(1536, 0xCD),
            .iso_packet_descriptor = std::move(iso_descs)
    };

    EXPECT_EQ(ret.number_of_packets, 2);
    EXPECT_EQ(ret.iso_packet_descriptor.size(), 2);
}

TEST(TestUsbIpRetSubmit, VariousErrorStatus) {
    // 测试各种错误状态
    auto ret_epipe = UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(0x1234);
    EXPECT_EQ(ret_epipe.status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));

    // create_ret_submit 直接接受 errno 并存储，内部不转换
    auto ret_enoent = UsbIpResponse::UsbIpRetSubmit::create_ret_submit(0x1234, ENOENT, 0, 0, {}, {});
    EXPECT_EQ(ret_enoent.status, static_cast<std::uint32_t>(ENOENT));
}

TEST(TestUsbIpRetUnlink, UnlinkError) {
    // create_ret_unlink 直接接受 errno 并存储，内部不转换
    auto ret = UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(0x1234, EPIPE);
    EXPECT_EQ(ret.status, static_cast<std::uint32_t>(EPIPE));
}

TEST(TestOpRepImport, DeviceWithMultipleInterfaces) {
    std::vector<UsbInterface> interfaces = {
            UsbInterface{
                    .interface_class = static_cast<std::uint8_t>(ClassCode::CDC),
                    .interface_subclass = 0x02,
                    .interface_protocol = 0x01,
                    .endpoints = {
                            UsbEndpoint{.address = 0x83, .attributes = 0x03, .max_packet_size = 16, .interval = 255}
                    },
                    .handler = nullptr
            },
            UsbInterface{
                    .interface_class = static_cast<std::uint8_t>(ClassCode::CDCData),
                    .interface_subclass = 0x00,
                    .interface_protocol = 0x00,
                    .endpoints = {
                            UsbEndpoint{.address = 0x81, .attributes = 0x02, .max_packet_size = 64, .interval = 0},
                            UsbEndpoint{.address = 0x02, .attributes = 0x02, .max_packet_size = 64, .interval = 0}
                    },
                    .handler = nullptr
            }
    };

    auto device = std::make_shared<UsbDevice>(UsbDevice{
            .path = "/test/cdc_device",
            .busid = "1-2",
            .bus_num = 1,
            .dev_num = 2,
            .speed = static_cast<std::uint32_t>(UsbSpeed::Full),
            .vendor_id = 0x2345,
            .product_id = 0x6789,
            .device_bcd = 0x0100,
            .device_class = 0x02, // CDC
            .device_subclass = 0x00,
            .device_protocol = 0x00,
            .configuration_value = 1,
            .num_configurations = 1,
            .interfaces = interfaces,
            .ep0_in = UsbEndpoint::get_default_ep0_in(),
            .ep0_out = UsbEndpoint::get_default_ep0_out()
    });

    auto ret = UsbIpResponse::OpRepImport::create_on_success(device);

    EXPECT_EQ(ret.status, 0);
    EXPECT_TRUE(ret.device);
    EXPECT_EQ(ret.device->interfaces.size(), 2);
}

TEST(TestOpRepDevlist, ManyDevices) {
    std::vector<std::shared_ptr<UsbDevice>> devices;

    // 创建多个设备
    for (int i = 0; i < 10; ++i) {
        devices.push_back(std::make_shared<UsbDevice>(UsbDevice{
                .path = "/test/device" + std::to_string(i),
                .busid = "1-" + std::to_string(i + 1),
                .bus_num = 1,
                .dev_num = static_cast<std::uint8_t>(i + 1),
                .speed = static_cast<std::uint32_t>(UsbSpeed::Full),
                .vendor_id = static_cast<std::uint16_t>(0x1234 + i),
                .product_id = static_cast<std::uint16_t>(0x5678 + i),
                .device_bcd = 0x0100,
                .device_class = 0x00,
                .device_subclass = 0x00,
                .device_protocol = 0x00,
                .configuration_value = 1,
                .num_configurations = 1,
                .interfaces = {},
                .ep0_in = UsbEndpoint::get_default_ep0_in(),
                .ep0_out = UsbEndpoint::get_default_ep0_out()
        }));
    }

    auto ret = UsbIpResponse::OpRepDevlist::create_from_devices(devices);

    EXPECT_EQ(ret.status, 0);
    EXPECT_EQ(ret.device_count, 10);
    EXPECT_EQ(ret.devices.size(), 10);
}
