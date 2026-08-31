#include <iostream>

#include "../example_utils.h"
#include "mock_cdc_acm.h"
#include "usbipdcpp/usbipdcpp_core.h"
#include "usbipdcpp/virtual_device/SimpleVirtualDeviceHandler.h"

using namespace usbipdcpp;

int main(int argc, char **argv) {
    auto opts = make_example_options("mock_cdc_acm", "USB/IP virtual serial port device");
    auto result = parse_example_args(opts, argc, argv);
    auto port = result["port"].as<std::uint16_t>();
    auto busid = result["busid"].as<std::string>();

    spdlog::set_level(spdlog::level::debug);

    StringPool string_pool;

    // CDC ACM 需要两个接口：通信接口和数据接口（make_interface 只返回描述符模板，
    // 设备创建后绑定示例内的 Mock handler）
    std::vector<UsbInterface> interfaces = {
            CdcAcmCommunicationInterfaceHandler::make_interface(0x83),
            CdcAcmDataInterfaceHandler::make_interface(0x81, 0x02),
    };

    // 创建设备：IAD 复合设备需在设备级声明 CDC 类
    auto mock_cdc_acm = UsbDevice::make(busid, 0x1234, 0x5680, std::move(interfaces),
                                        1, 1, static_cast<std::uint8_t>(ClassCode::CDC), "/usbipdcpp/mock_cdc_acm");
    // 设置接口处理器
    mock_cdc_acm->interfaces[0].with_handler<MockCdcAcmCommunicationInterfaceHandler>(string_pool);
    mock_cdc_acm->interfaces[1].with_handler<MockCdcAcmDataInterfaceHandler>(string_pool);
    mock_cdc_acm->with_handler<SimpleVirtualDeviceHandler>(string_pool)->setup_interface_handlers();

    // 关联通信接口和数据接口处理器
    auto &comm_handler =
            *std::dynamic_pointer_cast<MockCdcAcmCommunicationInterfaceHandler>(mock_cdc_acm->interfaces[0].handler);
    auto &data_handler =
            *std::dynamic_pointer_cast<MockCdcAcmDataInterfaceHandler>(mock_cdc_acm->interfaces[1].handler);
    comm_handler.set_data_handler(&data_handler);
    data_handler.set_comm_handler(&comm_handler);

    Server server;
    server.add_device(std::move(mock_cdc_acm));

    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), port};

    if (auto ec = server.start(endpoint); ec) {
        SPDLOG_ERROR("服务器启动失败：{}", ec.message());
        return 1;
    }

    SPDLOG_INFO("Mock CDC ACM (virtual serial port) started on port {}, busid {}", port, busid);
    SPDLOG_INFO("Connect with: usbip attach -r <host> -b {}", busid);
    SPDLOG_INFO("Then use: screen /dev/ttyACMx or minicom");
    SPDLOG_INFO("Running... (Ctrl+C / SIGTERM / Enter to stop)");

    wait_for_exit();

    SPDLOG_INFO("Stopping server...");
    server.stop();

    return 0;
}
