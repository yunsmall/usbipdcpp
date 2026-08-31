#pragma once

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
     * AudioSink::write_pcm 相同约束）
     */
    virtual void send_frame(const std::uint8_t *data, std::size_t size) = 0;

    /// 断连/流停止时清空缓冲，保证下次连接不残留旧数据（默认空实现）
    virtual void reset() {}

    /// 设置发往主机的回调（EcmDataInterfaceHandler 构造时注入，内部使用）
    void set_send_to_host(std::function<void(const std::uint8_t *, std::size_t)> callback) {
        send_to_host_ = std::move(callback);
    }

protected:
    /// 后端产生一帧时调用此函数发往主机（未注入回调时静默丢弃）
    void send_to_host(const std::uint8_t *data, std::size_t size) {
        if (send_to_host_) {
            send_to_host_(data, size);
        }
    }

private:
    std::function<void(const std::uint8_t *, std::size_t)> send_to_host_;
};

} // namespace usbipdcpp
