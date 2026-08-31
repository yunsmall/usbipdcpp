#include "usbipdcpp/virtual_device/network_backends/EthernetEchoBackend.h"

#include <cstring>

namespace usbipdcpp {

namespace {

// 字节序转换：协议多字节字段按网络序（大端）存内存，访问时显式转换
std::uint16_t bswap16(std::uint16_t v) {
#if defined(_MSC_VER)
    return _byteswap_ushort(v);
#else
    return __builtin_bswap16(v);
#endif
}

std::uint32_t bswap32(std::uint32_t v) {
#if defined(_MSC_VER)
    return _byteswap_ulong(v);
#else
    return __builtin_bswap32(v);
#endif
}

/// 从字节流读大端 16 位值（memcpy 保证未对齐安全，checksum 用）
std::uint16_t read_be16(const std::uint8_t *p) {
    std::uint16_t v;
    std::memcpy(&v, p, sizeof(v));
    return bswap16(v);
}

} // namespace

std::uint16_t EthernetEchoBackend::checksum(const std::uint8_t *data, std::size_t size, std::uint32_t initial) {
    // 16 位 1 的补码和校验（IP/TCP/ICMP 共用）
    // initial 为伪头等先算好的累加值（未取反），本函数继续累加后折叠取反
    std::uint32_t sum = initial;
    while (size > 1) {
        sum += read_be16(data);
        data += 2;
        size -= 2;
    }
    if (size == 1) {
        sum += static_cast<std::uint32_t>(data[0]) << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return static_cast<std::uint16_t>(~sum);
}

EthernetEchoBackend::EthernetEchoBackend(std::array<std::uint8_t, 6> mac, std::array<std::uint8_t, 4> ip,
                                         std::uint16_t tcp_port) :
    mac_(mac), ip_(ip), tcp_port_(tcp_port) {
}

void EthernetEchoBackend::send_frame(const std::uint8_t *data, std::size_t size) {
    rx_frames_.fetch_add(1);
    rx_bytes_.fetch_add(size);
    handle_frame(data, size);
}

void EthernetEchoBackend::handle_frame(const std::uint8_t *data, std::size_t size) {
    if (size < ETH_HLEN) {
        return; // 坏帧：不够以太网头
    }
    const auto *eth = reinterpret_cast<const EthHeader *>(data);
    // 本地链路单对端：记录帧源 MAC，后续应答（ICMP/TCP）发往它
    std::memcpy(peer_mac_, eth->src, 6);
    switch (bswap16(eth->ethertype)) {
        case ETH_TYPE_ARP:
            handle_arp(data, size);
            break;
        case ETH_TYPE_IPV4:
            handle_ipv4(data, size);
            break;
        default:
            break; // 其他协议类型忽略
    }
}

void EthernetEchoBackend::handle_arp(const std::uint8_t *frame, std::size_t size) {
    if (size < ETH_HLEN + sizeof(ArpHeader)) {
        return; // 不够 ARP 头
    }
    const auto *arp = reinterpret_cast<const ArpHeader *>(frame + ETH_HLEN);
    // 仅处理以太网/IPv4 的 ARP 请求，且目标 IP 是自己
    if (bswap16(arp->htype) != 1 || bswap16(arp->ptype) != ETH_TYPE_IPV4 || arp->hlen != 6 || arp->plen != 4) {
        return;
    }
    if (bswap16(arp->op) != ARP_OP_REQUEST || std::memcmp(arp->tpa, ip_.data(), 4) != 0) {
        return;
    }
    // 构造 ARP 应答：目标 = 请求者（硬件/协议地址互换）
    ArpHeader reply{};
    reply.htype = arp->htype; // 请求者的值即网络序，原样复制
    reply.ptype = arp->ptype;
    reply.hlen = 6;
    reply.plen = 4;
    reply.op = bswap16(ARP_OP_REPLY);
    std::memcpy(reply.sha, mac_.data(), 6); // sha = 我们
    std::memcpy(reply.spa, ip_.data(), 4); // spa = 我们
    std::memcpy(reply.tha, arp->sha, 6); // tha = 请求者
    std::memcpy(reply.tpa, arp->spa, 4); // tpa = 请求者
    send_frame_to_host(arp->sha, ETH_TYPE_ARP, reinterpret_cast<const std::uint8_t *>(&reply), sizeof(reply));
}

void EthernetEchoBackend::handle_ipv4(const std::uint8_t *frame, std::size_t size) {
    if (size < ETH_HLEN + sizeof(Ipv4Header)) {
        return; // 不够 IPv4 头
    }
    const auto *ip = reinterpret_cast<const Ipv4Header *>(frame + ETH_HLEN);
    std::size_t ihl = static_cast<std::size_t>(ip->version_ihl & 0x0F) * 4;
    std::size_t total_len = bswap16(ip->total_len);
    if (ihl < IPV4_HLEN || total_len < ihl || size < ETH_HLEN + total_len) {
        return; // 头长/总长不合法或帧被截断
    }
    // 只处理发给我们的包（不做转发）
    if (std::memcmp(ip->dst, ip_.data(), 4) != 0) {
        return;
    }
    const std::uint8_t *payload = frame + ETH_HLEN + ihl;
    std::size_t payload_len = total_len - ihl;
    switch (ip->proto) {
        case IP_PROTO_ICMP:
            handle_icmp(ip, payload, payload_len);
            break;
        case IP_PROTO_TCP:
            handle_tcp(ip, payload, payload_len);
            break;
        default:
            break;
    }
}

void EthernetEchoBackend::handle_icmp(const Ipv4Header *ip, const std::uint8_t *icmp, std::size_t icmp_len) {
    if (icmp_len < sizeof(IcmpEchoHeader) || icmp_len > TCP_MAX_PAYLOAD) {
        return; // 不够 ICMP echo 头或段超上限
    }
    const auto *echo = reinterpret_cast<const IcmpEchoHeader *>(icmp);
    if (echo->type != ICMP_ECHO_REQUEST || echo->code != 0) {
        return;
    }
    // 整段（头+payload）拷贝后改 type、重算校验和：payload 必须原样回显，
    // 只拷贝头结构体会越界读（校验和与发送都按 icmp_len 长度）
    std::uint8_t reply[TCP_MAX_PAYLOAD];
    std::memcpy(reply, icmp, icmp_len);
    auto *hdr = reinterpret_cast<IcmpEchoHeader *>(reply);
    hdr->type = ICMP_ECHO_REPLY;
    hdr->checksum = 0;
    hdr->checksum = bswap16(checksum(reply, icmp_len));
    send_ipv4(IP_PROTO_ICMP, ip->src, reply, icmp_len);
}

void EthernetEchoBackend::handle_tcp(const Ipv4Header *ip, const std::uint8_t *tcp, std::size_t tcp_len) {
    if (tcp_len < sizeof(TcpHeader)) {
        return; // 不够 TCP 头
    }
    const auto *tcp_hdr = reinterpret_cast<const TcpHeader *>(tcp);
    if (bswap16(tcp_hdr->dst_port) != tcp_port_) {
        return; // 不是我们的服务端口
    }
    std::size_t data_offset = static_cast<std::size_t>(tcp_hdr->offset_flags_hi >> 4) * 4;
    if (data_offset < sizeof(TcpHeader) || data_offset > tcp_len) {
        return;
    }
    std::uint32_t seq = bswap32(tcp_hdr->seq);
    std::uint32_t ack = bswap32(tcp_hdr->ack);
    std::uint8_t flags = tcp_hdr->flags;
    const std::uint8_t *payload = tcp + data_offset;
    std::size_t payload_len = tcp_len - data_offset;

    auto send_segment = [&](std::uint8_t seg_flags, std::uint32_t seg_seq, std::uint32_t seg_ack,
                            const std::uint8_t *seg_payload, std::size_t seg_payload_len) {
        // TCP 头 + payload（应答段无选项，data offset 恒为 5）
        std::uint8_t segment[TCP_HLEN + TCP_MAX_PAYLOAD];
        auto *hdr = reinterpret_cast<TcpHeader *>(segment);
        hdr->src_port = bswap16(tcp_port_); // sport = 我们
        hdr->dst_port = tcp_hdr->src_port; // dport = 对方端口（网络序原样复制）
        hdr->seq = bswap32(seg_seq);
        hdr->ack = bswap32(seg_ack);
        hdr->offset_flags_hi = static_cast<std::uint8_t>(5 << 4); // data offset = 5
        hdr->flags = seg_flags;
        hdr->window = bswap16(4096);
        hdr->checksum = 0;
        hdr->urgent = 0;
        if (seg_payload_len > 0) {
            std::memcpy(segment + TCP_HLEN, seg_payload, seg_payload_len);
        }
        // TCP 校验和 = 伪头（src/dst IP + 0 + proto + 段长）+ 段
        std::uint32_t pseudo = 0;
        pseudo += static_cast<std::uint16_t>((ip->src[0] << 8) | ip->src[1]);
        pseudo += static_cast<std::uint16_t>((ip->src[2] << 8) | ip->src[3]);
        pseudo += static_cast<std::uint16_t>((ip->dst[0] << 8) | ip->dst[1]);
        pseudo += static_cast<std::uint16_t>((ip->dst[2] << 8) | ip->dst[3]);
        pseudo += IP_PROTO_TCP;
        pseudo += TCP_HLEN + seg_payload_len;
        hdr->checksum = bswap16(checksum(segment, TCP_HLEN + seg_payload_len, pseudo));
        send_ipv4(IP_PROTO_TCP, ip->src, segment, TCP_HLEN + seg_payload_len);
    };

    if ((flags & TCP_FLAG_SYN) && !tcp_established_) {
        // 三次握手第一步：回 SYN-ACK（对端地址已在 handle_frame 记录）
        tcp_peer_seq_ = seq + 1;
        tcp_established_ = true;
        send_segment(TCP_FLAG_SYN | TCP_FLAG_ACK, tcp_my_seq_, tcp_peer_seq_, nullptr, 0);
    }
    else if (tcp_established_) {
        // 先处理数据回显，再处理 FIN
        if (payload_len > 0) {
            tcp_peer_seq_ = seq + payload_len;
            send_segment(TCP_FLAG_PSH | TCP_FLAG_ACK, tcp_my_seq_, tcp_peer_seq_, payload, payload_len);
            tcp_my_seq_ += payload_len;
        }
        if (flags & TCP_FLAG_FIN) {
            tcp_peer_seq_ = seq + payload_len + 1;
            send_segment(TCP_FLAG_ACK | TCP_FLAG_FIN, tcp_my_seq_, tcp_peer_seq_, nullptr, 0);
            tcp_established_ = false;
        }
        else if (payload_len == 0 && ack > tcp_my_seq_) {
            // 纯确认（我们发过数据）：同步我方序列号
            tcp_my_seq_ = ack;
        }
    }
}

void EthernetEchoBackend::send_frame_to_host(const std::uint8_t *dst_mac, std::uint16_t ethertype,
                                             const std::uint8_t *payload, std::size_t payload_len) {
    // 缓冲按最大应答帧开（ICMP echo 的 payload 可达近 MTU，按最小帧长开会越界）；
    // 填充到 60 字节最小帧长（含 FCS 时线长 64）只为满足以太网规范
    std::uint8_t frame[ETH_FRAME_MAX];
    auto *eth = reinterpret_cast<EthHeader *>(frame);
    std::memcpy(eth->dst, dst_mac, 6);
    std::memcpy(eth->src, mac_.data(), 6);
    eth->ethertype = bswap16(ethertype);
    if (payload_len > 0) {
        std::memcpy(frame + ETH_HLEN, payload, payload_len);
    }
    std::size_t total = ETH_HLEN + payload_len;
    if (total < ETH_MIN_FRAME) {
        std::memset(frame + total, 0, ETH_MIN_FRAME - total);
        total = ETH_MIN_FRAME;
    }
    tx_frames_.fetch_add(1);
    tx_bytes_.fetch_add(total);
    send_to_host(frame, total);
}

void EthernetEchoBackend::send_ipv4(std::uint8_t proto, const std::uint8_t *dst_ip, const std::uint8_t *payload,
                                    std::size_t payload_len) {
    // 20 字节 IPv4 头 + payload（不实现分片，payload 远小于 MTU 不会触发）
    std::uint8_t ip[IPV4_HLEN + TCP_MAX_PAYLOAD];
    auto *hdr = reinterpret_cast<Ipv4Header *>(ip);
    hdr->version_ihl = 0x45; // version 4, IHL 5
    hdr->tos = 0;
    hdr->total_len = bswap16(IPV4_HLEN + payload_len);
    hdr->id = 0; // 无需关联分片
    hdr->frag_offset = bswap16(0x4000); // DF 置位（不分片）
    hdr->ttl = 64;
    hdr->proto = proto;
    hdr->checksum = 0;
    std::memcpy(hdr->src, ip_.data(), 4);
    std::memcpy(hdr->dst, dst_ip, 4);
    hdr->checksum = bswap16(checksum(ip, IPV4_HLEN));
    if (payload_len > 0) {
        std::memcpy(ip + IPV4_HLEN, payload, payload_len);
    }
    send_frame_to_host(peer_mac_, ETH_TYPE_IPV4, ip, IPV4_HLEN + payload_len);
}

} // namespace usbipdcpp
