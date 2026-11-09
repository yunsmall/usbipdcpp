#include <iostream>

#include "../example_utils.h"

#include "usbipdcpp/virtual_device/devices/MscBulkOnlyHandler.h"
#include "usbipdcpp/virtual_device/storage_backends/RawImageBackend.h"
#include "usbipdcpp/virtual_device/SimpleVirtualDeviceHandler.h"
#include "usbipdcpp/usbipdcpp_core.h"

using namespace usbipdcpp;

int main(int argc, char **argv) {
    auto opts = make_example_options("mock_msc", "USB/IP virtual USB flash drive");
    opts.add_options()
            ("i,image", "Disk image path", cxxopts::value<std::string>()->default_value("disk.img"))
            ("super", "Export as SuperSpeed (USB 3.0)");
    auto result = parse_example_args(opts, argc, argv);
    auto port = result["port"].as<std::uint16_t>();
    auto busid = result["busid"].as<std::string>();
    auto image_path = result["image"].as<std::string>();
    auto speed = result.count("super") ? UsbSpeed::Super : UsbSpeed::High;

    spdlog::set_level(spdlog::level::trace);

    auto backend = std::unique_ptr<StorageBackend>(std::make_unique<RawImageBackend>(image_path, 4096));
    auto device = UsbDevice::make(busid, 0x1234, 0x5681,
                                  {MscBulkOnlyHandler::make_interface(0x81, 0x02)},
                                  1, 1, 0, "/usbipdcpp/mock_msc", speed);  // 磁盘是高速设备，EP0 按所配速度生成
    device->interfaces[0].with_handler<MscBulkOnlyHandler>(std::move(backend));
    device->with_handler<SimpleVirtualDeviceHandler>()->setup_interface_handlers();

    Server server;
    server.add_device(std::move(device));

    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), port};

    if (auto ec = server.start(endpoint); ec) {
        SPDLOG_ERROR("服务器启动失败：{}", ec.message());
        return 1;
    }

    SPDLOG_INFO("Mock MSC (USB Flash Drive) started on port {}, busid {}", port, busid);
    SPDLOG_INFO("Image: {}", image_path);
    SPDLOG_INFO("Connect: usbip attach -r <host> -b {}", busid);
    SPDLOG_INFO("Running... (Ctrl+C / SIGTERM / Enter to stop)");

    wait_for_exit();

    SPDLOG_INFO("Stopping...");
    server.stop();
    return 0;
}
