#pragma once

#include <cstdint>
#include <vector>

namespace usbipdcpp {

/**
 * @brief 块存储后端抽象基类。
 *
 * MscBulkOnlyHandler 通过此接口读写磁盘块，不关心底层是 raw 文件、qcow2
 * 还是其他格式。派生类实现 read/write/block_count。
 */
class StorageBackend {
public:
    virtual ~StorageBackend() = default;

    virtual void read(std::uint64_t lba, std::uint16_t count, void *buffer) = 0;
    virtual bool write(std::uint64_t lba, std::uint16_t count, const std::uint8_t *data) = 0;

    // 释放 LBA 范围的物理存储（可选，默认空实现）
    virtual void punch_hole(std::uint64_t lba, std::uint64_t count) {}

    virtual std::uint64_t block_count() const = 0;

    virtual std::uint32_t block_size() const {
        return 512;
    }
};

} // namespace usbipdcpp
