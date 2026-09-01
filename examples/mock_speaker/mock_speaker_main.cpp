#include <algorithm>
#include <iostream>
#include <memory>
#include <sstream>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN // 阻止 windows.h 拉入 winsock.h（项目用 winsock2，顺序冲突）
#define NOMINMAX // 阻止 windows.h 定义 min/max 宏（项目用 std::min/std::max）
#include <windows.h>
#include <mmsystem.h>
// MSVC/clang-cl 链接 winmm（timeBeginPeriod）
#pragma comment(lib, "winmm.lib")
#endif

#include "../example_utils.h"
#include "usbipdcpp/Device.h"
#include "usbipdcpp/Server.h"
#include "usbipdcpp/usbipdcpp_core.h"
#include "usbipdcpp/virtual_device/UacConstants.h"
#include "usbipdcpp/virtual_device/UacVirtualInterfaceHandler.h"
#include "usbipdcpp/virtual_device/audio_sinks/WavFileSink.h"
#ifdef USBIPDCPP_USE_MINIAUDIO
#include "PlaybackSink.h"
#endif

using namespace usbipdcpp;

/// 解析逗号分隔的采样率列表，如 "48000,44100,16000"
static std::vector<std::uint32_t> parse_sample_rates(const std::string &str) {
    std::vector<std::uint32_t> rates;
    std::stringstream ss(str);
    std::string item;
    while (std::getline(ss, item, ',')) {
        auto rate = std::stoul(item);
        if (rate > 0) {
            rates.push_back(static_cast<std::uint32_t>(rate));
        }
    }
    return rates;
}

int main(int argc, char **argv) {
#ifdef _WIN32
    // Windows 定时器默认粒度 ~15.6ms，asio 的亚毫秒 deadline 无法精确触发。
    // 进程级请求 1ms 粒度（系统取所有进程的最小值），TransferScheduler 的
    // 帧对齐/数据时长延迟才能按计划触发（播放延迟、闭环收敛都依赖它）
    timeBeginPeriod(1);
#endif
    auto opts = make_example_options("mock_speaker", "USB/IP virtual UAC speaker");
    auto options_group = opts.add_options();
    options_group
        ("rates", "Comma-separated sample rate list in Hz (first is initial)",
         cxxopts::value<std::string>()->default_value("48000"))
        ("channels", "Channel count (1 or 2)", cxxopts::value<int>()->default_value("1"))
        ("output", "Write received PCM to a WAV file instead of playing",
         cxxopts::value<std::string>());
#ifdef USBIPDCPP_USE_MINIAUDIO
    options_group("device", "Playback device name (miniaudio; empty = system default)",
                  cxxopts::value<std::string>()->default_value(""));
#endif
    auto result = parse_example_args(opts, argc, argv);
    auto port = result["port"].as<std::uint16_t>();
    auto busid = result["busid"].as<std::string>();
    auto channels = result["channels"].as<int>();
    auto has_output = result.count("output") > 0;
#ifdef USBIPDCPP_USE_MINIAUDIO
    auto device_name = result["device"].as<std::string>();
#endif

    // 列表参数解析（stoul 对非法输入抛异常，转成友好报错退出）
    std::vector<std::uint32_t> rates;
    try {
        rates = parse_sample_rates(result["rates"].as<std::string>());
    } catch (const std::exception &e) {
        std::cerr << "Failed to parse rates: " << e.what() << std::endl;
        return 1;
    }

    if (rates.empty()) {
        std::cerr << "Sample rate list must contain at least one positive rate" << std::endl;
        return 1;
    }
    if (channels != 1 && channels != 2) {
        std::cerr << "Channel count must be 1 or 2" << std::endl;
        return 1;
    }

    spdlog::set_level(spdlog::level::trace);

    StringPool string_pool;

    // 音频汇：--output 指定文件时写入 WAV 文件，否则本机播放（miniaudio）；
    // 无 miniaudio 且未指定文件时退化为丢弃计数（软失败，设备仍可枚举）
    std::unique_ptr<AudioSink> sink;
    if (has_output) {
        sink = std::make_unique<WavFileSink>(result["output"].as<std::string>());
    }
#ifdef USBIPDCPP_USE_MINIAUDIO
    else {
        sink = std::make_unique<PlaybackSink>(device_name);
    }
#else
    else {
        struct DiscardSink : public AudioSink {
            std::vector<AudioFormatInfo> supported_formats() const override {
                static const std::uint32_t rates_list[] = {8000, 16000, 22050, 32000, 44100, 48000, 96000};
                std::vector<AudioFormatInfo> fmts;
                for (auto rate: rates_list) {
                    fmts.push_back({1, 16, rate});
                    fmts.push_back({2, 16, rate});
                }
                return fmts;
            }
            AudioFormatInfo current_format() const override { return format; }
            bool set_format(std::uint16_t ch, std::uint8_t bits, std::uint32_t rate) override {
                if (bits != 16 || (ch != 1 && ch != 2)) return false;
                format = {ch, bits, rate};
                return true;
            }
            void write_pcm(const std::uint8_t *, std::size_t size) override {
                received += size;
                auto mb = received / (1024 * 1024);
                if (mb > last_reported_mb) {
                    last_reported_mb = mb;
                    SPDLOG_INFO("DiscardSink: 已接收 {} MB", mb);
                }
            }
            AudioFormatInfo format{1, 16, 48000};
            std::uint64_t received = 0;
            std::uint32_t last_reported_mb = 0;
        };
        SPDLOG_WARN("miniaudio 不可用且未指定 --output，进入丢弃模式（仅统计接收字节）");
        sink = std::make_unique<DiscardSink>();
    }
#endif

    // 高速等时端点：bInterval=4 → 每 1ms 一个包（对齐内核 gadget f_uac1.c 的
    // as_out_ep_desc），OUT 方向同步类型 Adaptive（USB 2.0 Table 9-13，与麦克风
    // IN 的 Async 不同）。端点大小按最高采样率帧对齐预留（同 mock_audio）
    auto fmts = sink->supported_formats();
    auto max_rate = std::max_element(fmts.begin(), fmts.end(),
                                     [](const AudioFormatInfo &a, const AudioFormatInfo &b) {
                                         return a.sample_rate < b.sample_rate;
                                     })->sample_rate;
    auto iso_packet_size = static_cast<std::uint16_t>(((max_rate + 999) / 1000) * 2 * channels);

    std::vector<UsbInterface> interfaces = {
            // Interface 0: AudioControl（含中断端点用于状态通知，
            // 对齐内核 gadget f_uac1.c 的 ac_int_ep_desc：wMaxPacketSize=2、bInterval=4）
            UsbInterface{
                    .interface_class = CC_AUDIO,
                    .interface_subclass = SC_AUDIOCONTROL,
                    .interface_protocol = 0x00,
                    .endpoints = {{UsbEndpoint{
                            .address = 0x82, // IN, endpoint 2 — interrupt for status
                            .attributes = 0x03, // Interrupt
                            .max_packet_size = 2, // UAC1 状态字（bStatusType + bOriginator）
                            .interval = 4,
                    }}},
            },
            // Interface 1: AudioStreaming（alt 0 空端点，alt 1 ISO OUT 端点）
            UsbInterface{
                    .interface_class = CC_AUDIO,
                    .interface_subclass = SC_AUDIOSTREAMING,
                    .interface_protocol = 0x00,
                    .endpoints = {{}, // alt 0: zero bandwidth
                                  {UsbEndpoint{
                                          .address = 0x01, // OUT, endpoint 1
                                          .attributes = static_cast<std::uint8_t>(EndpointAttributes::Isochronous) |
                                                        static_cast<std::uint8_t>(IsoSyncType::Adaptive),
                                          .max_packet_size = iso_packet_size,
                                          .interval = 4, // 每 1ms 一包（对齐 gadget f_uac1.c）
                                  }}},
            },
    };

    auto device = UsbDevice::make(busid, 0x1234, 0x5683, std::move(interfaces), 1, 1, 0,
                                  "/usbipdcpp/mock_speaker", UsbSpeed::High, 0x0100);
    // 接口从 0 连续编号：按下标依次填 interface_number（bInterfaceNumber 依赖它）
    device->assign_interface_numbers();

    // UacDeviceHelper 创建 AC/AS 汇 handler 并注册 + 设置描述符
    UacDeviceConfig config{.channels = static_cast<std::uint8_t>(channels)};
    if (auto ec = UacDeviceHelper::setup_speaker(device, 0, string_pool, std::move(sink), config); ec) {
        SPDLOG_ERROR("UAC 功能装配失败：{}", ec.message());
        return 1;
    }

    Server server;
    server.add_device(std::move(device));

    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), port};

    if (auto ec = server.start(endpoint); ec) {
        SPDLOG_ERROR("服务器启动失败：{}", ec.message());
        return 1;
    }

    std::string rates_str;
    for (auto r: rates)
        rates_str += std::to_string(r) + " ";
    SPDLOG_INFO("Mock UAC speaker started on port {}, busid {}, {}ch, rates [{}]", port, busid, channels, rates_str);
    SPDLOG_INFO("Connect: usbip attach -r <host> -b {}", busid);
    SPDLOG_INFO("Running... (Ctrl+C / SIGTERM / Enter to stop)");

    wait_for_exit();

    server.stop();
    return 0;
}
