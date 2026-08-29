#include <atomic>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>

#include "../example_utils.h"
#include "usbipdcpp/Server.h"
#include "usbipdcpp/virtual_device/PipeDeviceHandler.h"

using namespace usbipdcpp;

// ===== HID 键盘（Boot 协议，无 Report ID）=====
// 标准 63 字节 Boot 键盘报告描述符：8 字节 IN 报告（modifier + 保留 + 6 键）
// + 1 字节 OUT 报告（LED）
static const data_type kKeyboardReportDescriptor = {
        0x05, 0x01, // Usage Page (Generic Desktop)
        0x09, 0x06, // Usage (Keyboard)
        0xA1, 0x01, // Collection (Application)
        0x05, 0x07, //   Usage Page (Key Codes)
        0x19, 0xE0, //   Usage Minimum (224 = Left Ctrl)
        0x29, 0xE7, //   Usage Maximum (231 = Right GUI)
        0x15, 0x00, //   Logical Minimum (0)
        0x25, 0x01, //   Logical Maximum (1)
        0x75, 0x01, //   Report Size (1)
        0x95, 0x08, //   Report Count (8)
        0x81, 0x02, //   Input (Data, Variable, Absolute) —— 8 个修饰键位
        0x95, 0x01, //   Report Count (1)
        0x75, 0x08, //   Report Size (8)
        0x81, 0x01, //   Input (Constant) —— 保留字节
        0x95, 0x05, //   Report Count (5)
        0x75, 0x01, //   Report Size (1)
        0x05, 0x08, //   Usage Page (LEDs)
        0x19, 0x01, //   Usage Minimum (1)
        0x29, 0x05, //   Usage Maximum (5)
        0x91, 0x02, //   Output (Data, Variable, Absolute) —— 5 个 LED
        0x95, 0x01, //   Report Count (1)
        0x75, 0x03, //   Report Size (3)
        0x91, 0x01, //   Output (Constant)
        0x95, 0x06, //   Report Count (6)
        0x75, 0x08, //   Report Size (8)
        0x15, 0x00, //   Logical Minimum (0)
        0x25, 0x65, //   Logical Maximum (101)
        0x05, 0x07, //   Usage Page (Key Codes)
        0x19, 0x00, //   Usage Minimum (0)
        0x29, 0x65, //   Usage Maximum (101)
        0x81, 0x00, //   Input (Data, Array) —— 6 个按键槽
        0xC0,       // End Collection
};

int main(int argc, char **argv) {
    auto opts = make_example_options("mock_pipe_hid", "USB/IP virtual HID keyboard via generic pipe (read/write)");
    auto result = parse_example_args(opts, argc, argv);
    auto port = result["port"].as<std::uint16_t>();
    auto busid = result["busid"].as<std::string>();

    spdlog::set_level(spdlog::level::info);

    StringPool string_pool;

    // HID Boot 键盘接口：中断 IN 端点（8 字节报告）
    std::vector<UsbInterface> interfaces = {
            UsbInterface{
                    .interface_class = 0x03, // HID
                    .interface_subclass = 0x01, // Boot Interface Subclass
                    .interface_protocol = 0x01, // Keyboard
                    .endpoints = {{
                            UsbEndpoint{
                                    .address = 0x81, // IN
                                    .attributes = 0x03, // Interrupt
                                    .max_packet_size = 8,
                                    .interval = 10,
                            },
                    }},
            },
    };

    // 管道设备没有接口 handler（PipeDeviceHandler 自动管道化所有端点），接口手动定义
    auto device = UsbDevice::make(busid, 0x1234, 0x5691, interfaces, 1, 1, 0, "/usbipdcpp/mock_pipe_hid");

    auto pipe = device->with_handler<PipeDeviceHandler>(string_pool);
    // HID 描述符（0x21）：追加在配置描述符的接口描述符之后，驱动加载必需
    pipe->set_class_specific_descriptor({
            0x09, // bLength
            0x21, // bDescriptorType: HID
            0x11,
            0x01, // bcdHID 1.11
            0x00, // bCountryCode
            0x01, // bNumDescriptors
            0x22, // bDescriptorType[0]: Report
            static_cast<std::uint8_t>(kKeyboardReportDescriptor.size()),
            static_cast<std::uint8_t>(kKeyboardReportDescriptor.size() >> 8), // wDescriptorLength[0]
    });
    // GET_DESCRIPTOR(0x22) 返回报告描述符
    pipe->set_custom_descriptor(0x22, kKeyboardReportDescriptor);
    pipe->setup_interface_handlers();

    Server server;
    server.add_device(std::move(device));

    asio::ip::tcp::endpoint endpoint{asio::ip::tcp::v4(), port};
    if (auto ec = server.start(endpoint); ec) {
        SPDLOG_ERROR("服务器启动失败：{}", ec.message());
        return 1;
    }

    SPDLOG_INFO("Mock pipe HID keyboard started on port {}, busid {}", port, busid);
    SPDLOG_INFO("Connect with: usbip attach -r <host> -b {}", busid);
    SPDLOG_INFO("Press Enter to exit...");

    // 业务线程使用短超时轮询：退出顺序必须是"业务线程先退出、再 server.stop()"，
    // 否则 handler 随会话析构时业务线程还阻塞在 read/write 上（use-after-free）
    std::atomic<bool> running{true};

    // read 线程：处理宿主发来的 OUT 数据与非标准控制请求（HID 的
    // GET_REPORT/SET_REPORT 等 class 请求也会经 read() 返回）
    std::thread read_thread([&]() {
        PipeXfer xfer;
        while (running) {
            if (!pipe->read(xfer, 200)) {
                continue;
            }
            if (xfer.setup_req.has_value()) {
                // 控制请求（ep==0）：class/vendor 请求。IN 方向需要应答
                SPDLOG_INFO("收到控制请求 req={:02x} type={:02x} length={}",
                            xfer.setup_req->request, xfer.setup_req->request_type, xfer.setup_req->length);
                if (!xfer.setup_req->is_out() && xfer.setup_req->length > 0) {
                    // 应答当前键盘报告（示例简化：GET_REPORT 返回空报告）
                    pipe->write(PipeXfer{.ep = 0, .data = data_type(xfer.setup_req->length, 0)}, 200);
                }
            }
            else if (!xfer.data.empty()) {
                // OUT 数据：HID 键盘没有 OUT 数据端点，收到即打印
                SPDLOG_INFO("收到 {} 字节（ep {:02x}），LED/OUT 报告：{:02x}",
                            xfer.data.size(), xfer.ep, xfer.data[0]);
            }
        }
    });

    // 发送线程：每 2 秒按下并释放一次 A 键（演示阻塞 write）
    std::thread sender_thread([&]() {
        while (running) {
            // 按下 A（USB HID 键码 0x04）
            const data_type pressed = {0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00};
            pipe->write(PipeXfer{.ep = 0x81, .data = pressed}, 200);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            // 释放全部按键
            const data_type released = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
            pipe->write(PipeXfer{.ep = 0x81, .data = released}, 200);
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    });

    std::cin.get();

    running = false;
    read_thread.join();
    sender_thread.join();
    server.stop();
    return 0;
}
