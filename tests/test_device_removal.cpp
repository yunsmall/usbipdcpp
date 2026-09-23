// 设备被物理移除（is_device_removed()）后的释放语义专项测试：导入前被移除、
// 传输中被移除、FIN/RST 断开、stop 并发、多客户端争用——设备都必须从 using
// 列表直接丢弃、绝不回可用列表（后端只在拔出那一刻扫一遍列表，放回去就是
// 没人再清得掉的僵尸，只会在下次导入时白跑一次打开失败）。
// 真实 libusb 后端的置位路径（hotplug / 传输回调 / try_remove_dead_device）
// 需要真实设备，由 e2e 覆盖；这里用虚拟设备复现同样的语义
#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "test_utils.h"

#include "usbipdcpp/Device.h"
#include "usbipdcpp/Server.h"
#include "usbipdcpp/protocol.h"
#include "usbipdcpp/utils/StringPool.h"
#include "usbipdcpp/virtual_device/SimpleVirtualDeviceHandler.h"
#include "usbipdcpp/virtual_device/devices/KeyboardHandler.h"

using namespace usbipdcpp;
using namespace usbipdcpp::test;

namespace {
// 可标记"已物理移除"的虚拟键盘：设备级 handler 把 is_device_removed() 接到
// 共享标记上；"打开失败"放在接口层——真实设备的打开失败（如音频设备打开
// 失败）就发生在这里，基类 VirtualDeviceHandler::on_new_connection 对接口
// 建连失败有既有回滚（停调度器、断开已建连的接口、撤 responder），失败的
// 接口自身没建立状态也就无需回滚
class RemovableKeyboardInterfaceHandler : public KeyboardHandler {
public:
    using KeyboardHandler::KeyboardHandler;

    // 移除标记由测试持有，与设备级 handler 共享（生命周期长于设备）
    std::shared_ptr<std::atomic_bool> removed_flag;

    void on_new_connection(TransferResponder &responder, error_code &ec) override {
        if (removed_flag && removed_flag->load()) {
            // 已拔出的设备打不开：没有建立任何连接状态，无需回滚
            ec = make_error_code(ErrorType::NO_DEVICE);
            return;
        }
        KeyboardHandler::on_new_connection(responder, ec);
    }
};

// 设备级 handler：on_new_connection 不覆盖（连接建立与失败回滚全走基类），
// 只把 is_device_removed() 接到共享标记
class RemovableDeviceHandler : public SimpleVirtualDeviceHandler {
public:
    using SimpleVirtualDeviceHandler::SimpleVirtualDeviceHandler;

    std::shared_ptr<std::atomic_bool> removed_flag;

    bool is_device_removed() const override {
        return removed_flag && removed_flag->load();
    }
};

// 构造使用上面两个 handler 的虚拟键盘设备（接口定义与 mock_keyboard 相同）
std::shared_ptr<UsbDevice> make_removable_keyboard(StringPool &string_pool,
                                                   std::shared_ptr<std::atomic_bool> removed_flag) {
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
    auto interface_handler = mock_keyboard->interfaces[0].with_handler<RemovableKeyboardInterfaceHandler>(string_pool);
    interface_handler->removed_flag = removed_flag;
    auto device_handler = mock_keyboard->with_handler<RemovableDeviceHandler>(string_pool);
    device_handler->removed_flag = removed_flag;
    device_handler->setup_interface_handlers();
    return mock_keyboard;
}

// 断言设备已被丢弃：可用与使用中两个列表都为空
void expect_discarded(Server &server) {
    std::shared_lock lock(server.get_devices_mutex());
    EXPECT_TRUE(server.get_available_devices().empty());
    EXPECT_TRUE(server.get_using_devices().empty());
}

// 跑一轮"导入 → 传输中设备被移除 → 客户端断开"，断言设备被丢弃。
// use_rst 选择断开方式（FIN / RST 走 receiver 的不同收尾分支）
void expect_removed_device_discarded(bool use_rst) {
    asio::io_context io;
    StringPool string_pool;
    usbipdcpp::Server server;
    auto removed_flag = std::make_shared<std::atomic_bool>(false);
    server.add_device(make_removable_keyboard(string_pool, removed_flag));
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u);
    {
        std::shared_lock lock(server.get_devices_mutex());
        EXPECT_TRUE(server.get_available_devices().empty());
        EXPECT_EQ(server.get_using_devices().size(), 1u);
    }

    removed_flag->store(true); // 传输期间设备被物理拔出

    if (use_rst) {
        rst_disconnect(client);
    }
    else {
        client.close();
    }
    ASSERT_TRUE(wait_sessions_gone(server));
    expect_discarded(server);
    server.stop();
}
} // namespace

TEST(TestDeviceRemoval, RemovedDeviceIsNotReturnedToAvailable) {
    expect_removed_device_discarded(false);
}

TEST(TestDeviceRemoval, RemovedDeviceIsNotReturnedToAvailableOnRst) {
    expect_removed_device_discarded(true);
}

TEST(TestDeviceRemoval, AliveDeviceIsReturnedToAvailable) {
    // 对照组：设备未被移除时断开，照常回可用列表（丢弃逻辑不能误伤正常设备）
    asio::io_context io;
    StringPool string_pool;
    usbipdcpp::Server server;
    auto removed_flag = std::make_shared<std::atomic_bool>(false);
    server.add_device(make_removable_keyboard(string_pool, removed_flag));
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u);
    client.close();
    ASSERT_TRUE(wait_sessions_gone(server));

    {
        std::shared_lock lock(server.get_devices_mutex());
        EXPECT_EQ(server.get_available_devices().size(), 1u);
        EXPECT_TRUE(server.get_using_devices().empty());
    }
    server.stop();
}

TEST(TestDeviceRemoval, ImportRemovedDeviceFailsAndIsDiscarded) {
    // 已移除但还残留在列表里的设备（释放与标记之间的竞态窗口）：导入必然
    // 失败（打开已拔出的设备不可能成功），失败后设备被直接丢弃，不会留在
    // 列表里反复被导入——换个连接再导入只会得到"设备不存在"
    asio::io_context io;
    StringPool string_pool;
    usbipdcpp::Server server;
    auto removed_flag = std::make_shared<std::atomic_bool>(true); // 拔出后残留在列表里
    server.add_device(make_removable_keyboard(string_pool, removed_flag));
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), static_cast<std::uint32_t>(OperationStatuType::NA));
    expect_discarded(server);

    client.close();
    ASSERT_TRUE(wait_sessions_gone(server));

    asio::ip::tcp::socket retry_client(io);
    ASSERT_TRUE(connect_with_retry(retry_client, server.endpoint()));
    EXPECT_EQ(import_device(retry_client, "1-1"), static_cast<std::uint32_t>(OperationStatuType::NoDev));
    retry_client.close();
    ASSERT_TRUE(wait_sessions_gone(server));
    server.stop();
}

TEST(TestDeviceRemoval, StopWithRemovedImportedDevice) {
    // stop() 打断正在传输且已被移除设备的 session：设备同样要从 using 丢弃，
    // 不能在 stop 的收尾路径上被塞回可用列表
    asio::io_context io;
    StringPool string_pool;
    usbipdcpp::Server server;
    auto removed_flag = std::make_shared<std::atomic_bool>(false);
    server.add_device(make_removable_keyboard(string_pool, removed_flag));
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket client(io);
    ASSERT_TRUE(connect_with_retry(client, server.endpoint()));
    ASSERT_EQ(import_device(client, "1-1"), 0u);

    removed_flag->store(true); // 传输期间设备被物理拔出
    server.stop();             // 不关客户端直接 stop，走打断路径

    expect_discarded(server);
    client.close();
}

TEST(TestDeviceRemoval, SecondClientCannotImportDeviceInUseEvenIfRemoved) {
    // 设备已被移除但第一个客户端还占着（session 未收尾）：第二个客户端导入
    // 必须被拒（设备仍在 using），不能因"已移除"而放行；第一个断开后丢弃
    asio::io_context io;
    StringPool string_pool;
    usbipdcpp::Server server;
    auto removed_flag = std::make_shared<std::atomic_bool>(false);
    server.add_device(make_removable_keyboard(string_pool, removed_flag));
    ASSERT_FALSE(server.start(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0)));

    asio::ip::tcp::socket client_a(io);
    ASSERT_TRUE(connect_with_retry(client_a, server.endpoint()));
    ASSERT_EQ(import_device(client_a, "1-1"), 0u);

    removed_flag->store(true); // 使用期间设备被物理拔出

    asio::ip::tcp::socket client_b(io);
    ASSERT_TRUE(connect_with_retry(client_b, server.endpoint()));
    EXPECT_NE(import_device(client_b, "1-1"), 0u); // 占用中，拒绝
    client_b.close();

    client_a.close();
    ASSERT_TRUE(wait_sessions_gone(server));
    expect_discarded(server); // 断开后从 using 丢弃，不回可用列表
    server.stop();
}