#include <iostream>

#include "usbipdcpp.h"
#include "mock_cdc_acm.h"

using namespace usbipdcpp;

int main() {
    spdlog::set_level(spdlog::level::debug);

    StringPool string_pool;

    // CDC ACM 需要两个接口：通信接口和数据接口
    std::vector<UsbInterface> interfaces = {
        // Communication Interface
        UsbInterface{
            .interface_class = 0x02,      // CDC Communication
            .interface_subclass = 0x02,   // ACM
            .interface_protocol = 0x01,   // AT Commands (v25ter)
            .endpoints = {
                // Interrupt IN for serial state notifications
                UsbEndpoint{
                    .address = 0x83,      // IN endpoint 3
                    .attributes = 0x03,   // Interrupt
                    .max_packet_size = 64,
                    .interval = 16
                }
            }
        },
        // Data Interface
        UsbInterface{
            .interface_class = 0x0A,      // CDC Data
            .interface_subclass = 0x00,
            .interface_protocol = 0x00,
            .endpoints = {
                // Bulk IN
                UsbEndpoint{
                    .address = 0x81,      // IN endpoint 1
                    .attributes = 0x02,   // Bulk
                    .max_packet_size = 64,
                    .interval = 0
                },
                // Bulk OUT
                UsbEndpoint{
                    .address = 0x02,      // OUT endpoint 2
                    .attributes = 0x02,   // Bulk
                    .max_packet_size = 64,
                    .interval = 0
                }
            }
        }
    };

    // 设置接口处理器
    interfaces[0].with_handler<MockCdcAcmCommunicationInterfaceHandler>(string_pool);
    interfaces[1].with_handler<MockCdcAcmDataInterfaceHandler>(string_pool);

    // 创建设备
    auto mock_cdc_acm = std::make_shared<UsbDevice>(UsbDevice{
        .path = "/usbipdcpp/mock_cdc_acm",
        .busid = "1-3",
        .bus_num = 1,
        .dev_num = 3,
        .speed = static_cast<std::uint32_t>(UsbSpeed::Full),
        .vendor_id = 0x1234,
        .product_id = 0x5680,  // CDC ACM device
        .device_bcd = 0x0100,
        .device_class = 0x02,      // CDC Communication (at device level for IAD)
        .device_subclass = 0x00,
        .device_protocol = 0x00,
        .configuration_value = 1,
        .num_configurations = 1,
        .interfaces = interfaces,
        .ep0_in = UsbEndpoint::get_default_ep0_in(),
        .ep0_out = UsbEndpoint::get_default_ep0_out(),
    });
    mock_cdc_acm->with_handler<SimpleVirtualDeviceHandler>(string_pool);

    // 关联通信接口和数据接口处理器
    auto &comm_handler = *std::dynamic_pointer_cast<MockCdcAcmCommunicationInterfaceHandler>(
        mock_cdc_acm->interfaces[0].handler);
    auto &data_handler = *std::dynamic_pointer_cast<MockCdcAcmDataInterfaceHandler>(
        mock_cdc_acm->interfaces[1].handler);
    comm_handler.set_data_handler(&data_handler);
    data_handler.set_comm_handler(&comm_handler);

    Server server;
    server.add_device(std::move(mock_cdc_acm));

    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), 54326};

    server.start(endpoint);

    SPDLOG_INFO("Mock CDC ACM (virtual serial port) started");
    SPDLOG_INFO("Port: 54326");
    SPDLOG_INFO("Busid: 1-3");
    SPDLOG_INFO("This is an echo serial port - data sent will be echoed back");
    SPDLOG_INFO("Connect with: usbip attach -r <host> -b 1-3");
    SPDLOG_INFO("Then use: screen /dev/ttyACMx or minicom");
    SPDLOG_INFO("Press Enter to exit...");

    std::cin.get();

    SPDLOG_INFO("Stopping server...");
    server.stop();

    return 0;
}
