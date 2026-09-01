#include <atomic>
#include <charconv>
#include <chrono>
#include <iostream>
#include <thread>

#include "../example_utils.h"
#include "usbipdcpp/Server.h"
#include "usbipdcpp/utils/StringPool.h"
#include "usbipdcpp/virtual_device/RndisMsOsDeviceHandler.h"
#include "usbipdcpp/virtual_device/RndisVirtualInterfaceHandler.h"
#include "usbipdcpp/virtual_device/network_backends/EthernetEchoBackend.h"
// TunBackend 仅 Linux 内核提供 /dev/net/tun（含 Android/termux），Windows/macOS 不编译
#if defined(__linux__) || defined(__ANDROID__)
#include "usbipdcpp/virtual_device/network_backends/TunBackend.h"
#endif

using namespace usbipdcpp;

namespace {

/// 解析 "02:10:83:00:00:01" 形式 MAC 地址
std::array<std::uint8_t, 6> parse_mac(const std::string &s) {
    auto hex_val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::array<std::uint8_t, 6> mac{};
    if (s.size() != 17) {
        throw std::runtime_error("MAC 地址格式应为 02:10:83:00:00:01");
    }
    for (int i = 0; i < 6; i++) {
        int hi = hex_val(s[i * 3]);
        int lo = hex_val(s[i * 3 + 1]);
        if (hi < 0 || lo < 0 || (i < 5 && s[i * 3 + 2] != ':')) {
            throw std::runtime_error("MAC 地址格式应为 02:10:83:00:00:01");
        }
        mac[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return mac;
}

/// 解析 "192.168.53.1" 形式 IPv4 地址
std::array<std::uint8_t, 4> parse_ipv4(const std::string &s) {
    std::array<std::uint8_t, 4> ip{};
    std::size_t pos = 0;
    for (int i = 0; i < 4; i++) {
        int v = 0;
        auto [ptr, ec] = std::from_chars(s.data() + pos, s.data() + s.size(), v);
        if (ec != std::errc() || v < 0 || v > 255 || (i < 3 && (ptr == s.data() + s.size() || *ptr != '.'))) {
            throw std::runtime_error("IPv4 地址格式应为 192.168.53.1");
        }
        ip[i] = static_cast<std::uint8_t>(v);
        pos = static_cast<std::size_t>(ptr - s.data()) + (i < 3 ? 1 : 0);
    }
    if (pos != s.size()) {
        throw std::runtime_error("IPv4 地址格式应为 192.168.53.1");
    }
    return ip;
}

} // namespace

int main(int argc, char **argv) {
    auto opts = make_example_options("mock_rndis", "USB/IP virtual ethernet adapter (CDC RNDIS, Windows driverless)");
    opts.add_options()("mac", "Device MAC address", cxxopts::value<std::string>()->default_value("02:10:83:00:00:02"))
            ("ip", "Device-side IPv4 address", cxxopts::value<std::string>()->default_value("192.168.53.1"))
            ("tcp-port", "Device-side TCP echo port", cxxopts::value<std::uint16_t>()->default_value("5000"))
#if defined(__linux__) || defined(__ANDROID__)
            // Linux/Android 才有 /dev/net/tun（TunBackend），其他平台不注册该选项
            ("tun", "Use Linux TUN backend instead of user-space echo: route the virtual NIC into this host's kernel "
                    "network stack (needs root); value = interface name (e.g. usbip%d)",
             cxxopts::value<std::string>())
#endif
            ;
    auto result = parse_example_args(opts, argc, argv);
    auto port = result["port"].as<std::uint16_t>();
    auto busid = result["busid"].as<std::string>();
    auto mac = parse_mac(result["mac"].as<std::string>());
    auto ip = parse_ipv4(result["ip"].as<std::string>());
    auto tcp_port = result["tcp-port"].as<std::uint16_t>();

    spdlog::set_level(spdlog::level::trace);

    StringPool string_pool;

    // 后端：默认 echo 模式纯用户态应答 ARP/ICMP/TCP（无需 root）；--tun 模式把
    // 虚拟网卡接入本机内核协议栈（需 root，仅 Linux）。handler 持有其指针，
    // 须活得比设备久
    std::unique_ptr<NetworkBackend> backend;
#if defined(__linux__) || defined(__ANDROID__)
    if (result.count("tun")) {
        auto tun_name = result["tun"].as<std::string>();
        auto tun = std::make_unique<TunBackend>(tun_name, ip, std::array<std::uint8_t, 4>{255, 255, 255, 0});
        SPDLOG_INFO("TUN backend: interface '{}', device IP {}.{}.{}.{} (ARP/ICMP/TCP answered by kernel stack)",
                    tun->interface_name(), ip[0], ip[1], ip[2], ip[3]);
        backend = std::move(tun);
    }
    else
#endif
    {
        backend = std::make_unique<EthernetEchoBackend>(mac, ip, tcp_port);
    }

    // RNDIS 双接口：通信接口（中断 IN 通知 + ep0 封装命令）+ 数据接口（双 bulk，
    // 无 altsetting）。RNDIS 在 Windows 上免驱（"远程 NDIS 兼容设备"），
    // 这是与 ECM（Windows 需第三方驱动）的本质区别
    std::vector<UsbInterface> interfaces = {
            RndisCommunicationInterfaceHandler::make_interface(0x83),
            RndisDataInterfaceHandler::make_interface(0x81, 0x02),
    };

    // IAD 复合设备：设备描述符用 EF/04/01（Misc/CDC RNDIS）——Windows 的
    // rndismp 网卡驱动按 `USB\Class_EF&SubClass_04&Prot_01` 直接匹配（rndiscmp.inf，
    // 真实安卓 RNDIS 设备就是这个设备类）；bDeviceClass=0x02(CDC) 会被当单功能
    // CDC 设备直接丢给 usbser 绑成串口。PID 用 0x56E5：Windows 按 VID/PID 缓存
    // usbser 绑定，同一 PID 被绑过（0x56E2~0x56E4）后不再重新枚举——换全新
    // PID 才能走完整枚举流程
    auto device = UsbDevice::make(busid, 0x1234, 0x56E5, std::move(interfaces),
                                  1, 1, static_cast<std::uint8_t>(ClassCode::Misc), "/usbipdcpp/mock_rndis",
                                  UsbSpeed::Full, Version{1, 0, 0}, 0x04 /* CDC RNDIS 子类 */, 0x01);
    // 接口从 0 连续编号：按下标依次填 interface_number（多接口设备的 bInterfaceNumber 依赖它）
    device->assign_interface_numbers();
    auto comm = device->interfaces[0].with_handler<RndisCommunicationInterfaceHandler>(string_pool, mac,
                                                                                        UsbSpeed::Full);
    device->interfaces[1].with_handler<RndisDataInterfaceHandler>(string_pool, backend.get(), comm.get());
    // MS OS Compatible ID "RNDIS" + SubCompatibleID "5162001" 由
    // RndisMsOsDeviceHandler 预设（Windows rndismp 按 MS_COMP_RNDIS&
    // MS_SUBCOMP_5162001 匹配，见 rndiscmp.inf；空值匹配失败退回 usbser）
    auto dev_handler = device->with_handler<RndisMsOsDeviceHandler>(string_pool);
    dev_handler->setup_interface_handlers();

    Server server;
    server.add_device(std::move(device));

    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), port};

    if (auto ec = server.start(endpoint); ec) {
        SPDLOG_ERROR("服务器启动失败：{}", ec.message());
        return 1;
    }

    SPDLOG_INFO("Mock RNDIS (virtual ethernet adapter, driverless on Windows) started on port {}, busid {}", port,
                busid);
    SPDLOG_INFO("Device MAC: {:02X}:{:02X}:{:02X}:{:02X}:{:02X}:{:02X}, device IP: {}.{}.{}.{}", mac[0], mac[1],
                mac[2], mac[3], mac[4], mac[5], ip[0], ip[1], ip[2], ip[3]);
    if (result.count("tun")) {
        SPDLOG_INFO("Device-side services: provided by this host's kernel network stack (via TUN)");
    }
    else {
        SPDLOG_INFO("Device-side services: ping (ICMP echo), TCP echo on port {}", tcp_port);
    }
    SPDLOG_INFO("Connect with: usbip attach -r <host> -b {}", busid);
    SPDLOG_INFO("Then on host: ip addr add {}.{}.{}.2/24 dev usbX, ping {}.{}.{}.{}, nc {}.{}.{}.{} {}",
                ip[0], ip[1], ip[2], ip[0], ip[1], ip[2], ip[3], ip[0], ip[1], ip[2], ip[3], tcp_port);
    SPDLOG_INFO("Running... (Ctrl+C / SIGTERM / Enter to stop)");

    // 帧统计线程：每 5 秒打印一次收发统计
    std::atomic<bool> running{true};
    std::thread stat_thread([&]() {
        auto last_tx = backend->tx_frames();
        auto last_rx = backend->rx_frames();
        while (running) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            auto tx = backend->tx_frames();
            auto rx = backend->rx_frames();
            SPDLOG_INFO("Frames - rx: {} (+{}), tx: {} (+{})", rx, rx - last_rx, tx, tx - last_tx);
            last_tx = tx;
            last_rx = rx;
        }
    });

    wait_for_exit();

    running = false;
    stat_thread.join();
    server.stop();

    SPDLOG_INFO("Mock RNDIS stopped. Received {} frames ({} bytes), sent {} frames ({} bytes)", backend->rx_frames(),
                backend->rx_bytes(), backend->tx_frames(), backend->tx_bytes());
    return 0;
}
