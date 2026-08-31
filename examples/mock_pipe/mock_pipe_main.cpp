#include <atomic>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>

#include "../example_utils.h"
#include "usbipdcpp/Server.h"
#include "usbipdcpp/virtual_device/PipeDeviceHandler.h"

using namespace usbipdcpp;

int main(int argc, char **argv) {
    auto opts = make_example_options("mock_pipe", "USB/IP virtual generic pipe device (read/write)");
    auto result = parse_example_args(opts, argc, argv);
    auto port = result["port"].as<std::uint16_t>();
    auto busid = result["busid"].as<std::string>();

    spdlog::set_level(spdlog::level::info);

    StringPool string_pool;

    // vendor 类接口：bulk IN + bulk OUT（通用管道，无类特定协议）
    std::vector<UsbInterface> interfaces = {
            UsbInterface{
                    .interface_class = 0xFF, // vendor specific
                    .interface_subclass = 0x00,
                    .interface_protocol = 0x00,
                    .endpoints = {{
                            UsbEndpoint{
                                    .address = 0x81, // IN
                                    .attributes = 0x02, // Bulk
                                    .max_packet_size = 64,
                                    .interval = 0,
                            },
                            UsbEndpoint{
                                    .address = 0x02, // OUT
                                    .attributes = 0x02, // Bulk
                                    .max_packet_size = 64,
                                    .interval = 0,
                            },
                    }},
            },
    };

    // 管道设备没有接口 handler（PipeDeviceHandler 自动管道化所有端点），接口手动定义
    auto device = UsbDevice::make(busid, 0x1234, 0x5690, interfaces, 1, 1, 0, "/usbipdcpp/mock_pipe");
    auto pipe = device->with_handler<PipeDeviceHandler>(string_pool);

    // 标准请求行为配置（可选，默认行为：接受存在的 alt、其余回错误）：
    // 演示拒绝接口级 SET_FEATURE——本设备（vendor 管道）不支持任何接口级
    // feature，回错误比假装成功清晰。必须在连接前设置
    PipeStandardRequestHandler req_handler;
    req_handler.set_feature = [](PipeDeviceHandler &pipe, std::uint16_t feature_selector,
                                 std::uint32_t *p_status) {
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
    };
    pipe->set_standard_request_handler(std::move(req_handler));

    pipe->setup_interface_handlers();

    Server server;
    server.add_device(std::move(device));

    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), port};
    if (auto ec = server.start(endpoint); ec) {
        SPDLOG_ERROR("服务器启动失败：{}", ec.message());
        return 1;
    }

    SPDLOG_INFO("Mock pipe device started on port {}, busid {}", port, busid);
    SPDLOG_INFO("Connect with: usbip attach -r <host> -b {}", busid);
    SPDLOG_INFO("Running... (Ctrl+C / SIGTERM / Enter to stop)");

    // 业务线程使用短超时轮询：退出顺序必须是"业务线程先退出、再 server.stop()"，
    // 否则 handler 随会话析构时业务线程还阻塞在 read/write 上（use-after-free）。
    // 正常通信时 read/write 立即返回，超时轮询不影响使用
    std::atomic<bool> running{true};

    // 回显线程：收到的 OUT 数据原样发回（阻塞 read）
    std::thread echo_thread([&]() {
        PipeXfer xfer;
        while (running) {
            if (pipe->read(xfer, 200)) {
                SPDLOG_INFO("收到 {} 字节（ep {:02x}），回显", xfer.data.size(), xfer.ep);
                pipe->write(PipeXfer{.ep = 0x81, .data = std::move(xfer.data)}, 200);
            }
        }
    });

    // 周期发送线程：每 2 秒发送一条计数消息（演示阻塞 write）。
    // 打日志供 e2e 脚本断言传输活动（主机无 vendor 驱动，数据面靠 python 验证，
    // 这里只确认发送线程活着）
    std::thread sender_thread([&]() {
        std::uint32_t count = 0;
        while (running) {
            auto msg = "message " + std::to_string(count++) + "\r\n";
            SPDLOG_INFO("周期发送 {} 字节：{}", msg.size(), msg);
            pipe->write(PipeXfer{.ep = 0x81, .data = data_type(msg.begin(), msg.end())}, 200);
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    });

    wait_for_exit();

    running = false;
    echo_thread.join();
    sender_thread.join();
    server.stop();
    return 0;
}
