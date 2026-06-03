#include <iostream>

#include "../example_utils.h"

#include "virtual_device/devices/MscBulkOnlyHandler.h"
#include "virtual_device/storage_backends/RawImageBackend.h"
#include "virtual_device/SimpleVirtualDeviceHandler.h"
#include "usbipdcpp_core.h"

using namespace usbipdcpp;

int main(int argc, char **argv) {
    auto opts = make_example_options("mock_msc", "USB/IP virtual USB flash drive");
    opts.add_options()("i,image", "Disk image path", cxxopts::value<std::string>()->default_value("disk.img"));
    auto result = parse_example_args(opts, argc, argv);
    auto port = result["port"].as<std::uint16_t>();
    auto busid = result["busid"].as<std::string>();
    auto image_path = result["image"].as<std::string>();

    spdlog::set_level(spdlog::level::trace);

    StringPool string_pool;

    std::vector<UsbInterface> interfaces = {UsbInterface{.interface_class = 0x08, // Mass Storage
                                                         .interface_subclass = 0x06, // SCSI transparent
                                                         .interface_protocol = 0x50, // Bulk-Only Transport
                                                         .endpoints = {{// Bulk IN
                                                                        UsbEndpoint{.address = 0x81,
                                                                                    .attributes = 0x02, // Bulk
                                                                                    .max_packet_size = 512,
                                                                                    .interval = 0},
                                                                        // Bulk OUT
                                                                        UsbEndpoint{.address = 0x02,
                                                                                    .attributes = 0x02, // Bulk
                                                                                    .max_packet_size = 512,
                                                                                    .interval = 0}}}}};

    auto backend = std::unique_ptr<StorageBackend>(std::make_unique<RawImageBackend>(image_path, 4096));
    interfaces[0].with_handler<MscBulkOnlyHandler>(string_pool, std::move(backend));

    auto device = std::make_shared<UsbDevice>(UsbDevice{
            .path = "/usbipdcpp/mock_msc",
            .busid = busid,
            .bus_num = 1,
            .dev_num = 1,
            .speed = static_cast<std::uint32_t>(UsbSpeed::High),
            .vendor_id = 0x1234,
            .product_id = 0x5681,
            .device_bcd = 0x0100,
            .device_class = 0x00,
            .device_subclass = 0x00,
            .device_protocol = 0x00,
            .configuration_value = 1,
            .num_configurations = 1,
            .interfaces = interfaces,
            .ep0_in = UsbEndpoint::get_ep0_in(UsbSpeed::Full),
            .ep0_out = UsbEndpoint::get_ep0_out(UsbSpeed::Full),
    });
    auto device_handler = device->with_handler<SimpleVirtualDeviceHandler>(string_pool);
    device_handler->setup_interface_handlers();

    Server server;
    server.add_device(std::move(device));

    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), port};

    server.start(endpoint);

    SPDLOG_INFO("Mock MSC (USB Flash Drive) started on port {}, busid {}", port, busid);
    SPDLOG_INFO("Image: {}", image_path);
    SPDLOG_INFO("Connect: usbip attach -r <host> -b {}", busid);
    SPDLOG_INFO("Press Enter to exit...");

    std::cin.get();

    SPDLOG_INFO("Stopping...");
    server.stop();
    return 0;
}
