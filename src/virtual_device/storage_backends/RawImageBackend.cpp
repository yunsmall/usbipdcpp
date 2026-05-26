#include "virtual_device/storage_backends/RawImageBackend.h"

#include <spdlog/spdlog.h>
#include <cstring>
#include <filesystem>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace usbipdcpp {

RawImageBackend::RawImageBackend(std::string path, std::uint64_t initial_blocks, std::uint32_t block_size) :
    path_(std::move(path)), block_count_(initial_blocks), block_size_(block_size) {

    SPDLOG_INFO("磁盘镜像路径: {}", std::filesystem::absolute(path_).string());

    bool is_new_file = false;
    auto file_size = static_cast<std::size_t>(block_count_) * block_size_;

#ifdef _WIN32
    // 打开已有文件或创建新文件（OPEN_ALWAYS：存在则打开，不存在则创建）
    HANDLE fh = CreateFileA(path_.c_str(), GENERIC_READ | GENERIC_WRITE,
                            FILE_SHARE_READ, nullptr,
                            OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (fh == INVALID_HANDLE_VALUE) {
        SPDLOG_ERROR("无法打开/创建文件: {}", path_);
        return;
    }

    // 判断文件是否为新创建：已有文件大小 > 0 则使用实际大小
    LARGE_INTEGER existing_size;
    if (GetFileSizeEx(fh, &existing_size) && existing_size.QuadPart > 0) {
        auto exist_blocks = static_cast<std::uint64_t>(existing_size.QuadPart) / block_size_;
        if (exist_blocks > 0) {
            block_count_ = exist_blocks;
            file_size = static_cast<std::size_t>(existing_size.QuadPart);
        }
    }
    else {
        // 新文件：扩展文件到目标大小
        is_new_file = true;
        LARGE_INTEGER target;
        target.QuadPart = static_cast<LONGLONG>(file_size);
        SetFilePointerEx(fh, target, nullptr, FILE_BEGIN);
        SetEndOfFile(fh);
    }

    // 创建文件映射对象（dwMaximumSizeHigh:dwMaximumSizeLow 为 64 位大小）
    HANDLE mh = CreateFileMappingA(fh, nullptr, PAGE_READWRITE,
                                   static_cast<DWORD>(file_size >> 32),
                                   static_cast<DWORD>(file_size), nullptr);
    if (!mh) {
        SPDLOG_ERROR("CreateFileMapping 失败");
        CloseHandle(fh);
        return;
    }

    // 将文件映射到进程地址空间
    void *addr = MapViewOfFile(mh, FILE_MAP_ALL_ACCESS, 0, 0, file_size);
    if (!addr) {
        SPDLOG_ERROR("MapViewOfFile 失败");
        CloseHandle(mh);
        CloseHandle(fh);
        return;
    }

    file_handle_ = fh;
    mapping_handle_ = mh;
    mapped_data_ = addr;
    mapped_size_ = file_size;

#else
    // 打开已有文件，不存在则创建
    int fd = open(path_.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        SPDLOG_ERROR("无法打开/创建文件: {}", path_);
        return;
    }

    // 判断文件是否为新创建
    struct stat st{};
    if (fstat(fd, &st) == 0 && st.st_size > 0) {
        auto exist_blocks = static_cast<std::uint64_t>(st.st_size) / block_size_;
        if (exist_blocks > 0) {
            block_count_ = exist_blocks;
            file_size = static_cast<std::size_t>(st.st_size);
        }
    }
    else {
        // 新文件：截断到目标大小（文件原有内容会被填零）
        is_new_file = true;
        if (ftruncate(fd, static_cast<off_t>(file_size)) != 0) {
            SPDLOG_ERROR("ftruncate 失败");
            close(fd);
            return;
        }
    }

    // MAP_SHARED：写入映射区的数据会由内核异步写回磁盘
    void *addr = mmap(nullptr, file_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED) {
        SPDLOG_ERROR("mmap 失败");
        close(fd);
        return;
    }

    fd_ = fd;
    mapped_data_ = addr;
    mapped_size_ = file_size;
#endif

    SPDLOG_INFO("{}镜像: {} ({} 块, {} MiB)",
                is_new_file ? "创建" : "打开",
                path_, block_count_, block_count_ * block_size_ / 1024 / 1024);
}

RawImageBackend::~RawImageBackend() {
    if (mapped_data_) {
#ifdef _WIN32
        UnmapViewOfFile(mapped_data_);
        CloseHandle(mapping_handle_);
        CloseHandle(file_handle_);
#else
        munmap(mapped_data_, mapped_size_);
        close(fd_);
#endif
    }
}

std::vector<std::uint8_t> RawImageBackend::read(std::uint64_t lba, std::uint16_t count) {
    std::lock_guard lock(mutex_);
    auto total = static_cast<std::size_t>(count) * block_size_;
    auto offset = static_cast<std::size_t>(lba) * block_size_;
    std::vector<std::uint8_t> data(total);
    std::memcpy(data.data(), static_cast<const char *>(mapped_data_) + offset, total);
    return data;
}

bool RawImageBackend::write(std::uint64_t lba, std::uint16_t count, const std::uint8_t *data) {
    std::lock_guard lock(mutex_);
    auto total = static_cast<std::size_t>(count) * block_size_;
    auto offset = static_cast<std::size_t>(lba) * block_size_;
    // 直接写入映射内存，OS 负责写回磁盘
    std::memcpy(static_cast<char *>(mapped_data_) + offset, data, total);
    return true;
}

} // namespace usbipdcpp
