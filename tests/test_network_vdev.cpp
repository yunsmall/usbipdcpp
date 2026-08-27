#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
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

} // namespace

TEST(TestNetworkVdev, ServerCanStopWithImportedDevice) {
    // stop() 时 session 正处于传输状态（import 设备后的 receiver/sender 双线程）：
    // immediately_stop 要能打断 receiver，设备要能移回可用列表，所有 session
    // 线程 join 后 stop() 才返回，保证之后析构 Server 时没有线程还在访问它
    asio::io_context io;

    // string_pool 必须先于 server 声明（后于 server 析构），handler 保存其引用
    StringPool string_pool;
    usbipdcpp::Server server;
    server.add_device(make_mock_keyboard(string_pool));
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    for (int round = 0; round < 2; round++) {
        ASSERT_FALSE(server.start(ep));
        // 端口 0 启动，实际监听端点（含系统分配的端口）用 endpoint() 查询
        auto actual_ep = server.endpoint();

        // 连接服务器，轮询等待监听就绪
        asio::ip::tcp::socket client(io);
        bool connected = false;
        for (int i = 0; i < 200; i++) {
            std::error_code ec;
            client.connect(actual_ep, ec);
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

// ---------------------------------------------------------------------------
// 以下为客户端各种奇特连接/断连场景
// ---------------------------------------------------------------------------

namespace {
// 发送 import 请求并读取回复，返回回复中的 status；任何错误返回最大值
std::uint32_t import_device(asio::ip::tcp::socket &client, const std::string &busid) {
    UsbIpCommand::OpReqImport req{.status = 0, .busid = {}};
    std::copy(busid.begin(), busid.end(), req.busid.begin());
    usbipdcpp::error_code send_ec;
    req.to_socket(client, send_ec);
    if (send_ec) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    std::uint16_t version = 0;
    std::uint16_t command = 0;
    std::uint32_t status = 0;
    data_read_from_socket(client, version, command, status);
    if (command != OP_REP_IMPORT) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    return status;
}
} // namespace

TEST(TestNetworkVdev, ClientRstWithoutAnyData) {
    // 连接后不发任何数据直接 RST：session 挂在 parse_op 的阻塞读上，读要
    // 以连接重置错误返回（SOCKET_ERR 路径，区别于 FIN 的 EOF 路径），
    // session 干净退出，stop() 不卡
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    usbipdcpp::Server server;
    ASSERT_FALSE(server.start(ep));
    {
        asio::ip::tcp::socket client(io);
        ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
        rst_disconnect(client);
    }
    // session 线程要在客户端断开后自行清理
    ASSERT_TRUE(wait_sessions_gone(server));
    server.stop();
}

TEST(TestNetworkVdev, ClientSendsHalfOpThenDisconnects) {
    // 只发送 1 个字节就断开：服务器读 2 字节的 version 时卡在半个头上，
    // 随后 EOF。这是"半包断开"最极端的形态，session 要干净退出
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    usbipdcpp::Server server;
    ASSERT_FALSE(server.start(ep));
    {
        asio::ip::tcp::socket client(io);
        ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
        const std::array<std::uint8_t, 1> half_header = {0x01};
        asio::write(client, asio::buffer(half_header));
        client.close();
    }
    ASSERT_TRUE(wait_sessions_gone(server));
    server.stop();
}

TEST(TestNetworkVdev, ClientDisconnectsRightAfterImportRequest) {
    // 发出 import 请求后不等服务器回复就 RST：服务器可能正在打开设备或写
    // 回复时发现连接已断。session 要干净退出，设备不能卡在 using_devices——
    // 重启后重新 import 同一设备必须成功
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    StringPool string_pool;
    usbipdcpp::Server server;
    server.add_device(make_mock_keyboard(string_pool));

    ASSERT_FALSE(server.start(ep));
    {
        asio::ip::tcp::socket client(io);
        ASSERT_TRUE(connect_with_retry(client, server.endpoint()));

        UsbIpCommand::OpReqImport req{.status = 0, .busid = {}};
        const std::string busid = "1-1";
        std::copy(busid.begin(), busid.end(), req.busid.begin());
        usbipdcpp::error_code send_ec;
        req.to_socket(client, send_ec);
        ASSERT_FALSE(send_ec);

        rst_disconnect(client); // 不读回复直接 RST
    }
    ASSERT_TRUE(wait_sessions_gone(server));
    server.stop();

    // 重启后重新 import 同一设备：设备没卡在 using_devices 才能成功
    ASSERT_FALSE(server.start(ep));
    {
        asio::ip::tcp::socket client(io);
        ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
        ASSERT_EQ(import_device(client, "1-1"), 0u);
        client.close();
    }
    ASSERT_TRUE(wait_sessions_gone(server));
    server.stop();
}

TEST(TestNetworkVdev, ClientDisconnectsAfterSuccessfulImport) {
    // import 成功后客户端优雅断开（FIN）：receiver 读到 EOF 走完整清理路径
    // （on_disconnection → 设备移回可用列表）。3 轮循环验证设备可以反复
    // 导入-释放，没有泄漏在 using_devices 中
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    StringPool string_pool;
    usbipdcpp::Server server;
    server.add_device(make_mock_keyboard(string_pool));

    for (int round = 0; round < 3; round++) {
        ASSERT_FALSE(server.start(ep));
        {
            asio::ip::tcp::socket client(io);
            ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
            ASSERT_EQ(import_device(client, "1-1"), 0u);
            client.close(); // 传输中优雅断开
        }
        ASSERT_TRUE(wait_sessions_gone(server));
        server.stop();
    }
}

TEST(TestNetworkVdev, ClientRstDuringTransfer) {
    // import 成功后传输进行中客户端 RST（不发 FIN）：receiver 的挂起读要以
    // 连接重置错误返回并走完整清理路径，设备释放，之后可再次 import
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    StringPool string_pool;
    usbipdcpp::Server server;
    server.add_device(make_mock_keyboard(string_pool));

    ASSERT_FALSE(server.start(ep));
    {
        asio::ip::tcp::socket client(io);
        ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
        ASSERT_EQ(import_device(client, "1-1"), 0u);
        rst_disconnect(client); // 传输中 RST
    }
    ASSERT_TRUE(wait_sessions_gone(server));
    server.stop();

    // 重启后重新 import 验证设备已释放
    ASSERT_FALSE(server.start(ep));
    {
        asio::ip::tcp::socket client(io);
        ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
        ASSERT_EQ(import_device(client, "1-1"), 0u);
        client.close();
    }
    ASSERT_TRUE(wait_sessions_gone(server));
    server.stop();
}

TEST(TestNetworkVdev, TwoClientsContendForSameDevice) {
    // 两个客户端抢同一设备：A import 成功后 B 再 import 同一设备要收到 NA；
    // A 断开释放设备后 C 才能 import 成功
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    StringPool string_pool;
    usbipdcpp::Server server;
    server.add_device(make_mock_keyboard(string_pool));

    ASSERT_FALSE(server.start(ep));

    // A 先 import 成功
    asio::ip::tcp::socket client_a(io);
    ASSERT_TRUE(connect_with_retry(client_a, server.endpoint()));
    ASSERT_EQ(import_device(client_a, "1-1"), 0u);

    // B import 同一设备 → NA
    asio::ip::tcp::socket client_b(io);
    ASSERT_TRUE(connect_with_retry(client_b, server.endpoint()));
    ASSERT_EQ(import_device(client_b, "1-1"), static_cast<std::uint32_t>(OperationStatuType::NA));

    // A 断开，设备释放
    client_a.close();
    ASSERT_TRUE(wait_sessions_gone(server));

    // C 再 import 成功
    asio::ip::tcp::socket client_c(io);
    ASSERT_TRUE(connect_with_retry(client_c, server.endpoint()));
    ASSERT_EQ(import_device(client_c, "1-1"), 0u);

    client_c.close();
    ASSERT_TRUE(wait_sessions_gone(server));
    server.stop();
}

TEST(TestNetworkVdev, ClientSendsGarbageThenDisconnects) {
    // 发送无意义字节流后断开：解析要么报未知版本/命令，要么读中断，
    // 服务器不能崩，session 干净退出
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    usbipdcpp::Server server;
    ASSERT_FALSE(server.start(ep));
    {
        asio::ip::tcp::socket client(io);
        ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
        const std::array<std::uint8_t, 16> garbage = {0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x01, 0x02, 0x03,
                                                      0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B};
        asio::write(client, asio::buffer(garbage));
        client.close();
    }
    ASSERT_TRUE(wait_sessions_gone(server));
    server.stop();
}

TEST(TestNetworkVdev, StopRightAfterImportRequest) {
    // 客户端发出 import 请求后、服务器回复之前 stop()（客户端保持连接）：
    // immediately_stop 要在 import 处理的任意阶段打断 session。
    // 关键断言：重启后设备不在 using_devices（没泄漏），可以再 import
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    StringPool string_pool;
    usbipdcpp::Server server;
    server.add_device(make_mock_keyboard(string_pool));

    ASSERT_FALSE(server.start(ep));
    asio::ip::tcp::socket client(io);
    {
        ASSERT_TRUE(connect_with_retry(client, server.endpoint()));

        UsbIpCommand::OpReqImport req{.status = 0, .busid = {}};
        const std::string busid = "1-1";
        std::copy(busid.begin(), busid.end(), req.busid.begin());
        usbipdcpp::error_code send_ec;
        req.to_socket(client, send_ec);
        ASSERT_FALSE(send_ec);

        server.stop(); // 客户端还连着，不读回复直接 stop
    }
    client.close();

    // 重启后重新 import：若设备卡在 using_devices 会收到 NA
    ASSERT_FALSE(server.start(ep));
    {
        asio::ip::tcp::socket client(io);
        ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
        ASSERT_EQ(import_device(client, "1-1"), 0u);
        client.close();
    }
    ASSERT_TRUE(wait_sessions_gone(server));
    server.stop();
}

TEST(TestNetworkVdev, ClientDisconnectsDuringUrbTransfer) {
    // 客户端发出真实的 URB（CMD_SUBMIT，键盘中断 IN 端点）后立即断开：
    // 设备 handler 正在处理 URB 或 sender 正在写 RET_SUBMIT 响应时连接断开。
    // 覆盖设备侧在途 URB 清理（on_disconnection）和 sender 写失败分支，
    // 设备必须释放，重启后可再次 import
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    StringPool string_pool;
    usbipdcpp::Server server;
    server.add_device(make_mock_keyboard(string_pool));

    ASSERT_FALSE(server.start(ep));
    {
        asio::ip::tcp::socket client(io);
        ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
        ASSERT_EQ(import_device(client, "1-1"), 0u);

        // 构造键盘中断 IN 端点（逻辑 ep=1，服务器侧换算为 0x81）的 URB
        UsbIpCommand::UsbIpCmdSubmit submit{};
        submit.header.command = USBIP_CMD_SUBMIT;
        submit.header.seqnum = 1;
        submit.header.devid = 1;
        submit.header.direction = UsbIpDirection::In;
        submit.header.ep = 0x01;
        submit.transfer_flags = 0;
        submit.transfer_buffer_length = 8;
        submit.start_frame = 0;
        submit.number_of_packets = 0;
        submit.interval = 0;
        usbipdcpp::error_code send_ec;
        submit.to_socket(client, send_ec);
        ASSERT_FALSE(send_ec);

        rst_disconnect(client); // URB 流转中 RST，不读 RET_SUBMIT 响应
    }
    ASSERT_TRUE(wait_sessions_gone(server));
    server.stop();

    // 重启后重新 import 验证设备已释放
    ASSERT_FALSE(server.start(ep));
    {
        asio::ip::tcp::socket client(io);
        ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
        ASSERT_EQ(import_device(client, "1-1"), 0u);
        client.close();
    }
    ASSERT_TRUE(wait_sessions_gone(server));
    server.stop();
}

TEST(TestNetworkVdev, ImportNonexistentDevice) {
    // 客户端 import 一个不存在的设备：服务器回复 NoDev，session 正常收尾
    // 不崩；之后 import 真实设备仍然正常
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    StringPool string_pool;
    usbipdcpp::Server server;
    server.add_device(make_mock_keyboard(string_pool));

    ASSERT_FALSE(server.start(ep));
    {
        asio::ip::tcp::socket client(io);
        ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
        ASSERT_EQ(import_device(client, "9-9"), static_cast<std::uint32_t>(OperationStatuType::NoDev));
        client.close();
    }
    ASSERT_TRUE(wait_sessions_gone(server));
    server.stop();

    // 真实设备不受影响
    ASSERT_FALSE(server.start(ep));
    {
        asio::ip::tcp::socket client(io);
        ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
        ASSERT_EQ(import_device(client, "1-1"), 0u);
        client.close();
    }
    ASSERT_TRUE(wait_sessions_gone(server));
    server.stop();
}

TEST(TestNetworkVdev, ClientReconnectLoop) {
    // 服务器不重启的情况下，客户端反复 import→断开→重连→import 同一设备：
    // 设备每次都要正确释放回可用列表。FIN 和 RST 断开方式交替，
    // 10 轮后服务器仍然正常工作
    asio::io_context io;
    asio::ip::tcp::endpoint ep(asio::ip::address_v4::loopback(), 0);

    StringPool string_pool;
    usbipdcpp::Server server;
    server.add_device(make_mock_keyboard(string_pool));

    ASSERT_FALSE(server.start(ep));
    for (int round = 0; round < 10; round++) {
        asio::ip::tcp::socket client(io);
        ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
        ASSERT_EQ(import_device(client, "1-1"), 0u) << "第 " << round << " 轮 import 失败";
        if (round % 2 == 0) {
            client.close(); // 优雅断开
        }
        else {
            rst_disconnect(client); // RST 断开
        }
        ASSERT_TRUE(wait_sessions_gone(server)) << "第 " << round << " 轮 session 未清理";
    }
    server.stop();
}
