// libusb 提交失败断连策略：严格模式对齐内核 stub（任何提交失败都断开会话），
// 宽松模式只把 NO_DEVICE 视为设备移除（Android ISO 提交偶发 IO 时设备仍在）

#include <gtest/gtest.h>

// libusb 符号由 LibusbDeviceHandler.h 带入（其内部先包含 asio 再包含
// libusb，避免 Windows 上 winsock.h 与 winsock2.h 的顺序冲突）
#include "usbipdcpp/LibusbHandler/LibusbDeviceHandler.h"

using namespace usbipdcpp;

TEST(TestLibusbSubmitErrorPolicy, StrictDisconnectsOnAnyError) {
    const int errors[] = {
            LIBUSB_ERROR_IO,
            LIBUSB_ERROR_INVALID_PARAM,
            LIBUSB_ERROR_NO_MEM,
            LIBUSB_ERROR_BUSY,
            LIBUSB_ERROR_TIMEOUT,
            LIBUSB_ERROR_NO_DEVICE,
    };
    for (int err: errors) {
        EXPECT_TRUE(LibusbDeviceHandler::should_disconnect_on_submit_error(
                err, LibusbSubmitErrorPolicy::DisconnectOnAnyError))
                << "err=" << err;
    }
}

TEST(TestLibusbSubmitErrorPolicy, LenientOnlyDisconnectsOnNoDevice) {
    EXPECT_TRUE(LibusbDeviceHandler::should_disconnect_on_submit_error(
            LIBUSB_ERROR_NO_DEVICE, LibusbSubmitErrorPolicy::DisconnectOnDeviceGone));

    // IO 是 libusb 的 errno 兜底桶（带宽不足、EAGAIN/EBUSY/EINTR 等），
    // 不代表设备被拔出，宽松策略下会话继续
    const int transient_errors[] = {
            LIBUSB_ERROR_IO,
            LIBUSB_ERROR_BUSY,
            LIBUSB_ERROR_NO_MEM,
            LIBUSB_ERROR_TIMEOUT,
    };
    for (int err: transient_errors) {
        EXPECT_FALSE(LibusbDeviceHandler::should_disconnect_on_submit_error(
                err, LibusbSubmitErrorPolicy::DisconnectOnDeviceGone))
                << "err=" << err;
    }
}

TEST(TestLibusbSubmitErrorPolicy, DefaultConfigIsStrict) {
    // 默认值与内核 stub 的悲观策略一致；宽松模式必须显式配置
    LibusbDeviceHandlerConfig config;
    EXPECT_EQ(config.submit_error_policy, LibusbSubmitErrorPolicy::DisconnectOnAnyError);
}
