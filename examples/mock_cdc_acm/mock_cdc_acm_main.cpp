#include <cxxopts.hpp>
#include <iostream>

#include "mock_cdc_acm.h"
#include "usbipdcpp_core.h"
#include "virtual_device/SimpleVirtualDeviceHandler.h"

using namespace usbipdcpp;

int main(int argc, char **argv) {
    cxxopts::Options options("mock_cdc_acm", "USB/IP virtual serial port device");
    options.add_options()
        ("p,port", "TCP port", cxxopts::value<std::uint16_t>()->default_value("53240"))
        ("b,busid", "Bus ID", cxxopts::value<std::string>()->default_value("1-1"))
        ("help", "Print help");
    cxxopts::ParseResult result;
    try {
        result = options.parse(argc, argv);
    } catch (const cxxopts::exceptions::exception &e) {
        std::cerr << e.what() << std::endl;
        std::cout << options.help() << std::endl;
        return 1;
    }
    if (result.count("help")) {
        std::cout << options.help() << std::endl;
        return 0;
    }
    auto port = result["port"].as<std::uint16_t>();
    auto busid = result["busid"].as<std::string>();

    spdlog::set_level(spdlog::level::debug);

    StringPool string_pool;

    // CDC ACM 需要两个接口：通信接口和数据接口
    std::vector<UsbInterface> interfaces = {// Communication Interface
                                            UsbInterface{.interface_class = 0x02, // CDC Communication
                                                         .interface_subclass = 0x02, // ACM
                                                         .interface_protocol = 0x01, // AT Commands (v25ter)
                                                         .endpoints = {{// Interrupt IN for serial state notifications
                                                                        UsbEndpoint{.address = 0x83, // IN endpoint 3
                                                                                    .attributes = 0x03, // Interrupt
                                                                                    .max_packet_size = 64,
                                                                                    .interval = 16}}}},
                                            // Data Interface
                                            UsbInterface{.interface_class = 0x0A, // CDC Data
                                                         .interface_subclass = 0x00,
                                                         .interface_protocol = 0x00,
                                                         .endpoints = {{// Bulk IN
                                                                        UsbEndpoint{.address = 0x81, // IN endpoint 1
                                                                                    .attributes = 0x02, // Bulk
                                                                                    .max_packet_size = 64,
                                                                                    .interval = 0},
                                                                        // Bulk OUT
                                                                        UsbEndpoint{.address = 0x02, // OUT endpoint 2
                                                                                    .attributes = 0x02, // Bulk
                                                                                    .max_packet_size = 64,
                                                                                    .interval = 0}}}}};

    // 设置接口处理器
    interfaces[0].with_handler<MockCdcAcmCommunicationInterfaceHandler>(string_pool);
    interfaces[1].with_handler<MockCdcAcmDataInterfaceHandler>(string_pool);

    // 创建设备
    auto mock_cdc_acm = std::make_shared<UsbDevice>(UsbDevice{
            .path = "/usbipdcpp/mock_cdc_acm",
            .busid = busid,
            .bus_num = 1,
            .dev_num = 1,
            .speed = static_cast<std::uint32_t>(UsbSpeed::Full),
            .vendor_id = 0x1234,
            .product_id = 0x5680, // CDC ACM device
            .device_bcd = 0x0100,
            .device_class = 0x02, // CDC Communication (at device level for IAD)
            .device_subclass = 0x00,
            .device_protocol = 0x00,
            .configuration_value = 1,
            .num_configurations = 1,
            .interfaces = interfaces,
            .ep0_in = UsbEndpoint::get_ep0_in(UsbSpeed::Full),
            .ep0_out = UsbEndpoint::get_ep0_out(UsbSpeed::Full),
    });
    auto device_handler = mock_cdc_acm->with_handler<SimpleVirtualDeviceHandler>(string_pool);
    device_handler->setup_interface_handlers();

    // 关联通信接口和数据接口处理器
    auto &comm_handler =
            *std::dynamic_pointer_cast<MockCdcAcmCommunicationInterfaceHandler>(mock_cdc_acm->interfaces[0].handler);
    auto &data_handler =
            *std::dynamic_pointer_cast<MockCdcAcmDataInterfaceHandler>(mock_cdc_acm->interfaces[1].handler);
    comm_handler.set_data_handler(&data_handler);
    data_handler.set_comm_handler(&comm_handler);

    Server server;
    server.add_device(std::move(mock_cdc_acm));

    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), port};

    server.start(endpoint);

    SPDLOG_INFO("Mock CDC ACM (virtual serial port) started on port {}, busid {}", port, busid);
    SPDLOG_INFO("Connect with: usbip attach -r <host> -b {}", busid);
    SPDLOG_INFO("Then use: screen /dev/ttyACMx or minicom");
    SPDLOG_INFO("Press Enter to exit...");

    std::cin.get();

    SPDLOG_INFO("Stopping server...");
    server.stop();

    return 0;
}
