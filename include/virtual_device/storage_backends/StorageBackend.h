#pragma once

#include <cstdint>
#include <system_error>
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
    virtual void punch_hole(std::uint64_t lba, std::uint64_t count) {
    }

    // 返回 LBA 处映射内存的直读指针（nullptr 表示无 mmap，需走 staging_data_ 中转）
    virtual void *get_direct_buffer(std::uint64_t lba) {
        return nullptr;
    }

    // 零拷贝发送（sendfile / TransmitFile），默认 false → 回退 asio::write
    virtual bool send_direct(std::uint64_t lba, std::size_t offset, std::size_t length, intptr_t sock_fd,
                             std::error_code &ec) {
        return false;
    }

    // 零拷贝接收（splice sock→pipe→file），默认 false → 回退 asio::read
    virtual bool recv_direct(std::uint64_t lba, std::size_t offset, std::size_t length, intptr_t sock_fd,
                             std::error_code &ec) {
        return false;
    }

    [[nodiscard]] virtual std::uint64_t block_count() const = 0;

    [[nodiscard]] virtual std::uint32_t block_size() const {
        return 512;
    }
};

} // namespace usbipdcpp
