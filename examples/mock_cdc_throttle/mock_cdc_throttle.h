#pragma once

#include <cstdint>
#include <mutex>
#include <thread>

#include "usbipdcpp/virtual_device/CdcAcmVirtualInterfaceHandler.h"

/**
 * @brief 限流 + 统计串口数据接口处理器
 *
 * 两件事：
 * 1. 窗口限流：固定窗口（window_ms 毫秒）内最多接收 limit_bytes 字节，超过后
 *    进入 NAK 期暂停接收 window_ms 毫秒（期间 OUT 请求挂起，主机 NAK 背压），
 *    窗口结束恢复接收并排空挂起请求。
 * 2. 统计上报：累计主机发来数据中 '1' 字符的个数，每 window_ms/2 毫秒把累计数
 *    （十进制文本 + '\n'）发回主机（IN 方向）。
 *
 * 限流语义对齐内核 u_serial 的背压：tty 层推不下时数据留在请求里不丢、稍后
 * 重试，read_pool 耗尽即 OUT 端点 NAK。这里的"推不下"由固定窗口触发——窗口内
 * 累计到上限后 on_data_received 返回 false → 请求挂起进 out_channel → 主机 NAK
 * 停发；窗口结束恢复时把所有挂起请求取出应答（数据已在设备内存里，仅延迟应答）
 */
class ThrottleCdcAcmDataInterfaceHandler : public usbipdcpp::CdcAcmDataInterfaceHandler {
public:
    ThrottleCdcAcmDataInterfaceHandler(usbipdcpp::UsbInterface &handle_interface, usbipdcpp::StringPool &string_pool,
                                       std::uint32_t limit_bytes, std::uint32_t window_ms);

    /// 创建限流数据接口（已绑定本 handler），复用基类 CDC 数据接口定义
    static usbipdcpp::UsbInterface make_interface(usbipdcpp::StringPool &string_pool, std::uint8_t in_ep,
                                                  std::uint8_t out_ep, std::uint32_t limit_bytes,
                                                  std::uint32_t window_ms) {
        usbipdcpp::UsbInterface i = usbipdcpp::CdcAcmDataInterfaceHandler::make_interface(string_pool, in_ep, out_ep);
        i.with_handler<ThrottleCdcAcmDataInterfaceHandler>(string_pool, limit_bytes, window_ms);
        return i;
    }

    bool on_data_received(usbipdcpp::data_type &&data) override;
    void on_disconnection(usbipdcpp::error_code &ec) override;

    /// 查询当前是否处于 NAK 期（限流暂停接收中）
    bool is_throttling() const;

private:
    void arm_throttle_timer();   // 启动 NAK 期计时线程（窗口结束触发恢复）
    void flush_pending_out();    // 取走并应答 NAK 期挂起的全部 OUT 请求

    std::uint32_t limit_bytes;  // 每窗口接收字节上限 n
    std::uint32_t window_ms;    // 窗口时长（毫秒），届时恢复接收

    mutable std::mutex state_mutex;
    std::size_t received_in_window = 0;  // 当前窗口已接收字节
    std::size_t ones_count = 0;          // 累计收到的 '1' 字符数（上报用）
    bool throttling = false;             // 是否处于 NAK 期

    std::jthread throttle_timer;  // NAK 期计时线程（断连时 request_stop 停止）
    std::jthread report_timer;    // 周期上报线程：每 window_ms/2 发一次计数（断连时停止）
};