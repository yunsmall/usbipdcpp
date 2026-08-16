#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>

#include "test_utils.h"

#include "usbipdcpp/Device.h"
#include "usbipdcpp/Server.h"
#include "usbipdcpp/network.h"
#include "usbipdcpp/protocol.h"
#include "usbipdcpp/utils/StringPool.h"
#include "usbipdcpp/virtual_device/SimpleVirtualDeviceHandler.h"
#include "usbipdcpp/virtual_device/devices/KeyboardHandler.h"

using namespace usbipdcpp;
using namespace usbipdcpp::test;

namespace {
// 构造一个虚拟键盘设备（与 examples/mock_keyboard 相同的构造方式）。
// string_pool 必须由调用方持有且生命周期长于设备（handler 内部保存其引用）
std::shared_ptr<UsbDevice> make_mock_keyboard(StringPool &string_pool) {
    std::vector<UsbInterface> interfaces = {
            UsbInterface{
                    .interface_class = static_cast<std::uint8_t>(ClassCode::HID),
                    .interface_subclass = 0x01, // Boot Interface Subclass
                    .interface_protocol = 0x01, // Keyboard
                    .endpoints = {{
                            UsbEndpoint{
                                    .address = 0x81, // IN
                                    .attributes = 0x03,
                                    .max_packet_size = 16,
                                    .interval = 10,
                            },
                    }},
            },
    };
    interfaces[0].with_handler<KeyboardHandler>(string_pool);

    auto mock_keyboard = std::make_shared<UsbDevice>(UsbDevice{
            .path = "/test/mock_keyboard",
            .busid = "1-1",
            .bus_num = 1,
            .dev_num = 1,
            .speed = static_cast<std::uint32_t>(UsbSpeed::Full),
            .vendor_id = 0x1234,
            .product_id = 0x5678,
            .device_bcd = 0xABCD,
            .device_class = 0x00,
            .device_subclass = 0x00,
            .device_protocol = 0x00,
            .configuration_value = 1,
            .num_configurations = 1,
            .interfaces = interfaces,
            .ep0_in = UsbEndpoint::get_ep0_in(UsbSpeed::Full),
            .ep0_out = UsbEndpoint::get_ep0_out(UsbSpeed::Full),
    });
    auto device_handler = mock_keyboard->with_handler<SimpleVirtualDeviceHandler>(string_pool);
    device_handler->setup_interface_handlers();
    return mock_keyboard;
}

// 探测一个空闲端口，固定端口才能验证多次 start 的监听不冲突
std::uint16_t probe_free_port(asio::io_context &io) {
    asio::ip::tcp::acceptor probe(io);
    probe.open(asio::ip::tcp::v4());
    probe.bind(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0));
    return probe.local_endpoint().port();
}
} // namespace

TEST(TestNetworkVdev, ServerCanStopWithImportedDevice) {
    // stop() 时 session 正处于传输状态（import 设备后的 receiver/sender 双线程）：
    // immediately_stop 要能打断 receiver，设备要能移回可用列表，计数归零后
    // stop() 才返回，保证之后析构 Server 时没有线程还在访问它
    asio::io_context io;
    const std::uint16_t port = probe_free_port(io);

    // string_pool 必须先于 server 声明（后于 server 析构），handler 保存其引用
    StringPool string_pool;
    usbipdcpp::Server server;
    server.add_device(make_mock_keyboard(string_pool));
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), port);

    for (int round = 0; round < 2; round++) {
        server.start(ep);

        // 连接服务器，轮询等待监听就绪
        asio::ip::tcp::socket client(io);
        bool connected = false;
        for (int i = 0; i < 200; i++) {
            std::error_code ec;
            client.connect(ep, ec);
            if (!ec) {
                connected = true;
                break;
            }
            client.close();
            client = asio::ip::tcp::socket(io);
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        ASSERT_TRUE(connected);

        // 发送 import 请求，让 session 进入传输状态
        UsbIpCommand::OpReqImport req{.status = 0, .busid = {}};
        const std::string busid = "1-1";
        std::copy(busid.begin(), busid.end(), req.busid.begin());
        usbipdcpp::error_code send_ec;
        req.to_socket(client, send_ec);
        ASSERT_FALSE(send_ec);

        // 读 OpRepImport 响应头（version + command + status），确认导入成功
        std::uint16_t version = 0;
        std::uint16_t command = 0;
        std::uint32_t status = 0;
        data_read_from_socket(client, version, command, status);
        ASSERT_EQ(command, OP_REP_IMPORT);
        ASSERT_EQ(status, 0u);

        // 不关客户端，直接 stop()：immediately_stop 要打断传输中的 session
        server.stop();
        client.close();
    }
}
