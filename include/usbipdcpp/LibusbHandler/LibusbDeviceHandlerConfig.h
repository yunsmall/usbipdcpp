#pragma once

namespace usbipdcpp {

/**
 * @brief libusb_submit_transfer 失败时的断连策略
 *
 * 内核 stub 对提交失败采用悲观策略：任何 usb_submit_urb 失败都
 * usbip_event_add(SDEV_EVENT_ERROR_SUBMIT) 丢弃连接（stub_rx.c）。
 * 但 Android 等平台上 ISO 提交可能因带宽/资源问题返回 LIBUSB_ERROR_IO，
 * 设备实际仍在，此时可以只让本次 URB 失败并继续会话。
 */
enum class LibusbSubmitErrorPolicy {
    /// 任何提交失败都断开会话，与内核 stub 行为一致
    DisconnectOnAnyError,
    /// 仅 LIBUSB_ERROR_NO_DEVICE（及异步回调的 LIBUSB_TRANSFER_NO_DEVICE）
    /// 视为设备移除；其他失败只让本次 URB 以 EPIPE 结束，会话继续
    DisconnectOnDeviceGone,
};

/**
 * @brief LibusbDeviceHandler 的行为配置
 *
 * 创建 handler 时按值拷贝，运行期间不可变。
 */
struct LibusbDeviceHandlerConfig {
    /// libusb_submit_transfer 失败时的断连策略
    LibusbSubmitErrorPolicy submit_error_policy = LibusbSubmitErrorPolicy::DisconnectOnAnyError;
};

} // namespace usbipdcpp
