#include <algorithm>
#include <iostream>
#include <memory>
#include <sstream>

#include "../example_utils.h"
#include "usbipdcpp/Device.h"
#include "usbipdcpp/Server.h"
#include "usbipdcpp/usbipdcpp_core.h"
#include "usbipdcpp/virtual_device/UacConstants.h"
#include "usbipdcpp/virtual_device/UacVirtualInterfaceHandler.h"
#include "usbipdcpp/virtual_device/audio_sources/FourierSource.h"
#include "usbipdcpp/virtual_device/audio_sources/SineWaveSource.h"
#ifdef USBIPDCPP_USE_MINIAUDIO
#include "AudioFileSource.h"
#endif

using namespace usbipdcpp;

/// 解析逗号分隔的采样率列表，如 "48000,16000,8000"
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

/// 解析谐波列表，如 "440:50,880:25:1.5708"（频率Hz:幅度百分比[:相位弧度]，相位可省略
/// 默认 0；幅度可为负值表示反相，即相位翻转 π）。
/// 合成公式：y(t) = Σ A_k·sin(2π·f_k·t + φ_k)
/// 方波前 3 谐波示例（440Hz，原始傅里叶系数 4/(kπ)，k 为奇数）：
/// "440:127.324,1320:42.441,2200:25.465"——叠加峰值超满幅时默认整体除以峰值
/// 不削波（波形形状不变），--clamp 改为直接削波
static std::vector<FourierHarmonic> parse_harmonics(const std::string &str) {
    std::vector<FourierHarmonic> result;
    std::stringstream ss(str);
    std::string item;
    while (std::getline(ss, item, ',')) {
        std::stringstream is(item);
        std::string freq_str, amp_str, phase_str;
        if (!std::getline(is, freq_str, ':') || !std::getline(is, amp_str, ':')) {
            continue;
        }
        FourierHarmonic h;
        h.frequency = static_cast<std::uint32_t>(std::stoul(freq_str));
        h.amplitude = std::stod(amp_str) / 100.0;
        if (std::getline(is, phase_str)) {
            h.phase = std::stod(phase_str);
        }
        result.push_back(h);
    }
    return result;
}

/// 创建音频源：--audio 文件源（文件打不开时软失败输出静音，同 FfmpegSource 做法）、
/// --harmonics 傅里叶级数合成、否则正弦波
static std::unique_ptr<AudioSource> create_source(const cxxopts::ParseResult &result,
                                                  const std::vector<std::uint32_t> &rates,
                                                  const std::vector<FourierHarmonic> &harmonics,
                                                  int channels, int freq, int amp, bool normalize) {
#ifdef USBIPDCPP_USE_MINIAUDIO
    if (result.count("audio") > 0) {
        return std::make_unique<AudioFileSource>(result["audio"].as<std::string>(), rates);
    }
#endif
    if (!harmonics.empty()) {
        return std::make_unique<FourierSource>(harmonics, rates, static_cast<std::uint16_t>(channels), normalize);
    }
    return std::make_unique<SineWaveSource>(static_cast<std::uint32_t>(freq), rates,
                                            static_cast<std::uint16_t>(channels), amp / 100.0);
}

int main(int argc, char **argv) {
    auto opts = make_example_options("mock_audio", "USB/IP virtual UAC microphone");
    auto options_group = opts.add_options();
    options_group
        ("freq", "Sine frequency in Hz (integer)", cxxopts::value<int>()->default_value("440"))
        ("rates", "Comma-separated sample rate list in Hz (first is initial)",
         cxxopts::value<std::string>()->default_value("48000"))
        ("channels", "Channel count (1 or 2)", cxxopts::value<int>()->default_value("1"))
        ("amp", "Amplitude 0-100 (percent of full scale)", cxxopts::value<int>()->default_value("50"))
        ("harmonics",
         "Fourier series synthesis: y(t)=sum(A_k*sin(2*pi*f_k*t+phi_k)); harmonics "
         "\"freq:amp[:phase],...\" (freq integer Hz, amp percent of full scale, may be "
         "negative for phase inversion, phase radians default 0; overrides sine). "
         "Square wave example at 440Hz, first 3 harmonics (raw coeffs 4/(k*pi)): "
         "\"440:127.324,1320:42.441,2200:25.465\"",
         cxxopts::value<std::string>())
        ("clamp", "Clip over-limit samples to full scale instead of scaling the whole "
         "wave down by the peak (default: scale only when peak exceeds full scale)",
         cxxopts::value<bool>()->default_value("false")->implicit_value("true"));
#ifdef USBIPDCPP_USE_MINIAUDIO
    // 仅 miniaudio 可用时注册 --audio：不可用的构建里 -h 不显示该选项，
    // 直接传也会被 cxxopts 按未知选项拒绝（parse_example_args 打印 help 退出）
    options_group("audio", "Audio file to play instead of sine (WAV/MP3/FLAC/OGG)", cxxopts::value<std::string>());
#endif
    auto result = parse_example_args(opts, argc, argv);
    auto port = result["port"].as<std::uint16_t>();
    auto busid = result["busid"].as<std::string>();
    auto freq = result["freq"].as<int>();
    auto channels = result["channels"].as<int>();
    auto amp = result["amp"].as<int>();
    auto clamp = result["clamp"].as<bool>();
#ifdef USBIPDCPP_USE_MINIAUDIO
    auto has_audio = result.count("audio") > 0;
#else
    auto has_audio = false; // --audio 未注册（无 miniaudio 构建）
#endif
    auto has_harmonics = result.count("harmonics") > 0;

    // 列表参数解析（stoul/stod 对非法输入抛异常，转成友好报错退出）
    std::vector<std::uint32_t> rates;
    std::vector<FourierHarmonic> harmonics;
    try {
        rates = parse_sample_rates(result["rates"].as<std::string>());
        if (has_harmonics) {
            harmonics = parse_harmonics(result["harmonics"].as<std::string>());
        }
    } catch (const std::exception &e) {
        std::cerr << "Failed to parse rates/harmonics: " << e.what() << std::endl;
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
    if (has_harmonics && harmonics.empty()) {
        std::cerr << "Harmonics list is empty or malformed, use \"freq:amp[:phase],...\"" << std::endl;
        return 1;
    }

    spdlog::set_level(spdlog::level::trace);

    StringPool string_pool;

    // 音频源：--audio 提供时用文件音源（声道数由文件决定，--channels 忽略）；
    // --harmonics 提供时用傅里叶级数合成；否则正弦波。
    // 文件打不开时 AudioFileSource 软失败（日志报错 + 输出静音），设备仍可枚举（同 FfmpegSource 做法）
    auto source = create_source(result, rates, harmonics, channels, freq, amp, !clamp);
    // 声道数以音源实际为准
    channels = source->current_format().channels;

    // 高速等时端点：bInterval=4 → 每 1ms 一个包（对齐内核 gadget f_uac1.c 的 as_in_ep_desc，
    // 真实 UAC 设备形态），端点大小按最高采样率帧对齐预留。
    // 包调度（对齐内核 gadget u_audio.c）为"基准包长 + 残差溢出补一帧"，溢出包比基准大一帧，
    // 所以 wMaxPacketSize 必须按帧向上取整（帧大小×ceil(max_rate/1000)）：非 1kHz 整数倍采样率
    // （如 44100 单声道 = 88.2 字节/ms → 88/90 字节包交替）下按字节取整会截断溢出包切碎帧。
    // 必须用音源实际支持格式的最大采样率：软失败降级格式（48000）可能不在命令行 rates 里，
    // 端点按命令行算会偏小，等时数据被截断
    auto fmts = source->supported_formats();
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
                                          .interval = 4, // 每 1ms 一包（对齐 gadget f_uac1.c）
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
    UacDeviceHelper::setup(device, string_pool, std::move(source));

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
    auto mode_str = has_audio ? std::string("audio file")
                              : (has_harmonics ? std::string("fourier harmonics")
                                               : std::to_string(freq) + "Hz sine");
    SPDLOG_INFO("Mock UAC microphone started on port {}, busid {}, {}, {}ch, rates [{}]", port, busid, mode_str,
                channels, rates_str);
    SPDLOG_INFO("Connect: usbip attach -r <host> -b {}", busid);
    SPDLOG_INFO("Press Enter to stop...");

    std::cin.get();

    server.stop();
    return 0;
}
