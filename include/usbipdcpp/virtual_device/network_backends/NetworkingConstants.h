#pragma once

#include <cstdint>

namespace usbipdcpp {

// ==================== 以太网/TCP-IP 协议常量 ====================

// 长度常量
constexpr std::size_t ETH_HLEN = 14; // 以太网头长
constexpr std::size_t ETH_MIN_FRAME = 60; // 以太网最小帧长（14 头 + 46 payload，不含 FCS）
constexpr std::size_t IPV4_HLEN = 20; // IPv4 头长（无选项）
constexpr std::size_t TCP_HLEN = 20; // TCP 头长（无选项）
constexpr std::size_t TCP_MAX_PAYLOAD = 1500; // 应答段 payload 上限（不超过 MTU）
// 应答帧缓冲上限（14 头 + 20 IP + 20 TCP + 1500 payload）：
// 注意不能按 ETH_MIN_FRAME 开缓冲——ICMP echo 等应答 payload 可达近 MTU，
// 按最小帧长开数组会 memcpy 越界（glibc _FORTIFY_SOURCE 直接 abort）
constexpr std::size_t ETH_FRAME_MAX = ETH_HLEN + IPV4_HLEN + TCP_HLEN + TCP_MAX_PAYLOAD;

// 以太网类型（EtherType）
constexpr std::uint16_t ETH_TYPE_ARP = 0x0806;
constexpr std::uint16_t ETH_TYPE_IPV4 = 0x0800;

// IPv4 协议号（RFC 790）
constexpr std::uint8_t IP_PROTO_ICMP = 1;
constexpr std::uint8_t IP_PROTO_TCP = 6;

// ARP 操作码（RFC 826）
constexpr std::uint16_t ARP_OP_REQUEST = 1;
constexpr std::uint16_t ARP_OP_REPLY = 2;

// ICMP 类型（RFC 792）
constexpr std::uint8_t ICMP_ECHO_REQUEST = 8;
constexpr std::uint8_t ICMP_ECHO_REPLY = 0;

// TCP 标志位（RFC 793）
constexpr std::uint8_t TCP_FLAG_FIN = 0x01;
constexpr std::uint8_t TCP_FLAG_SYN = 0x02;
constexpr std::uint8_t TCP_FLAG_PSH = 0x08;
constexpr std::uint8_t TCP_FLAG_ACK = 0x10;

// ==================== 协议头结构体 ====================
// 字段按线格式排布（多字节字段网络序），用命名成员访问代替手算字节偏移；
// 网络序字段在结构体里保持原样，读取时用 bswap 类函数显式转换。

#pragma pack(push, 1)
/// 以太网头（14 字节）
struct EthHeader {
    std::uint8_t dst[6];
    std::uint8_t src[6];
    std::uint16_t ethertype; // 网络序（0x0806 ARP / 0x0800 IPv4）
};
static_assert(sizeof(EthHeader) == ETH_HLEN, "以太网头固定 14 字节");

/// ARP 头（28 字节，RFC 826，硬件类型以太网 + 协议 IPv4）
struct ArpHeader {
    std::uint16_t htype; // 网络序，1 = 以太网
    std::uint16_t ptype; // 网络序，0x0800 = IPv4
    std::uint8_t hlen; // 硬件地址长，6
    std::uint8_t plen; // 协议地址长，4
    std::uint16_t op; // 网络序，1 = request，2 = reply
    std::uint8_t sha[6];
    std::uint8_t spa[4];
    std::uint8_t tha[6];
    std::uint8_t tpa[4];
};
static_assert(sizeof(ArpHeader) == 28, "ARP 头固定 28 字节");

/// IPv4 头（20 字节，无选项，RFC 791）
struct Ipv4Header {
    std::uint8_t version_ihl; // 高 4 位 version=4，低 4 位 IHL（×4 字节）
    std::uint8_t tos;
    std::uint16_t total_len; // 网络序，整个 IP 包长
    std::uint16_t id; // 网络序
    std::uint16_t frag_offset; // 网络序（含 DF/MF 标志）
    std::uint8_t ttl;
    std::uint8_t proto; // 1 ICMP / 6 TCP
    std::uint16_t checksum; // 网络序
    std::uint8_t src[4];
    std::uint8_t dst[4];
};
static_assert(sizeof(Ipv4Header) == IPV4_HLEN, "IPv4 头固定 20 字节");

/// TCP 头（20 字节，无选项，RFC 793）
struct TcpHeader {
    std::uint16_t src_port; // 网络序
    std::uint16_t dst_port; // 网络序
    std::uint32_t seq; // 网络序
    std::uint32_t ack; // 网络序
    std::uint8_t offset_flags_hi; // 高 4 位 data offset（×4 字节），低 4 位保留
    std::uint8_t flags; // FIN/SYN/RST/PSH/ACK/URG 位
    std::uint16_t window; // 网络序
    std::uint16_t checksum; // 网络序
    std::uint16_t urgent; // 网络序
};
static_assert(sizeof(TcpHeader) == TCP_HLEN, "TCP 头固定 20 字节");

/// ICMP echo 头（8 字节，RFC 792）
struct IcmpEchoHeader {
    std::uint8_t type; // 8 = echo request，0 = echo reply
    std::uint8_t code;
    std::uint16_t checksum; // 网络序
    std::uint16_t id; // 网络序
    std::uint16_t seq; // 网络序
};
static_assert(sizeof(IcmpEchoHeader) == 8, "ICMP echo 头固定 8 字节");
#pragma pack(pop)

} // namespace usbipdcpp
