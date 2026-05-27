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
    inline constexpr std::uint8_t ReadCapacity10 = 0x25;
    inline constexpr std::uint8_t ReadCapacity16 = 0x9E;
    inline constexpr std::uint8_t Read10 = 0x28;
    inline constexpr std::uint8_t Write10 = 0x2A;
    inline constexpr std::uint8_t Verify10 = 0x2F;
} // namespace ScsiCmd

} // namespace usbipdcpp
