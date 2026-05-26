#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <mutex>

#include "Export.h"
#include "virtual_device/storage_backends/StorageBackend.h"

namespace usbipdcpp {

/**
 * @brief 原始磁盘镜像文件后端（内存映射实现，跨平台）
 *
 * 将整个镜像文件通过 mmap / MapViewOfFile 映射到进程地址空间，
 * 读写在映射内存上直接 memcpy，由 OS 负责异步写回磁盘。
 *
 * 文件不存在时自动创建并填零到 initial_blocks 大小。
 * 文件已存在时根据实际大小估算块数（文件大小 / 512）。
 */
class USBIPDCPP_API RawImageBackend : public StorageBackend {
public:
    /**
     * @param path           镜像文件路径
     * @param initial_blocks 新建文件时的块数，打开已有文件时忽略
     * @param block_size     每块字节数（默认 512）
     */
    explicit RawImageBackend(std::string path, std::uint64_t initial_blocks = 2048,
                             std::uint32_t block_size = 512);
    ~RawImageBackend() override;

    std::vector<std::uint8_t> read(std::uint64_t lba, std::uint16_t count) override;
    bool write(std::uint64_t lba, std::uint16_t count, const std::uint8_t *data) override;

    std::uint64_t block_count() const override {
        return block_count_;
    }

    std::uint32_t block_size() const override {
        return block_size_;
    }

    const std::string &path() const {
        return path_;
    }

    bool is_valid() const {
        return mapped_data_ != nullptr;
    }

private:
    std::string path_;               // 文件路径
    std::uint64_t block_count_;      // 总块数
    std::uint32_t block_size_ = 512; // 每块字节数
    void *mapped_data_ = nullptr;    // 映射后的内存首地址
    std::size_t mapped_size_ = 0;    // 映射的总字节数
    mutable std::mutex mutex_;       // 保护并发读写

#ifdef _WIN32
    void *file_handle_ = nullptr;    // CreateFile 返回的 HANDLE
    void *mapping_handle_ = nullptr; // CreateFileMapping 返回的 HANDLE
#else
    int fd_ = -1; // open 返回的文件描述符
#endif
};

} // namespace usbipdcpp
