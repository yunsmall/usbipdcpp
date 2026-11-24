#pragma once

#include <cstddef>

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

    /**
     * @brief bulk OUT 分块大小（字节；0 = 不分块，默认）
     *
     * 非 0 时服务器把客户端的一个 CMD_SUBMIT（一个 URB）按流水线拆成多个
     * libusb 传输提交给设备：总在途分配约为
     * bulk_out_chunk_depth * bulk_out_chunk_size，与 URB 总长度无关；读下一
     * 块时前一块可以已在设备侧传输，收包与转发重叠。设备侧因此看到多个
     * 独立 URB，而客户端仍只收到一条 RET_SUBMIT。
     *
     * 注意：这会改变设备可见的传输边界，固件若假设“一次传输就是一个
     * 完整消息”可能不接受，必须由使用者确认设备支持后再开启。块大小不是
     * 端点 wMaxPacketSize 的整数倍时自动向下取整到整数倍（不够一个包时
     * 按一个包），避免分块边界产生短包、被固件误当成传输结尾；取不到有效
     * wMaxPacketSize 时无法对齐，按配置值使用。每个 URB 的最后一块允许
     * 短于块大小（URB 尾部）。
     * 仅作用于 bulk OUT；bulk IN/控制/中断/等时仍然整块提交。
     */
    std::size_t bulk_out_chunk_size = 0;

    /**
     * @brief bulk OUT 分块的流水线深度（同时在途的块数，默认 2）
     *
     * 主线程读第 N+1 块时，第 N 块可以已经在设备侧传输，收包与转发重叠。
     * 内存上限约为 depth * bulk_out_chunk_size：depth=1 时完全串行（读一块、
     * 等它完成、再读下一块），不会有多余块在途；depth 越大吞吐越高、占用
     * 越多。小于 1 按 1 处理。仅在 bulk_out_chunk_size != 0 时生效。
     */
    std::size_t bulk_out_chunk_depth = 2;
};

} // namespace usbipdcpp
