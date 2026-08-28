#include <iostream>

#include "../example_utils.h"
#include "mock_cdc_throttle.h"
#include "usbipdcpp/usbipdcpp_core.h"
#include "usbipdcpp/virtual_device/SimpleVirtualDeviceHandler.h"

using namespace usbipdcpp;

/**
 * 限流串口示例：固定窗口内只能接收固定字节数，超出后设备暂停接收（OUT NAK
 * 背压），一段时间后自动恢复。演示 CdcAcmDataInterfaceHandler 的 out_channel
 * 挂起机制（on_data_received 返回 false → 请求挂起 → 主机 NAK 停发）。
 *
 * 用法：
 *   ./mock_cdc_throttle --limit-bytes 128 --window-sec 3
 * 主机端每 3 秒窗口内最多能写 128 字节；写多了设备 NAK 3 秒，之后恢复。
 */
int main(int argc, char **argv) {
    auto opts = make_example_options("mock_cdc_throttle", "USB/IP virtual serial port with OUT NAK throttling");
    opts.add_options()("l,limit-bytes", "每窗口接收字节上限（达到后本窗口暂停接收）",
                       cxxopts::value<std::uint32_t>()->default_value("64"))
            ("w,window-sec", "窗口时长（秒）：收满限流后暂停接收这么久再恢复",
             cxxopts::value<std::uint32_t>()->default_value("5"));
    auto result = parse_example_args(opts, argc, argv);

    const auto port = result["port"].as<std::uint16_t>();
    const auto busid = result["busid"].as<std::string>();
    const auto limit_bytes = result["limit-bytes"].as<std::uint32_t>();
    const auto window_ms = result["window-sec"].as<std::uint32_t>() * 1000u;

    spdlog::set_level(spdlog::level::debug);

    StringPool string_pool;

    // CDC ACM 需要两个接口：通信接口和数据接口（与 mock_cdc_acm 一致的描述符）
    std::vector<UsbInterface> interfaces = {
            UsbInterface{.interface_class = 0x02,         // CDC Communication
                         .interface_subclass = 0x02,      // ACM
                         .interface_protocol = 0x01,      // AT Commands (v25ter)
                         .endpoints = {{UsbEndpoint{.address = 0x83,  // Interrupt IN
                                                    .attributes = 0x03,
                                                    .max_packet_size = 64,
                                                    .interval = 16}}}},
            UsbInterface{.interface_class = 0x0A,         // CDC Data
                         .interface_subclass = 0x00,
                         .interface_protocol = 0x00,
                         .endpoints = {{UsbEndpoint{.address = 0x81,  // Bulk IN
                                                    .attributes = 0x02,
                                                    .max_packet_size = 64,
                                                    .interval = 0},
                                        UsbEndpoint{.address = 0x02,  // Bulk OUT
                                                    .attributes = 0x02,
                                                    .max_packet_size = 64,
                                                    .interval = 0}}}},
    };

    interfaces[0].with_handler<CdcAcmCommunicationInterfaceHandler>(string_pool);
    // 数据接口用限流 handler：窗口内最多收 limit_bytes 字节，超出后 NAK window_ms
    interfaces[1].with_handler<ThrottleCdcAcmDataInterfaceHandler>(string_pool, limit_bytes, window_ms);

    auto device = std::make_shared<UsbDevice>(UsbDevice{
            .path = "/usbipdcpp/mock_cdc_throttle",
            .busid = busid,
            .bus_num = 1,
            .dev_num = 1,
            .speed = static_cast<std::uint32_t>(UsbSpeed::Full),
            .vendor_id = 0x1234,
            .product_id = 0x5681,  // throttled CDC ACM device
            .device_bcd = 0x0100,
            .device_class = 0x02,  // CDC Communication (at device level for IAD)
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

    // 关联通信接口和数据接口处理器
    auto &comm_handler =
            *std::dynamic_pointer_cast<CdcAcmCommunicationInterfaceHandler>(device->interfaces[0].handler);
    auto &data_handler =
            *std::dynamic_pointer_cast<ThrottleCdcAcmDataInterfaceHandler>(device->interfaces[1].handler);
    comm_handler.set_data_handler(&data_handler);
    data_handler.set_comm_handler(&comm_handler);

    Server server;
    server.add_device(std::move(device));

    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), port};
    if (auto ec = server.start(endpoint); ec) {
        SPDLOG_ERROR("服务器启动失败：{}", ec.message());
        return 1;
    }

    SPDLOG_INFO("限流串口已启动：端口 {}，busid {}，每 {} ms 窗口最多接收 {} 字节",
                port, busid, window_ms, limit_bytes);
    SPDLOG_INFO("连接：usbip attach -r <主机> -b {}", busid);
    SPDLOG_INFO("设备统计收到的 '1' 字符数，每 {} ms 以「数字\\n」发回主机", window_ms / 2);
    SPDLOG_INFO("主机持续写入超过 {} 字节后，设备暂停接收 {} ms（OUT NAK），之后自动恢复",
                limit_bytes, window_ms);
    SPDLOG_INFO("Press Enter to exit...");

    std::cin.get();
    server.stop();
    return 0;
}