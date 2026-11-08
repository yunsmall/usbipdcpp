// libusb 后端公开 API 的前置条件与幂等语义测试：
// 无效参数应返回错误码而不是崩溃/误删设备，重复 start 不应触发 std::terminate。

#include <gtest/gtest.h>

#include <asio.hpp>
#include <libusb.h>

#include "usbipdcpp/LibusbHandler/LibusbServer.h"

using namespace usbipdcpp;

TEST(TestLibusbServerApi, NullDeviceIsRejected) {
    // unbind_host_device 对 nullptr 必须安全返回，不能进 libusb 查询
    LibusbServer server;
    EXPECT_EQ(server.unbind_host_device(nullptr), DeviceOperationResult::DeviceNotFound);
}

TEST(TestLibusbServerApi, NegativeFdIsRejected) {
    // 普通模式 handler 的 wrapped_fd_ 也是 -1，负 fd 不能命中普通模式设备
    LibusbServer server;
    EXPECT_EQ(server.unbind_host_device_by_fd(-1), DeviceOperationResult::DeviceNotFound);
}

TEST(TestLibusbServerApi, NullDeviceBindIsRejected) {
    LibusbServer server;
    EXPECT_EQ(server.bind_host_device(nullptr), DeviceOperationResult::DeviceNotFound);
}

TEST(TestLibusbServerApi, NegativeFdBindIsRejected) {
    LibusbServer server;
    EXPECT_EQ(server.bind_host_device_with_wrapped_fd(-1), DeviceOperationResult::DeviceNotFound);
}

TEST(TestLibusbServerApi, StartIsIdempotent) {
    // 重复 start 未 stop 时不能对 joinable 的 std::thread 赋值（会 terminate），
    // 与 Server::start 的幂等语义一致直接返回成功
    if (libusb_init(nullptr) != 0) {
        GTEST_SKIP() << "libusb_init 失败，跳过（需要可用的 libusb 运行环境）";
    }
    LibusbServer server;
    asio::ip::tcp::endpoint ep(asio::ip::make_address("127.0.0.1"), 0);
    auto first = server.start(ep);
    EXPECT_FALSE(first);
    auto second = server.start(ep);
    EXPECT_FALSE(second);
    server.stop();
    libusb_exit(nullptr);
}
