#include <cxxopts.hpp>
#include <iostream>

#include "FfmpegSource.h"
#include "Device.h"
#include "Server.h"
#include "usbipdcpp_core.h"
#include "virtual_device/UvcConstants.h"
#include "virtual_device/UvcVirtualInterfaceHandler.h"

using namespace usbipdcpp;

int main(int argc, char **argv) {
    cxxopts::Options options("mock_uvc_ffmpeg", "USB/IP virtual UVC camera — FFmpeg video source");
    options.add_options()
        ("p,port", "TCP port", cxxopts::value<std::uint16_t>()->default_value("53240"))
        ("b,busid", "Bus ID", cxxopts::value<std::string>()->default_value("1-1"))
        ("v,video", "Video file path", cxxopts::value<std::string>()->default_value("video.mp4"))
        ("passthrough", "Passthrough MJPEG/H264 without transcoding")
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
    auto video_path = result["video"].as<std::string>();
    auto passthrough = result.count("passthrough") > 0;

    spdlog::set_level(spdlog::level::trace);

    StringPool string_pool;

    std::vector<UsbInterface> interfaces = {
            UsbInterface{
                    .interface_class = CC_VIDEO,
                    .interface_subclass = SC_VIDEOCONTROL,
                    .interface_protocol = PC_PROTOCOL_15,
                    .endpoints = {{UsbEndpoint{
                            .address = 0x87,
                            .attributes = 0x03,
                            .max_packet_size = 16,
                            .interval = 8,
                    }}},
            },
            UsbInterface{
                    .interface_class = CC_VIDEO,
                    .interface_subclass = SC_VIDEOSTREAMING,
                    .interface_protocol = PC_PROTOCOL_15,
                    .endpoints = {{},
                                  {UsbEndpoint{
                                          .address = 0x81,
                                          .attributes = static_cast<std::uint8_t>(EndpointAttributes::Isochronous) |
                                                        static_cast<std::uint8_t>(IsoSyncType::Async),
                                          .max_packet_size = 512,
                                          .interval = 1,
                                  }}},
            },
    };

    auto device = std::make_shared<UsbDevice>(UsbDevice{
            .path = "/usbipdcpp/mock_uvc_ffmpeg",
            .busid = busid,
            .bus_num = 1,
            .dev_num = 1,
            .speed = static_cast<std::uint32_t>(UsbSpeed::High),
            .vendor_id = 0x1234,
            .product_id = 0x5681,
            .device_bcd = 0x0100,
            .device_class = 0xEF,
            .device_subclass = 0x02,
            .device_protocol = 0x01,
            .configuration_value = 1,
            .num_configurations = 1,
            .interfaces = interfaces,
            .ep0_in = UsbEndpoint::get_ep0_in(UsbSpeed::High),
            .ep0_out = UsbEndpoint::get_ep0_out(UsbSpeed::High),
    });

    auto source = std::make_unique<FfmpegSource>(video_path, passthrough);
    UvcDeviceHelper::setup(device, string_pool, std::move(source));

    Server server;
    server.add_device(std::move(device));

    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), port};

    server.start(endpoint);

    SPDLOG_INFO("Mock UVC camera (FFmpeg) started on port {}, busid {}", port, busid);
    SPDLOG_INFO("Video: {}", video_path);
    SPDLOG_INFO("Connect: usbip attach -r <host> -b {}", busid);
    SPDLOG_INFO("Press Enter to stop...");

    std::cin.get();

    server.stop();
    return 0;
}
