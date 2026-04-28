#include <gtest/gtest.h>

#include "protocol.h"
#include "test_utils.h"
#include "DeviceHandler/DeviceHandler.h"

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

    void receive_urb(UsbIpCommand::UsbIpCmdSubmit cmd,
                     UsbEndpoint ep,
                     std::optional<UsbInterface> interface,
                     usbipdcpp::error_code &ec) override {}
};

// 创建测试用的 UsbDevice
inline UsbDevice create_test_device() {
    return UsbDevice{
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
}

TEST(TestUsbIpRetSubmit, CreateOkWithNoIso) {
    auto test_device = create_test_device();
    MockDeviceHandlerForTest mock_handler(test_device);

    auto* trx = new GenericTransfer{};
    trx->data = {0x01, 0x02, 0x03, 0x04};
    trx->actual_length = trx->data.size();

    TransferHandle handle(trx, &mock_handler);
    auto ret = UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
            0x1234, static_cast<std::uint32_t>(trx->actual_length), std::move(handle));

    EXPECT_EQ(ret.header.seqnum, 0x1234);
    EXPECT_EQ(ret.status, 0);
    EXPECT_EQ(ret.actual_length, trx->actual_length);
    EXPECT_TRUE(ret.transfer);
    // TransferHandle 析构时自动释放
}

TEST(TestUsbIpRetSubmit, CreateOkWithoutData) {
    auto ret = UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(0x5678, 0);

    EXPECT_EQ(ret.header.seqnum, 0x5678);
    EXPECT_EQ(ret.status, 0);
    EXPECT_EQ(ret.actual_length, 0);
    EXPECT_FALSE(ret.transfer);
}

TEST(TestUsbIpRetSubmit, CreateEpipeNoIso) {
    auto test_device = create_test_device();
    MockDeviceHandlerForTest mock_handler(test_device);

    auto* trx = new GenericTransfer{};
    trx->data = {0xAA, 0xBB};
    trx->actual_length = trx->data.size();

    TransferHandle handle(trx, &mock_handler);
    auto ret = UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_no_iso(
            0xABCD, static_cast<std::uint32_t>(trx->actual_length), std::move(handle));

    EXPECT_EQ(ret.header.seqnum, 0xABCD);
    EXPECT_EQ(ret.status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
    // TransferHandle 析构时自动释放
}

TEST(TestUsbIpRetSubmit, CreateEpipeWithoutData) {
    auto ret = UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(0x1111, 0);

    EXPECT_EQ(ret.header.seqnum, 0x1111);
    EXPECT_EQ(ret.status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));
    EXPECT_FALSE(ret.transfer);
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
    auto test_device = create_test_device();
    MockDeviceHandlerForTest mock_handler(test_device);

    auto* trx = new GenericTransfer{};
    trx->data.resize(65536, 0xAB);
    trx->actual_length = trx->data.size();

    TransferHandle handle(trx, &mock_handler);
    auto ret = UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
            0x1234, static_cast<std::uint32_t>(trx->actual_length), std::move(handle));

    EXPECT_EQ(ret.actual_length, trx->actual_length);
    // TransferHandle 析构时自动释放
}

TEST(TestUsbIpRetSubmit, ZeroLengthData) {
    auto ret = UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(0x1234, 0);

    EXPECT_EQ(ret.actual_length, 0);
    EXPECT_FALSE(ret.transfer);
}

TEST(TestUsbIpRetSubmit, WithIsoPacketDescriptors) {
    // 带等时包描述符
    auto test_device = create_test_device();
    MockDeviceHandlerForTest mock_handler(test_device);

    auto* trx = new GenericTransfer{};
    trx->data.resize(1536, 0xCD);
    trx->actual_length = 1536;
    trx->iso_descriptors = {
            {.offset = 0, .length = 1024, .actual_length = 1024, .status = 0},
            {.offset = 1024, .length = 1024, .actual_length = 512, .status = 0},
    };

    TransferHandle handle(trx, &mock_handler);
    auto ret = UsbIpResponse::UsbIpRetSubmit::create_ret_submit(
            0x1234, 0, 1536, 0, 2, std::move(handle));

    EXPECT_EQ(ret.number_of_packets, 2);
    // TransferHandle 析构时自动释放
}

TEST(TestUsbIpRetSubmit, VariousErrorStatus) {
    // 测试各种错误状态
    auto ret_epipe = UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(0x1234, 0);
    EXPECT_EQ(ret_epipe.status, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE));

    // create_ret_submit 直接接受 errno 并存储，内部不转换
    auto ret_enoent = UsbIpResponse::UsbIpRetSubmit::create_ret_submit(0x1234, ENOENT, 0, 0, 0, TransferHandle());
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
