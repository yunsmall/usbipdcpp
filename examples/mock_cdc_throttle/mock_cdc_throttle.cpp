#include "mock_cdc_throttle.h"

#include <chrono>
#include <string>
#include <thread>

#include <spdlog/spdlog.h>

using namespace usbipdcpp;

ThrottleCdcAcmDataInterfaceHandler::ThrottleCdcAcmDataInterfaceHandler(
    UsbInterface &handle_interface, StringPool &string_pool, std::uint32_t limit_bytes, std::uint32_t window_ms) :
    CdcAcmDataInterfaceHandler(handle_interface, string_pool), limit_bytes(limit_bytes), window_ms(window_ms) {
    // 防御：limit=0 会导致每一条 OUT 都立刻触发限流，不符合"固定字节数"语义
    if (this->limit_bytes == 0) {
        this->limit_bytes = 1;
    }

    // 周期上报线程：每 window_ms/2 毫秒把累计 '1' 计数以「数字\n」发回主机（IN 方向）。
    // 独立于 NAK 计时——限流暂停接收不停止统计上报
    report_timer = std::jthread([this](std::stop_token st) {
        const auto report_interval = std::chrono::milliseconds(this->window_ms / 2);
        while (!st.stop_requested()) {
            // 等到下个上报时刻（10ms 粒度轮询，断连可及时停止）
            auto deadline = std::chrono::steady_clock::now() + report_interval;
            while (!st.stop_requested() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (st.stop_requested()) {
                break;
            }
            std::size_t count;
            {
                std::lock_guard lock(state_mutex);
                count = ones_count;
            }
            // 以「数字\n」格式发回主机（非阻塞：tx 缓冲满就丢弃本次，下次周期再说）
            std::string msg = std::to_string(count) + "\n";
            send_data(msg);
            SPDLOG_INFO("上报 '1' 计数：{}", msg);
        }
    });
}

bool ThrottleCdcAcmDataInterfaceHandler::on_data_received(data_type &&data) {
    bool start_throttle = false;
    {
        std::lock_guard lock(state_mutex);
        if (throttling) {
            // NAK 期：不接收，返回 false 让 handler 把请求挂起进 out_channel
            // （数据已在设备内存里，仅延迟应答；主机 URB 挂着 = 背压停发）
            return false;
        }
        // 统计本条数据中的 '1' 字符个数，累计到上报计数
        for (std::uint8_t b: data) {
            if (b == static_cast<std::uint8_t>('1')) {
                ones_count++;
            }
        }
        received_in_window += data.size();
        if (received_in_window >= limit_bytes) {
            // 窗口收满：本条计入并立即应答，接下来进入 NAK 期 window_ms 毫秒
            received_in_window = 0;
            throttling = true;
            start_throttle = true;
        }
    }
    if (start_throttle) {
        SPDLOG_INFO("窗口已收满 {} 字节，暂停接收 {} ms（OUT NAK）", limit_bytes, window_ms);
        arm_throttle_timer();
    }
    // 窗口内正常接收（数据已完成统计），应答由 handler 完成
    return true;
}

void ThrottleCdcAcmDataInterfaceHandler::arm_throttle_timer() {
    // 新定时器覆盖旧 jthread：若上一窗口的定时线程仍在跑会被 join 等它退出
    // （正常情况早已自己结束，join 立即返回）
    throttle_timer = std::jthread([this](std::stop_token st) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(window_ms);
        // 轮询等待窗口结束；断连时 on_disconnection 会 request_stop 提前退出
        while (!st.stop_requested() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (st.stop_requested()) {
            return;  // 断连中：handler 即将析构，不再走恢复路径
        }
        {
            std::lock_guard lock(state_mutex);
            throttling = false;
        }
        flush_pending_out();
    });
}

void ThrottleCdcAcmDataInterfaceHandler::flush_pending_out() {
    // 取走 NAK 期挂起的全部 OUT 请求并应答（数据在设备内存里，排出即释放空位）。
    // 挂起期间的数据不做 '1' 统计（未走 on_data_received 的业务统计路径），只排空释放
    std::size_t flushed = 0;
    while (try_take_out()) {
        flushed++;
    }
    if (flushed) {
        SPDLOG_INFO("限流结束，排空 {} 个挂起的 OUT 请求，恢复接收", flushed);
    }
    else {
        SPDLOG_INFO("限流结束，恢复接收");
    }
}

void ThrottleCdcAcmDataInterfaceHandler::on_disconnection(error_code &ec) {
    // 先停两个线程再清理通道：handler 即将析构，不能让定时/上报线程访问已销毁的会话
    if (throttle_timer.joinable()) {
        throttle_timer.request_stop();
        throttle_timer.join();
    }
    if (report_timer.joinable()) {
        report_timer.request_stop();
        report_timer.join();
    }
    CdcAcmDataInterfaceHandler::on_disconnection(ec);
}

bool ThrottleCdcAcmDataInterfaceHandler::is_throttling() const {
    std::lock_guard lock(state_mutex);
    return throttling;
}