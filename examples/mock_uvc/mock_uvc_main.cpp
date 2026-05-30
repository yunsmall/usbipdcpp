#include <iostream>

#include "Device.h"
#include "Server.h"
#include "usbipdcpp.h"
#include "virtual_device/UvcConstants.h"
#include "virtual_device/UvcVirtualInterfaceHandler.h"
#include "virtual_device/video_sources/ColorBarSource.h"

using namespace usbipdcpp;

int main() {
    spdlog::set_level(spdlog::level::trace);

    StringPool string_pool;

    // UVC 1.5 Table 3-2/3-13: VC/VS interface protocol must be PC_PROTOCOL_15
    std::vector<UsbInterface> interfaces = {
            // Interface 0: VideoControl（含中断端点用于状态通知）
            UsbInterface{
                    .interface_class = CC_VIDEO,
                    .interface_subclass = SC_VIDEOCONTROL,
                    .interface_protocol = PC_PROTOCOL_15,
                    .endpoints = {{UsbEndpoint{
                            .address = 0x87, // IN, endpoint 7 — interrupt for status
                            .attributes = 0x03, // Interrupt
                            .max_packet_size = 16,
                            .interval = 8,
                    }}},
            },
            // Interface 1: VideoStreaming（alt 0 空端点，alt 1 ISO IN 端点）
            UsbInterface{
                    .interface_class = CC_VIDEO,
                    .interface_subclass = SC_VIDEOSTREAMING,
                    .interface_protocol = PC_PROTOCOL_15,
                    .endpoints = {{}, // alt 0: zero bandwidth
                                  {UsbEndpoint{
                                          .address = 0x81, // IN, endpoint 1
                                          .attributes = static_cast<std::uint8_t>(EndpointAttributes::Isochronous) |
                                                        static_cast<std::uint8_t>(IsoSyncType::Async),
                                          .max_packet_size = 512,
                                          .interval = 1,
                                  }}},
            },
    };

    auto device = std::make_shared<UsbDevice>(UsbDevice{
            .path = "/usbipdcpp/mock_uvc",
            .busid = "1-1",
            .bus_num = 1,
            .dev_num = 1,
            .speed = static_cast<std::uint32_t>(UsbSpeed::High),
            .vendor_id = 0x1234,
            .product_id = 0x5681,
            .device_bcd = 0x0100,
            .device_class = 0xEF, // Miscellaneous (IAD)
            .device_subclass = 0x02, // Common Class
            .device_protocol = 0x01, // Interface Association Descriptor
            .configuration_value = 1,
            .num_configurations = 1,
            .interfaces = interfaces,
            .ep0_in = UsbEndpoint::get_ep0_in(UsbSpeed::High),
            .ep0_out = UsbEndpoint::get_ep0_out(UsbSpeed::High),
    });

    // UvcDeviceHelper 创建 VC/VS handler 并注册 + 设置描述符
    auto source = std::make_unique<ColorBarSource>(320, 240, 15);
    UvcDeviceHelper::setup(device, string_pool, std::move(source));

    Server server;
    server.add_device(std::move(device));

    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), 54326};

    server.start(endpoint);

    SPDLOG_INFO("Mock UVC camera started on port 54326");
    SPDLOG_INFO("Connect: usbip attach -r <host> -b 1-1");
    SPDLOG_INFO("Press Enter to stop...");

    std::cin.get();

    server.stop();
    return 0;
}
