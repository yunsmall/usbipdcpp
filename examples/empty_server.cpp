#include <print>
#include <iostream>

#include <spdlog/spdlog.h>

#include "Server.h"
#include "constant.h"
#include "DeviceHandler/DeviceHandler.h"
#include "InterfaceHandler/InterfaceHandler.h"

using namespace usbipdcpp;

int main() {
    spdlog::set_level(spdlog::level::trace);

    StringPool string_pool;

    Server server({
            UsbDevice{
                    .path = "/sys/devices/pci0000:00/0000:00:1d.1/usb3/1-1",
                    .busid = "1-1",
                    .bus_num = 1,
                    .dev_num = 1,
                    .speed = static_cast<std::uint32_t>(UsbSpeed::Low),
                    .vendor_id = 0x1234,
                    .product_id = 0x5678,
                    .device_bcd = 0xabcd,
                    .device_class = 0x00,
                    .device_subclass = 0x00,
                    .device_protocol = 0x00,
                    .configuration_value = 1,
                    .num_configurations = 1,
                    .interfaces = {
                            UsbInterface{
                                    .interface_class = static_cast<std::uint8_t>(ClassCode::HID),
                                    .interface_subclass = 0x01,
                                    .interface_protocol = 0x02,
                                    .endpoints = {
                                            UsbEndpoint{
                                                    .address = 0x80,
                                                    .attributes = 0x03,
                                                    .max_packet_size = 0x08,
                                                    .interval = 10
                                            }
                                    }

                            }
                    },
                    .ep0_in = UsbEndpoint::get_default_ep0_in(),
                    .ep0_out = UsbEndpoint::get_default_ep0_out(),
            }
    });

    asio::ip::tcp::endpoint endpoint(asio::ip::tcp::v4(), 54321);
    server.start(endpoint);

    std::system("pause");


    return 0;
}
