#include <iostream>

#include "usbipdcpp.h"
#include "virtual_device/devices/MscBulkOnlyHandler.h"
#include "virtual_device/storage_backends/RawImageBackend.h"

using namespace usbipdcpp;

int main(int argc, char **argv) {
    spdlog::set_level(spdlog::level::debug);

    std::string image_path = "disk.img";
    if (argc > 1)
        image_path = argv[1];

    StringPool string_pool;

    std::vector<UsbInterface> interfaces = {
            UsbInterface{
                    .interface_class = 0x08,    // Mass Storage
                    .interface_subclass = 0x06, // SCSI transparent
                    .interface_protocol = 0x50, // Bulk-Only Transport
                    .endpoints = {
                            // Bulk IN
                            UsbEndpoint{
                                    .address = 0x81,
                                    .attributes = 0x02, // Bulk
                                    .max_packet_size = 512,
                                    .interval = 0
                            },
                            // Bulk OUT
                            UsbEndpoint{
                                    .address = 0x02,
                                    .attributes = 0x02, // Bulk
                                    .max_packet_size = 512,
                                    .interval = 0
                            }
                    }
            }
    };

    auto backend = std::unique_ptr<StorageBackend>(
            std::make_unique<RawImageBackend>(image_path, 4096));
    interfaces[0].with_handler<MscBulkOnlyHandler>(string_pool, std::move(backend));

    auto device = std::make_shared<UsbDevice>(UsbDevice{
            .path = "/usbipdcpp/mock_msc",
            .busid = "1-1",
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
            .ep0_in = UsbEndpoint::get_default_ep0_in(),
            .ep0_out = UsbEndpoint::get_default_ep0_out(),
    });
    auto device_handler = device->with_handler<SimpleVirtualDeviceHandler>(string_pool);
    device_handler->setup_interface_handlers();

    Server server;
    server.add_device(std::move(device));

    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), 54327};

    server.start(endpoint);

    SPDLOG_INFO("Mock MSC (USB Flash Drive) started");
    SPDLOG_INFO("Port: 54327  Busid: 1-1");
    SPDLOG_INFO("Image: {} ({} MiB)", image_path,
                4096 * 512 / 1024 / 1024);
    SPDLOG_INFO("Connect: usbip attach -r <host> -b 1-1");
    SPDLOG_INFO("Press Enter to exit...");

    std::cin.get();

    SPDLOG_INFO("Stopping...");
    server.stop();
    return 0;
}
