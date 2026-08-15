#include <algorithm>
#include <iostream>
#include <sstream>

#include "../example_utils.h"
#include "usbipdcpp/Device.h"
#include "usbipdcpp/Server.h"
#include "usbipdcpp/usbipdcpp_core.h"
#include "usbipdcpp/virtual_device/UacConstants.h"
#include "usbipdcpp/virtual_device/UacVirtualInterfaceHandler.h"
#include "usbipdcpp/virtual_device/audio_sources/SineWaveSource.h"

using namespace usbipdcpp;

/// 解析逗号分隔的采样率列表，如 "48000,16000,8000"
static std::vector<std::uint32_t> parse_sample_rates(const std::string &str) {
    std::vector<std::uint32_t> rates;
    std::stringstream ss(str);
    std::string item;
    while (std::getline(ss, item, ',')) {
        auto rate = std::stoul(item);
        if (rate > 0 && rate % 8000 == 0) {
            rates.push_back(static_cast<std::uint32_t>(rate));
        }
    }
    return rates;
}

int main(int argc, char **argv) {
    auto opts = make_example_options("mock_audio", "USB/IP virtual UAC microphone");
    opts.add_options()
        ("freq", "Sine frequency in Hz (integer)", cxxopts::value<int>()->default_value("440"))
        ("rates", "Comma-separated sample rate list in Hz (multiples of 8000, first is initial)",
         cxxopts::value<std::string>()->default_value("48000"))
        ("channels", "Channel count (1 or 2)", cxxopts::value<int>()->default_value("1"))
        ("amp", "Amplitude 0-100 (percent of full scale)", cxxopts::value<int>()->default_value("50"));
    auto result = parse_example_args(opts, argc, argv);
    auto port = result["port"].as<std::uint16_t>();
    auto busid = result["busid"].as<std::string>();
    auto freq = result["freq"].as<int>();
    auto rates = parse_sample_rates(result["rates"].as<std::string>());
    auto channels = result["channels"].as<int>();
    auto amp = result["amp"].as<int>();

    if (rates.empty()) {
        std::cerr << "Sample rate list must contain at least one positive multiple of 8000" << std::endl;
        return 1;
    }
    if (channels != 1 && channels != 2) {
        std::cerr << "Channel count must be 1 or 2" << std::endl;
        return 1;
    }

    spdlog::set_level(spdlog::level::trace);

    StringPool string_pool;

    // 高速等时端点：每 microframe 一个包，端点大小按最高采样率计算 = max_rate*2*ch/8000
    auto max_rate = *std::max_element(rates.begin(), rates.end());
    auto iso_packet_size = static_cast<std::uint16_t>(max_rate * 2 * channels / 8000);

    std::vector<UsbInterface> interfaces = {
            // Interface 0: AudioControl（无端点）
            UsbInterface{
                    .interface_class = CC_AUDIO,
                    .interface_subclass = SC_AUDIOCONTROL,
                    .interface_protocol = 0x00,
                    .endpoints = {{}},
            },
            // Interface 1: AudioStreaming（alt 0 空端点，alt 1 ISO IN 端点）
            UsbInterface{
                    .interface_class = CC_AUDIO,
                    .interface_subclass = SC_AUDIOSTREAMING,
                    .interface_protocol = 0x00,
                    .endpoints = {{}, // alt 0: zero bandwidth
                                  {UsbEndpoint{
                                          .address = 0x81, // IN, endpoint 1
                                          .attributes = static_cast<std::uint8_t>(EndpointAttributes::Isochronous) |
                                                        static_cast<std::uint8_t>(IsoSyncType::Async),
                                          .max_packet_size = iso_packet_size,
                                          .interval = 1,
                                  }}},
            },
    };

    auto device = std::make_shared<UsbDevice>(UsbDevice{
            .path = "/usbipdcpp/mock_audio",
            .busid = busid,
            .bus_num = 1,
            .dev_num = 1,
            .speed = static_cast<std::uint32_t>(UsbSpeed::High),
            .vendor_id = 0x1234,
            .product_id = 0x5682,
            .device_bcd = 0x0100,
            .device_class = 0x00, // 单功能设备，接口各自定义（无需 IAD）
            .device_subclass = 0x00,
            .device_protocol = 0x00,
            .configuration_value = 1,
            .num_configurations = 1,
            .interfaces = interfaces,
            .ep0_in = UsbEndpoint::get_ep0_in(UsbSpeed::High),
            .ep0_out = UsbEndpoint::get_ep0_out(UsbSpeed::High),
    });

    // UacDeviceHelper 创建 AC/AS handler 并注册 + 设置描述符
    auto source = std::make_unique<SineWaveSource>(static_cast<std::uint32_t>(freq), rates,
                                                   static_cast<std::uint16_t>(channels), amp / 100.0);
    UacDeviceHelper::setup(device, string_pool, std::move(source));

    Server server;
    server.add_device(std::move(device));

    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), port};

    server.start(endpoint);

    std::string rates_str;
    for (auto r: rates)
        rates_str += std::to_string(r) + " ";
    SPDLOG_INFO("Mock UAC microphone started on port {}, busid {}, {}Hz sine, {}ch, rates [{}]", port, busid, freq,
                channels, rates_str);
    SPDLOG_INFO("Connect: usbip attach -r <host> -b {}", busid);
    SPDLOG_INFO("Press Enter to stop...");

    std::cin.get();

    server.stop();
    return 0;
}
