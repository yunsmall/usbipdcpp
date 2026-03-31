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
