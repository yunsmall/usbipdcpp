#pragma once

#include <cstddef>
#include <cstdint>

#include "usbipdcpp/type.h"
#include "usbipdcpp/utils/utils.h"

namespace usbipdcpp {
// RNDIS（Remote NDIS）协议常量与消息结构体。
// 参考微软 RNDIS 2.0 规范与内核 rndis.c / include/linux/rndis.h；
// 设备描述符用 ACM 外壳（内核 f_rndis.c：控制接口匹配 ACM 而非 Ethernet），
// 不需要微软的 RNDIS 专有描述符（0x24/0x20）

// RNDIS 消息类型（最高位 = 完成标志）
enum class RndisMessageType : std::uint32_t {
    Packet = 0x00000001, // 数据通道封装
    Init = 0x00000002,
    InitComplete = 0x80000002,
    Halt = 0x00000003,
    Query = 0x00000004,
    QueryComplete = 0x80000004,
    Set = 0x00000005,
    SetComplete = 0x80000005,
    Reset = 0x00000006,
    ResetComplete = 0x80000006,
    Indicate = 0x00000007, // 设备主动状态通知（MEDIA_CONNECT 等）
    Keepalive = 0x00000008,
    KeepaliveComplete = 0x80000008,
};

// RNDIS 状态码（主机按此判断命令结果）
enum class RndisStatus : std::uint32_t {
    Success = 0x00000000,
    MediaConnect = 0x4001000B,
    MediaDisconnect = 0x4001000C,
    NotSupported = 0xC00000BB,
    InvalidData = 0xC0010015,
};

// 枚举 → 线格式值（聚合初始化 packed 结构体字段用，enum class 不能隐式转整型）
inline constexpr std::uint32_t rndis_msg(RndisMessageType t) {
    return static_cast<std::uint32_t>(t);
}
inline constexpr std::uint32_t rndis_status(RndisStatus s) {
    return static_cast<std::uint32_t>(s);
}

// ==================== OID 常量 ====================
// 值取自内核 include/linux/rndis.h；名称保持 NDIS 惯例（OID_GEN_* / OID_802_3_*）
inline constexpr std::uint32_t OID_GEN_SUPPORTED_LIST = 0x00010101;
inline constexpr std::uint32_t OID_GEN_HARDWARE_STATUS = 0x00010102;
inline constexpr std::uint32_t OID_GEN_MEDIA_SUPPORTED = 0x00010103;
inline constexpr std::uint32_t OID_GEN_MEDIA_IN_USE = 0x00010104;
inline constexpr std::uint32_t OID_GEN_MAXIMUM_FRAME_SIZE = 0x00010106;
inline constexpr std::uint32_t OID_GEN_LINK_SPEED = 0x00010107;
inline constexpr std::uint32_t OID_GEN_TRANSMIT_BLOCK_SIZE = 0x0001010A;
inline constexpr std::uint32_t OID_GEN_RECEIVE_BLOCK_SIZE = 0x0001010B;
inline constexpr std::uint32_t OID_GEN_VENDOR_ID = 0x0001010C;
inline constexpr std::uint32_t OID_GEN_VENDOR_DESCRIPTION = 0x0001010D;
inline constexpr std::uint32_t OID_GEN_CURRENT_PACKET_FILTER = 0x0001010E;
inline constexpr std::uint32_t OID_GEN_MAXIMUM_TOTAL_SIZE = 0x00010111;
inline constexpr std::uint32_t OID_GEN_MAC_OPTIONS = 0x00010113;
inline constexpr std::uint32_t OID_GEN_MEDIA_CONNECT_STATUS = 0x00010114;
inline constexpr std::uint32_t OID_GEN_VENDOR_DRIVER_VERSION = 0x00010116;
inline constexpr std::uint32_t OID_GEN_PHYSICAL_MEDIUM = 0x00010202;
inline constexpr std::uint32_t OID_GEN_XMIT_OK = 0x00020101;
inline constexpr std::uint32_t OID_GEN_RCV_OK = 0x00020102;
inline constexpr std::uint32_t OID_GEN_XMIT_ERROR = 0x00020103;
inline constexpr std::uint32_t OID_GEN_RCV_ERROR = 0x00020104;
inline constexpr std::uint32_t OID_GEN_RCV_NO_BUFFER = 0x00020105;
inline constexpr std::uint32_t OID_802_3_PERMANENT_ADDRESS = 0x01010101;
inline constexpr std::uint32_t OID_802_3_CURRENT_ADDRESS = 0x01010102;
inline constexpr std::uint32_t OID_802_3_MULTICAST_LIST = 0x01010103;
inline constexpr std::uint32_t OID_802_3_MAXIMUM_LIST_SIZE = 0x01010104;
inline constexpr std::uint32_t OID_802_3_MAC_OPTIONS = 0x01010105;
inline constexpr std::uint32_t OID_802_3_RCV_ERROR_ALIGNMENT = 0x01020101;
inline constexpr std::uint32_t OID_802_3_XMIT_ONE_COLLISION = 0x01020102;
inline constexpr std::uint32_t OID_802_3_XMIT_MORE_COLLISIONS = 0x01020103;

// ==================== 协议常量 ====================
// 版本与设备能力（对齐内核 rndis.h：connectionless 网卡 + 802.3 介质）
inline constexpr std::uint32_t RNDIS_MAJOR_VERSION = 0x00000001;
inline constexpr std::uint32_t RNDIS_MINOR_VERSION = 0x00000000;
inline constexpr std::uint32_t RNDIS_DF_CONNECTIONLESS = 0x00000001;
inline constexpr std::uint32_t RNDIS_MEDIUM_802_3 = 0x00000000;
// 最大帧长（含以太网头，不含 FCS）与单条消息上限（对齐内核 rndis.h）
inline constexpr std::uint32_t RNDIS_MAXIMUM_FRAME_SIZE = 1518;
inline constexpr std::uint32_t RNDIS_MAX_TOTAL_SIZE = 1558;
// INIT_C 的 MaxTransferSize：内核 rndis.c 回 mtu+ethhdr+rndis_header+22 = 1580。
// 主机侧要求 ≥ hard_mtu（1524），低于 1518 会绑定失败
inline constexpr std::uint32_t RNDIS_MAX_TRANSFER_SIZE = 1580;
// 响应队列上限：正常握手队列条数是个位数，上限防恶意主机无限撑爆内存
// （对齐通道类"丢最旧"的防堆积惯例）
inline constexpr std::size_t RNDIS_RESPONSE_QUEUE_LIMIT = 64;
// 主机默认包过滤器（rndis_host.c RNDIS_DEFAULT_FILTER）：
// DIRECTED|BROADCAST|ALL_MULTICAST|PROMISCUOUS
inline constexpr std::uint16_t RNDIS_DEFAULT_FILTER = 0x2D;
// MAC 选项（OID_GEN_MAC_OPTIONS）：RECEIVE_SERIALIZED|FULL_DUPLEX（对齐内核）
inline constexpr std::uint32_t RNDIS_MAC_OPTIONS_SERIALIZED_FULL_DUPLEX = 0x12;

// ==================== 消息结构体 ====================
// 线上格式固定小端。出站消息用 append_to 序列化（htole 自动转小端）；
// 入站消息 memcpy 到结构体后字段值直接访问（假定小端主机，同 MSC 的 CBW）

#pragma pack(push, 1)
/// 所有消息共用的 12 字节前缀
struct RndisMessageHeader {
    std::uint32_t message_type;
    std::uint32_t message_length; // 整条消息字节数（含本头）
    std::uint32_t request_id; // 命令回显号（HALT/RESET/INDICATE 不用）
};
static_assert(sizeof(RndisMessageHeader) == 12, "RNDIS 消息头固定 12 字节");

/// RNDIS_MSG_INIT（主机→设备，24 字节）
struct RndisInitMsg {
    std::uint32_t message_type;
    std::uint32_t message_length;
    std::uint32_t request_id;
    std::uint32_t major_version;
    std::uint32_t minor_version;
    std::uint32_t max_transfer_size; // 主机侧 rx 缓冲，设备可忽略
};
static_assert(sizeof(RndisInitMsg) == 24, "RNDIS_MSG_INIT 固定 24 字节");

/// RNDIS_MSG_INIT_C（设备→主机，52 字节）
struct RndisInitCmplt {
    std::uint32_t message_type;
    std::uint32_t message_length;
    std::uint32_t request_id;
    std::uint32_t status;
    std::uint32_t major_version;
    std::uint32_t minor_version;
    std::uint32_t device_flags; // RNDIS_DF_CONNECTIONLESS
    std::uint32_t medium; // RNDIS_MEDIUM_802_3
    std::uint32_t max_packets_per_transfer; // 1：一次传输一个包
    std::uint32_t max_transfer_size;
    std::uint32_t packet_alignment_factor; // 0：无对齐要求
    std::uint32_t af_list_offset; // 0：无地址族列表
    std::uint32_t af_list_size;

    void append_to(data_type &d) const {
        vector_append_to_le(d, message_type, message_length, request_id, status, major_version, minor_version,
                            device_flags, medium, max_packets_per_transfer, max_transfer_size,
                            packet_alignment_factor, af_list_offset, af_list_size);
    }
};
static_assert(sizeof(RndisInitCmplt) == 52, "RNDIS_MSG_INIT_C 固定 52 字节");

/// RNDIS_MSG_QUERY（主机→设备，28 字节 + 输入缓冲）
/// 输入缓冲偏移相对 RequestID 字段（消息第 8 字节），解析时数据起点 = 8 + offset
struct RndisQueryMsg {
    std::uint32_t message_type;
    std::uint32_t message_length;
    std::uint32_t request_id;
    std::uint32_t oid;
    std::uint32_t information_buffer_length; // 输入缓冲长度（ActiveSync 会带 48 字节怪癖值）
    std::uint32_t information_buffer_offset; // 输入缓冲偏移（相对第 8 字节）
    std::uint32_t device_vc_handle; // 0
};
static_assert(sizeof(RndisQueryMsg) == 28, "RNDIS_MSG_QUERY 固定 28 字节");

/// RNDIS_MSG_QUERY_C（设备→主机，24 字节 + 数据；InformationBufferOffset 固定 16）
struct RndisQueryCmplt {
    std::uint32_t message_type;
    std::uint32_t message_length;
    std::uint32_t request_id;
    std::uint32_t status;
    std::uint32_t information_buffer_length;
    std::uint32_t information_buffer_offset; // 16：数据紧跟 24 字节头

    void append_to(data_type &d) const {
        vector_append_to_le(d, message_type, message_length, request_id, status, information_buffer_length,
                            information_buffer_offset);
    }
};
static_assert(sizeof(RndisQueryCmplt) == 24, "RNDIS_MSG_QUERY_C 头固定 24 字节");

/// RNDIS_MSG_SET（主机→设备，28 字节 + 输入缓冲；offset 语义同 QUERY）
struct RndisSetMsg {
    std::uint32_t message_type;
    std::uint32_t message_length;
    std::uint32_t request_id;
    std::uint32_t oid;
    std::uint32_t information_buffer_length;
    std::uint32_t information_buffer_offset;
    std::uint32_t device_vc_handle;
};
static_assert(sizeof(RndisSetMsg) == 28, "RNDIS_MSG_SET 固定 28 字节");

/// RNDIS_MSG_SET_C（设备→主机，16 字节）
struct RndisSetCmplt {
    std::uint32_t message_type;
    std::uint32_t message_length;
    std::uint32_t request_id;
    std::uint32_t status;

    void append_to(data_type &d) const {
        vector_append_to_le(d, message_type, message_length, request_id, status);
    }
};
static_assert(sizeof(RndisSetCmplt) == 16, "RNDIS_MSG_SET_C 固定 16 字节");

/// RNDIS_MSG_RESET_C（设备→主机，16 字节；无 RequestID）
struct RndisResetCmplt {
    std::uint32_t message_type;
    std::uint32_t message_length;
    std::uint32_t status;
    std::uint32_t addressing_reset; // 1 = 地址/状态信息已复位

    void append_to(data_type &d) const {
        vector_append_to_le(d, message_type, message_length, status, addressing_reset);
    }
};
static_assert(sizeof(RndisResetCmplt) == 16, "RNDIS_MSG_RESET_C 固定 16 字节");

/// RNDIS_MSG_KEEPALIVE_C（设备→主机，16 字节）
struct RndisKeepaliveCmplt {
    std::uint32_t message_type;
    std::uint32_t message_length;
    std::uint32_t request_id;
    std::uint32_t status;

    void append_to(data_type &d) const {
        vector_append_to_le(d, message_type, message_length, request_id, status);
    }
};
static_assert(sizeof(RndisKeepaliveCmplt) == 16, "RNDIS_MSG_KEEPALIVE_C 固定 16 字节");

/// RNDIS_MSG_INDICATE（设备→主机主动状态通知，20 字节；无 RequestID）
struct RndisIndicateStatusMsg {
    std::uint32_t message_type;
    std::uint32_t message_length;
    std::uint32_t status; // MEDIA_CONNECT / MEDIA_DISCONNECT
    std::uint32_t status_buffer_length; // 0
    std::uint32_t status_buffer_offset; // 0

    void append_to(data_type &d) const {
        vector_append_to_le(d, message_type, message_length, status, status_buffer_length, status_buffer_offset);
    }
};
static_assert(sizeof(RndisIndicateStatusMsg) == 20, "RNDIS_MSG_INDICATE 固定 20 字节");

/// RNDIS_MSG_PACKET（数据通道封装头，44 字节）
/// 数据起始 = 8 + data_offset（8 = MessageType+MessageLength 两个 DWORD）：
/// 设备→主机发 36（数据从字节 44 起），主机→设备发 16（数据从字节 24 起）。
/// OOB/PerPacket 字段内核实现全为 0（不做 TCP 校验和卸载），设备容忍非 0 即可
struct RndisPacketHeader {
    std::uint32_t message_type;
    std::uint32_t message_length; // 44 + data_length（不含尾填充）
    std::uint32_t data_offset;
    std::uint32_t data_length;
    std::uint32_t oob_data_offset;
    std::uint32_t oob_data_length;
    std::uint32_t num_oob_data_elements;
    std::uint32_t per_packet_info_offset;
    std::uint32_t per_packet_info_length;
    std::uint32_t vc_handle;
    std::uint32_t reserved;

    void append_to(data_type &d) const {
        vector_append_to_le(d, message_type, message_length, data_offset, data_length, oob_data_offset,
                            oob_data_length, num_oob_data_elements, per_packet_info_offset,
                            per_packet_info_length, vc_handle, reserved);
    }
};
static_assert(sizeof(RndisPacketHeader) == 44, "RNDIS_MSG_PACKET 头固定 44 字节");
#pragma pack(pop)

/// RESPONSE_AVAILABLE 通知：中断 IN 端点发 8 字节（两个 LE32：{1, 0}），
/// 主机据此 GET_ENCAPSULATED_RESPONSE 取走排队响应（对齐内核 resp_avail 回调）
inline data_type make_rndis_response_available_notification() {
    return {1, 0, 0, 0, 0, 0, 0, 0};
}
} // namespace usbipdcpp
