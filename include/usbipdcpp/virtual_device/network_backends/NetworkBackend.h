#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>

#include "usbipdcpp/Export.h"

namespace usbipdcpp {

/**
 * @brief 以太网后端抽象接口
 *
 * 与 AudioSource / AudioSink（音频前后端）对称：实现类负责以太网帧的收发处理。
 * EcmDataInterfaceHandler 把主机发来的帧交给 send_frame（推给"网络侧"），
 * 后端产生要发回主机的帧时通过 send_to_host 回调推给 handler。
 * 回调方向都是"进"（消费方主动驱动），不设拉取模型——网卡数据面是异步事件流。
 */
class USBIPDCPP_API NetworkBackend {
public:
    virtual ~NetworkBackend() = default;

    /**
     * @brief 主机通过 USB 发来一帧以太网帧（bulk OUT 收流线程调用）
     * @param data 帧数据（以太网帧，含 14 字节头）
     * @param size 帧长度
     * @note 由 USB 收流线程调用，实现类必须快速返回、自行保证线程安全（与
     * AudioSink::write_pcm 相同约束）。实现类必须在函数开头调用 count_rx(size)
     */
    virtual void send_frame(const std::uint8_t *data, std::size_t size) = 0;

    /// 断连/流停止时清空缓冲，保证下次连接不残留旧数据（默认空实现）
    virtual void reset() {}

    /// 设置发往主机的回调（EcmDataInterfaceHandler 构造时注入，内部使用）
    void set_send_to_host(std::function<void(const std::uint8_t *, std::size_t)> callback) {
        send_to_host_ = std::move(callback);
    }

    // ========== 帧统计（调试/示例展示用，原子计数，任意线程可查） ==========

    /// 主机收的帧数（send_frame 累计）
    [[nodiscard]] std::uint64_t rx_frames() const {
        return rx_frames_;
    }
    /// 主机收的字节数
    [[nodiscard]] std::uint64_t rx_bytes() const {
        return rx_bytes_;
    }
    /// 发往主机的帧数（send_to_host 累计）
    [[nodiscard]] std::uint64_t tx_frames() const {
        return tx_frames_;
    }
    /// 发往主机的字节数
    [[nodiscard]] std::uint64_t tx_bytes() const {
        return tx_bytes_;
    }

protected:
    /// send_frame 开头必须调用：累计主机收帧统计
    void count_rx(std::size_t size) {
        rx_frames_++;
        rx_bytes_ += size;
    }

    /// 后端产生一帧时调用此函数发往主机（未注入回调时静默丢弃）
    void send_to_host(const std::uint8_t *data, std::size_t size) {
        if (send_to_host_) {
            send_to_host_(data, size);
            tx_frames_++;
            tx_bytes_ += size;
        }
    }

private:
    std::function<void(const std::uint8_t *, std::size_t)> send_to_host_;
    std::atomic<std::uint64_t> rx_frames_{0};
    std::atomic<std::uint64_t> rx_bytes_{0};
    std::atomic<std::uint64_t> tx_frames_{0};
    std::atomic<std::uint64_t> tx_bytes_{0};
};

} // namespace usbipdcpp
