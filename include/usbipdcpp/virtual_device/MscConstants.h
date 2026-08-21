#pragma once

#include <cstdint>

namespace usbipdcpp {

#pragma pack(push, 1)
struct CBW {
    std::uint32_t dCBWSignature; // 0x43425355 "USBC"
    std::uint32_t dCBWTag;
    std::uint32_t dCBWDataTransferLength;
    std::uint8_t bmCBWFlags; // bit 7: 0=OUT, 1=IN
    std::uint8_t bCBWLUN;
    std::uint8_t bCBWCBLength;
    std::uint8_t CBWCB[16];
};

struct CSW {
    std::uint32_t dCSWSignature; // 0x53425355 "USBS"
    std::uint32_t dCSWTag;
    std::uint32_t dCSWDataResidue;
    std::uint8_t bCSWStatus; // 0=passed, 1=failed, 2=phase error
};
#pragma pack(pop)

enum class BotState : std::uint8_t {
    Idle,
    DataIn,
    DataOut,
    Status,
};

/// Bulk-Only Transport 签名
inline constexpr std::uint32_t CBW_SIGNATURE = 0x43425355; // "USBC"
inline constexpr std::uint32_t CSW_SIGNATURE = 0x53425355; // "USBS"

/// SCSI 命令码
namespace ScsiCmd {
    inline constexpr std::uint8_t TestUnitReady = 0x00;
    inline constexpr std::uint8_t RequestSense = 0x03;
    inline constexpr std::uint8_t Inquiry = 0x12;
    inline constexpr std::uint8_t ModeSense6 = 0x1A;
    inline constexpr std::uint8_t StartStopUnit = 0x1B;
    inline constexpr std::uint8_t PreventAllowMediumRemoval = 0x1E;
    inline constexpr std::uint8_t ReadFormatCapacities = 0x23;
    inline constexpr std::uint8_t ReadCapacity10 = 0x25;
    inline constexpr std::uint8_t ReadCapacity16 = 0x9E;
    inline constexpr std::uint8_t Read10 = 0x28;
    inline constexpr std::uint8_t Write10 = 0x2A;
    inline constexpr std::uint8_t Verify10 = 0x2F;
    inline constexpr std::uint8_t SynchronizeCache = 0x35;
    inline constexpr std::uint8_t WriteSame10 = 0x41;
    inline constexpr std::uint8_t Unmap = 0x42;
    inline constexpr std::uint8_t ModeSense10 = 0x5A;
    inline constexpr std::uint8_t AtaPassThrough = 0x85;
    inline constexpr std::uint8_t WriteSame16 = 0x93;
} // namespace ScsiCmd

/// SCSI 数据都是大端序，统一用字节数组字段 + 以下辅助读写，
/// 避免结构体字段直填整型导致的小端机器字节序错乱
inline void put_be16(std::uint8_t *p, std::uint16_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 8);
    p[1] = static_cast<std::uint8_t>(v);
}

inline void put_be24(std::uint8_t *p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 16);
    p[1] = static_cast<std::uint8_t>(v >> 8);
    p[2] = static_cast<std::uint8_t>(v);
}

inline void put_be32(std::uint8_t *p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v >> 24);
    p[1] = static_cast<std::uint8_t>(v >> 16);
    p[2] = static_cast<std::uint8_t>(v >> 8);
    p[3] = static_cast<std::uint8_t>(v);
}

inline void put_be64(std::uint8_t *p, std::uint64_t v) {
    put_be32(p, static_cast<std::uint32_t>(v >> 32));
    put_be32(p + 4, static_cast<std::uint32_t>(v));
}

inline std::uint16_t get_be16(const std::uint8_t *p) {
    return static_cast<std::uint16_t>((p[0] << 8) | p[1]);
}

inline std::uint32_t get_be32(const std::uint8_t *p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}

inline std::uint64_t get_be64(const std::uint8_t *p) {
    return (std::uint64_t(get_be32(p)) << 32) | get_be32(p + 4);
}

#pragma pack(push, 1)

/// INQUIRY 标准响应数据（SPC-4，36 字节）
struct InquiryData {
    std::uint8_t device_type;        // byte 0：0x00 = 直接访问块设备
    std::uint8_t rmb;                // byte 1：bit7 = 可移动介质
    std::uint8_t version;            // byte 2：0x07 = SPC-4
    std::uint8_t hisup_format;       // byte 3：bit7 = HiSup，bit3-0 = 响应数据格式
    std::uint8_t additional_length;  // byte 4：附加长度 = 31
    std::uint8_t reserved[2];        // byte 5-6
    std::uint8_t cmdque;             // byte 7：bit1 = 命令队列
    std::uint8_t vendor_id[8];       // byte 8-15：厂商标识
    std::uint8_t product_id[16];     // byte 16-31：产品标识
    std::uint8_t product_revision[4]; // byte 32-35：产品版本
};

/// VPD 0x00 支持的 VPD 页列表
struct VpdSupportedPages {
    std::uint8_t device_type; // 0x00
    std::uint8_t page_code;   // 0x00
    std::uint8_t page_length; // 支持的页数
    std::uint8_t pages[5];    // 0x00 0x80 0xB0 0xB1 0xB2
};

/// VPD 0x80 单元序列号
struct VpdUnitSerialNumber {
    std::uint8_t device_type; // 0x00
    std::uint8_t page_code;   // 0x80
    std::uint8_t page_length; // 序列号长度
    std::uint8_t serial[32];  // 序列号（超出截断，不足补 0）
};

/// VPD 0xB0 块设备特性（SBC-4）：全零 = 非旋转介质、无特殊特性
struct VpdBlockDeviceCharacteristics {
    std::uint8_t device_type;   // 0x00
    std::uint8_t page_code;     // 0xB0
    std::uint8_t page_length[2]; // 0x00 0x3C
    std::uint8_t data[60];      // 全部为 0
};

/// VPD 0xB1 块限制（SBC-4，64 字节）
struct VpdBlockLimits {
    std::uint8_t device_type;                  // 0x00
    std::uint8_t page_code;                    // 0xB1
    std::uint8_t page_length[2];               // 0x00 0x3C
    std::uint8_t medium_rotation_rate[2];      // byte 4-5：0 = 非旋转介质
    std::uint8_t reserved0[2];                 // byte 6-7
    std::uint8_t opt_transfer_granularity[4];  // byte 8-11
    std::uint8_t max_transfer_length[4];       // byte 12-15
    std::uint8_t opt_transfer_length[4];       // byte 16-19
    std::uint8_t max_prefetch_length[4];       // byte 20-23
    std::uint8_t max_unmap_lba_count[4];       // byte 24-27：单次 UNMAP 最大 LBA 数
    std::uint8_t max_unmap_block_desc_count[4]; // byte 28-31：UNMAP 最大描述符数
    std::uint8_t opt_unmap_granularity[4];     // byte 32-35：UNMAP 最优粒度（LBA）
    std::uint8_t unmap_granularity_alignment[4]; // byte 36-39：bit31 = UGAVALID
    std::uint8_t max_write_same_length[4];     // byte 40-43：单次 WRITE SAME 最大块数
    std::uint8_t max_atomic_transfer_length[4]; // byte 44-47
    std::uint8_t atomic_alignment[4];          // byte 48-51
    std::uint8_t atomic_transfer_granularity[4]; // byte 52-55
    std::uint8_t max_atomic_boundary[4];       // byte 56-59
    std::uint8_t max_atomic_boundary_granularity[4]; // byte 60-63
};

/// VPD 0xB2 逻辑块分配（SBC-4）
struct VpdLogicalBlockProvisioning {
    std::uint8_t device_type;    // 0x00
    std::uint8_t page_code;      // 0xB2
    std::uint8_t page_length[2]; // 0x00 0x04
    std::uint8_t reserved;       // byte 4
    std::uint8_t lbpu;           // byte 5：bit7 = LBPU（支持 UNMAP）
    std::uint8_t provisioning;   // byte 6：分配相关标志
    std::uint8_t reserved2;      // byte 7
};

/// REQUEST SENSE 固定格式响应（18 字节）
struct SenseData {
    std::uint8_t valid_response_code;      // byte 0：0x70 = 固定格式、有效
    std::uint8_t reserved0;                // byte 1
    std::uint8_t sense_key;                // byte 2
    std::uint8_t info[4];                  // byte 3-6
    std::uint8_t additional_length;        // byte 7：10
    std::uint8_t command_specific[4];      // byte 8-11
    std::uint8_t asc;                      // byte 12
    std::uint8_t ascq;                     // byte 13
    std::uint8_t fru;                      // byte 14
    std::uint8_t sense_key_specific[3];    // byte 15-17
};

/// MODE SENSE (6) 响应（4 字节模式头，无块描述符和模式页）
struct ModeSense6Data {
    std::uint8_t mode_data_length;       // byte 0：剩余长度 = len-1
    std::uint8_t medium_type;            // byte 1：0
    std::uint8_t wp;                     // byte 2：bit7 = 写保护
    std::uint8_t block_descriptor_length; // byte 3：0
};

/// MODE SENSE (10) 响应（8 字节模式头）
struct ModeSense10Data {
    std::uint8_t mode_data_length[2];    // byte 0-1：剩余长度 = len-2（大端）
    std::uint8_t medium_type;            // byte 2：0
    std::uint8_t wp;                     // byte 3：bit7 = 写保护
    std::uint8_t block_descriptor_length[4]; // byte 4-7：0
};

/// READ CAPACITY (10) 响应
struct ReadCapacity10Data {
    std::uint8_t last_lba[4];    // 最后 LBA（大端）
    std::uint8_t block_size[4];  // 块大小（大端）
};

/// READ CAPACITY (16) 响应
struct ReadCapacity16Data {
    std::uint8_t last_lba[8];    // 最后 LBA（大端）
    std::uint8_t block_size[4];  // 块大小（大端）
    std::uint8_t prot_info;      // byte 12
    std::uint8_t p_type;         // byte 13
    std::uint8_t reserved[2];    // byte 14-15
};

/// READ FORMAT CAPACITIES 响应（当前容量 + 一个格式描述符）
struct ReadFormatCapacitiesData {
    std::uint8_t reserved[2];     // byte 0-1
    std::uint8_t list_length[2];  // byte 2-3：容量列表总长度（大端）
    std::uint8_t capacity[4];     // byte 4-7：块数（大端）
    std::uint8_t format_type;     // byte 8：0x02 = 已格式化介质
    std::uint8_t block_size[3];   // byte 9-11：块大小（大端 24 位）
};

/// UNMAP 参数中的块描述符（16 字节，大端）
struct UnmapBlockDescriptor {
    std::uint8_t lba[8];          // 起始 LBA（大端）
    std::uint8_t block_count[4];  // 块数（大端）
    std::uint8_t reserved[4];     // 0
};

/// READ/WRITE (10) CDB（0x28/0x2A）
struct ReadWrite10Cdb {
    std::uint8_t opcode;       // 0x28 / 0x2A
    std::uint8_t flags;        // byte 1
    std::uint8_t lba[4];       // byte 2-5（大端）
    std::uint8_t group;        // byte 6
    std::uint8_t block_count[2]; // byte 7-8（大端，0 = 256）
    std::uint8_t control;      // byte 9
};

/// WRITE SAME (10) CDB（0x41）
struct WriteSame10Cdb {
    std::uint8_t opcode;       // 0x41
    std::uint8_t flags;        // byte 1：bit0 = UNMAP
    std::uint8_t lba[4];       // byte 2-5（大端）
    std::uint8_t reserved;     // byte 6
    std::uint8_t block_count[2]; // byte 7-8（大端，0 = 到介质末尾）
    std::uint8_t control;      // byte 9
};

/// WRITE SAME (16) CDB（0x93）
struct WriteSame16Cdb {
    std::uint8_t opcode;       // 0x93
    std::uint8_t flags;        // byte 1：bit0 = UNMAP
    std::uint8_t lba[8];       // byte 2-9（大端）
    std::uint8_t block_count[4]; // byte 10-13（大端，0 = 到介质末尾）
    std::uint8_t group;        // byte 14
    std::uint8_t control;      // byte 15
};

#pragma pack(pop)

} // namespace usbipdcpp
