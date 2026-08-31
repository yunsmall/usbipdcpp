#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "usbipdcpp/Export.h"
#include "usbipdcpp/virtual_device/NetworkingConstants.h"
#include "usbipdcpp/virtual_device/network_backends/NetworkBackend.h"

namespace usbipdcpp {

/// 极简以太网后端：ARP 应答 + ICMP echo + TCP echo（演示"主机通过虚拟网卡
/// 访问设备侧服务"）
/// 纯用户态以太网帧处理（无平台网络 API 依赖）：主机 attach 网卡并配置同网段
/// IP 后，可 ping 通设备侧 IP，`nc <设备IP> <tcp_port>` 连上 TCP echo 服务
class USBIPDCPP_API EthernetEchoBackend : public NetworkBackend {
public:
    /**
     * @param mac 设备侧 MAC 地址（应答帧源地址）
     * @param ip 设备侧 IPv4 地址（点分十进制）
     * @param tcp_port TCP echo 服务端口
     */
    EthernetEchoBackend(std::array<std::uint8_t, 6> mac, std::array<std::uint8_t, 4> ip, std::uint16_t tcp_port);

    void send_frame(const std::uint8_t *data, std::size_t size) override;

private:
    /// 处理一帧（调用方在 send_frame 内，全部同步）
    void handle_frame(const std::uint8_t *data, std::size_t size);

    /// ARP 请求（以太网 0x0806）→ 回 ARP 应答
    void handle_arp(const std::uint8_t *frame, std::size_t size);

    /// IPv4（以太网 0x0800）→ 分发 ICMP/TCP
    void handle_ipv4(const std::uint8_t *frame, std::size_t size);

    /// ICMP echo request → echo reply
    void handle_icmp(const Ipv4Header *ip, const std::uint8_t *icmp, std::size_t icmp_len);

    /// TCP echo 状态机（SYN → SYN-ACK，数据回显，FIN 关闭）
    void handle_tcp(const Ipv4Header *ip, const std::uint8_t *tcp, std::size_t tcp_len);

    /// 发一帧（以太网头 + payload，填充到 60 字节最小帧长）
    void send_frame_to_host(const std::uint8_t *dst_mac, std::uint16_t ethertype, const std::uint8_t *payload,
                            std::size_t payload_len);

    /// 发一个 IPv4 包（组装 IP 头 + 校验和）
    void send_ipv4(std::uint8_t proto, const std::uint8_t *dst_ip, const std::uint8_t *payload,
                   std::size_t payload_len);

    /// 16 位 1 的补码和校验（IP/TCP 校验和）
    static std::uint16_t checksum(const std::uint8_t *data, std::size_t size, std::uint32_t initial = 0);

    std::array<std::uint8_t, 6> mac_;
    std::array<std::uint8_t, 4> ip_;
    std::uint16_t tcp_port_;

    // TCP 会话状态（当前只支持单连接，简化演示）
    bool tcp_established_ = false;
    std::uint8_t peer_mac_[6]{}; // 对端 MAC（handle_frame 每帧记录，本地链路单对端）
    std::uint32_t tcp_my_seq_ = 0x10000000; // 我方序列号（固定初始值即可，演示用途）
    std::uint32_t tcp_peer_seq_ = 0; // 对方下一个期望序列号（对端已发数据的下一字节）
};

} // namespace usbipdcpp
