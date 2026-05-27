#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

#include "Export.h"
#include "virtual_device/storage_backends/StorageBackend.h"

namespace usbipdcpp {

/** 纯内存块存储后端，不落盘，无平台依赖。主要用于测试或临时数据。
 *  get_direct_buffer 返回内部缓冲区指针，READ/WRITE 零拷贝均可用。 */
class USBIPDCPP_API MemoryBackend : public StorageBackend {
public:
    explicit MemoryBackend(std::uint64_t blocks, std::uint32_t block_size = 512) :
        block_count_(blocks), block_size_(block_size), data_(static_cast<std::size_t>(blocks) * block_size) {
    }

    std::size_t read(std::uint64_t lba, std::uint16_t count, void *buffer) override {
        auto total = static_cast<std::size_t>(count) * block_size_;
        auto offset = static_cast<std::size_t>(lba) * block_size_;
        std::memcpy(buffer, data_.data() + offset, total);
        return total;
    }

    std::size_t write(std::uint64_t lba, std::uint16_t count, const void *data) override {
        auto total = static_cast<std::size_t>(count) * block_size_;
        auto offset = static_cast<std::size_t>(lba) * block_size_;
        std::memcpy(data_.data() + offset, data, total);
        return total;
    }

    void punch_hole(std::uint64_t lba, std::uint64_t count) override {
        auto offset = static_cast<std::size_t>(lba) * block_size_;
        auto length = static_cast<std::size_t>(count) * block_size_;
        std::memset(data_.data() + offset, 0, length);
    }

    void *get_direct_buffer(std::uint64_t lba) override {
        return data_.data() + static_cast<std::size_t>(lba) * block_size_;
    }

    std::uint64_t block_count() const override {
        return block_count_;
    }
    std::uint32_t block_size() const override {
        return block_size_;
    }

private:
    std::uint64_t block_count_;
    std::uint32_t block_size_;
    std::vector<std::uint8_t> data_;
};

} // namespace usbipdcpp
