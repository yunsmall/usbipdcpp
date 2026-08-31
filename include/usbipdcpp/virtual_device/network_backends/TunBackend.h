#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <thread>

#include "usbipdcpp/Export.h"
#include "usbipdcpp/virtual_device/network_backends/NetworkBackend.h"

namespace usbipdcpp {

/**
 * @brief TUN/TAP 后端：把虚拟网卡接入主机真实内核协议栈（Linux /dev/net/tun）
 *
 * 用 IFF_TAP 创建设备（二层，行为等同真实以太网卡）：主机（USB 客户端）发来的
 * 以太网帧 write 进 tap 设备，内核按普通网卡帧处理（ARP / ICMP / TCP 由内核
 * 协议栈应答，可路由转发到真实网络）；内核写回 tap 的完整以太网帧由读线程经
 * send_to_host 推给主机。tap 接口在 fd 关闭时销毁。
 *
 * @note 仅 Linux 内核平台可用（含 WSL2 / Android termux / armbian）：TUNSETIFF 与
 * 网卡配置（IP/掩码/UP）需要 root 权限。Windows/macOS 不编译该文件（macOS 的
 * utun 是三层设备且无公开 TAP 接口，tuntaposx 驱动在新系统上已被禁用）
 */
class USBIPDCPP_API TunBackend : public NetworkBackend {
public:
    /**
     * @param if_name 接口名（含 %d 时内核自动分配编号，如 "usbip%d"）；创建后
     *        用 interface_name() 查实际名
     * @param ip 设备侧 IPv4（4 字节，如 192.168.53.1）
     * @param netmask 子网掩码（如 255.255.255.0）
     * @throws std::runtime_error 打开 /dev/net/tun 或 ioctl 失败（root 权限 /
     *        内核未开 CONFIG_TUN / 接口名非法）
     */
    TunBackend(std::string if_name, std::array<std::uint8_t, 4> ip, std::array<std::uint8_t, 4> netmask);

    ~TunBackend() override;

    /// 主机发来一帧（USB 收流线程调用）：写入 tun 设备交给内核协议栈
    void send_frame(const std::uint8_t *data, std::size_t size) override;

    /// 实际创建的接口名（TUNSETIFF 会把 %d 展开成编号）
    [[nodiscard]] const std::string &interface_name() const {
        return if_name_;
    }

private:
    /// 阻塞读 tun 设备：内核协议栈发来的帧经 send_to_host 推给主机
    void read_loop();

    int tun_fd_ = -1;
    std::string if_name_;
    std::thread read_thread_;
    std::atomic_bool stop_{false};
};

} // namespace usbipdcpp
