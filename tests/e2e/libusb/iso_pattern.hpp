#pragma once

#include <cstdint>
#include <cstring>

namespace usbipdcpp::e2e::iso {

/**
 * @brief iso 包校验头的魔数
 *
 * 每个 iso 包槽位的前 8 字节是 {包序号, 包序号 ^ magic}，后面是
 * (包序号 * 31 + 偏移) 的填充。设备侧和客户端共用这套布局，双向都能
 * 在不依赖包间连续性的前提下校验单个包的数据完整性（iso 本身允许丢包）。
 */
inline constexpr std::uint32_t packet_magic = 0x5a5a5a5a;

/**
 * @brief 长度不足 4 的包放不下包序号时使用的长度派生字节
 *
 * 这种包没有序号可校验，只能把内容和长度绑定：槽位读错、读到未写入的
 * 残留数据（客户端缓冲初始化为 0xee）都会对不上。同一次传输里两个
 * 同长度的短包内容相同，这是该长度下的固有局限。
 */
inline std::uint8_t short_packet_byte(std::uint32_t length, std::uint32_t offset) {
    return static_cast<std::uint8_t>(length * 53u + offset * 7u);
}

/**
 * @brief 按包序号填充一个 iso 包槽位
 * @param data 槽位起始地址
 * @param length 槽位长度
 * @param index 包序号（设备侧 IN 递增计数器 / 客户端 OUT 全局计数器）
 */
inline void fill_packet(std::uint8_t *data, std::uint32_t length, std::uint32_t index) {
    if (length < 4) {
        for (std::uint32_t i = 0; i < length; ++i) {
            data[i] = short_packet_byte(length, i);
        }
        return;
    }
    std::memcpy(data, &index, sizeof(index));
    if (length < 8) {
        // 4~7 字节：放得下序号但放不下校验字，多余字节按序号派生，
        // 让短包也能参与内容校验（而不是留 0xee 不写）
        for (std::uint32_t i = 4; i < length; ++i) {
            data[i] = static_cast<std::uint8_t>(index * 31 + i);
        }
        return;
    }
    const std::uint32_t check = index ^ packet_magic;
    std::memcpy(data + 4, &check, sizeof(check));
    for (std::uint32_t i = 8; i < length; ++i) {
        data[i] = static_cast<std::uint8_t>(index * 31 + i);
    }
}

/**
 * @brief 校验一个 iso 包槽位的内容
 * @param data 槽位起始地址
 * @param length 槽位长度
 * @return 内容匹配（零长度包无内容可校验，视为匹配）时返回 true
 */
inline bool verify_packet(const std::uint8_t *data, std::uint32_t length) {
    if (length < 4) {
        for (std::uint32_t i = 0; i < length; ++i) {
            if (data[i] != short_packet_byte(length, i)) {
                return false;
            }
        }
        return true;
    }
    std::uint32_t index = 0;
    std::memcpy(&index, data, sizeof(index));
    if (length < 8) {
        for (std::uint32_t i = 4; i < length; ++i) {
            if (data[i] != static_cast<std::uint8_t>(index * 31 + i)) {
                return false;
            }
        }
        return true;
    }
    std::uint32_t check = 0;
    std::memcpy(&check, data + 4, sizeof(check));
    if (check != (index ^ packet_magic)) {
        return false;
    }
    for (std::uint32_t i = 8; i < length; ++i) {
        if (data[i] != static_cast<std::uint8_t>(index * 31 + i)) {
            return false;
        }
    }
    return true;
}

}  // namespace usbipdcpp::e2e::iso
