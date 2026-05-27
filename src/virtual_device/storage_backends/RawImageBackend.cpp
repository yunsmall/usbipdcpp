#include "virtual_device/storage_backends/RawImageBackend.h"

#include <spdlog/spdlog.h>
#include <cstring>
#include <filesystem>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#include <mswsock.h>
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

    // 标记为稀疏文件，允许后续 punch_hole 释放空间
    DeviceIoControl(fh, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, nullptr, nullptr);

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

    // 获取文件系统块大小（用于 fallocate punch_hole 对齐）
    struct stat st{};
    if (fstat(fd, &st) == 0) {
        fs_block_size_ = st.st_blksize;
    }
    // 判断文件是否为新创建
    if (st.st_size > 0) {
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

#ifndef _WIN32
    if (pipe(splice_pipe_) < 0) {
        SPDLOG_WARN("pipe 创建失败，splice 零拷贝接收不可用");
    }
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
        close(splice_pipe_[0]);
        close(splice_pipe_[1]);
#endif
    }
}

void RawImageBackend::read(std::uint64_t lba, std::uint16_t count, void *buffer) {
    std::lock_guard lock(mutex_);
    auto total = static_cast<std::size_t>(count) * block_size_;
    auto offset = static_cast<std::size_t>(lba) * block_size_;
    std::memcpy(buffer, static_cast<const char *>(mapped_data_) + offset, total);
}

bool RawImageBackend::write(std::uint64_t lba, std::uint16_t count, const std::uint8_t *data) {
    std::lock_guard lock(mutex_);
    auto total = static_cast<std::size_t>(count) * block_size_;
    auto offset = static_cast<std::size_t>(lba) * block_size_;
    // 直接写入映射内存，OS 负责写回磁盘
    std::memcpy(static_cast<char *>(mapped_data_) + offset, data, total);
    return true;
}

void RawImageBackend::punch_hole(std::uint64_t lba, std::uint64_t count) {
    std::lock_guard lock(mutex_);
    auto offset = static_cast<std::size_t>(lba) * block_size_;
    auto length = static_cast<std::size_t>(count) * block_size_;
    // 清零映射内存：mmap 进程页表不感知 fallocate 打洞，必须手动清零
    std::memset(static_cast<char *>(mapped_data_) + offset, 0, length);
#ifdef _WIN32
    FILE_ZERO_DATA_INFORMATION zero{};
    zero.FileOffset.QuadPart = static_cast<LONGLONG>(offset);
    zero.BeyondFinalZero.QuadPart = static_cast<LONGLONG>(offset + length);
    DeviceIoControl(file_handle_, FSCTL_SET_ZERO_DATA, &zero, sizeof(zero), nullptr, 0, nullptr, nullptr);
#else
    // fallocate punch_hole 要求 offset 对齐到 fs 块边界，向下对齐并扩容覆盖整段
    auto aligned_off = (offset / fs_block_size_) * fs_block_size_;
    auto end = offset + length;
    auto aligned_end = ((end + fs_block_size_ - 1) / fs_block_size_) * fs_block_size_;
    if (fallocate(fd_, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                  static_cast<off_t>(aligned_off), static_cast<off_t>(aligned_end - aligned_off)) != 0) {
        SPDLOG_WARN("punch_hole 失败: LBA={} count={}", lba, count);
    }
#endif
}

bool RawImageBackend::recv_direct(std::uint64_t lba, std::size_t offset, std::size_t length,
                                 intptr_t sock_fd, std::error_code& ec) {
#ifdef _WIN32
    return false; // Windows 无 splice，回退 asio::read
#else
    if (splice_pipe_[0] < 0) return false;
    auto file_offset = static_cast<off64_t>(lba) * block_size_ + offset;
    size_t remaining = length;
    while (remaining > 0) {
        // sock → pipe
        ssize_t n = splice(static_cast<int>(sock_fd), nullptr,
                           splice_pipe_[1], nullptr, remaining, SPLICE_F_MOVE);
        if (n <= 0) {
            if (n == 0) break;
            ec.assign(errno, std::generic_category());
            return false;
        }
        // pipe → file（DMA 到页缓存，零用户态拷贝）
        off64_t off = static_cast<off64_t>(file_offset + (length - remaining));
        ssize_t m = splice(splice_pipe_[0], nullptr, fd_, &off, n, SPLICE_F_MOVE);
        if (m < 0) {
            ec.assign(errno, std::generic_category());
            return false;
        }
        remaining -= m;
    }
    return true;
#endif
}

void* RawImageBackend::get_direct_buffer(std::uint64_t lba) {
    if (!mapped_data_) return nullptr;
    return static_cast<char*>(mapped_data_) + static_cast<std::size_t>(lba) * block_size_;
}

bool RawImageBackend::send_direct(std::uint64_t lba, std::size_t offset, std::size_t length,
                                  intptr_t sock_fd, std::error_code& ec) {
    auto file_offset = static_cast<std::size_t>(lba) * block_size_ + offset;
#ifdef _WIN32
    auto hFile = static_cast<HANDLE>(file_handle_);
    auto hSocket = static_cast<SOCKET>(sock_fd);

    LARGE_INTEGER pos{};
    pos.QuadPart = static_cast<LONGLONG>(file_offset);
    if (!SetFilePointerEx(hFile, pos, nullptr, FILE_BEGIN)) {
        ec.assign(GetLastError(), std::system_category());
        return false;
    }
    if (!TransmitFile(hSocket, hFile, static_cast<DWORD>(length), 0, nullptr, nullptr, 0)) {
        DWORD err = GetLastError();
        if (err != WSA_IO_PENDING) {
            ec.assign(err, std::system_category());
            return false;
        }
    }
    return true;
#else
    // splice: file → pipe → sock（与 recv 共用 splice_pipe_）
    if (splice_pipe_[0] < 0) return false;
    off64_t off = static_cast<off64_t>(file_offset);
    size_t remaining = length;
    while (remaining > 0) {
        ssize_t n = splice(fd_, &off, splice_pipe_[1], nullptr, remaining, SPLICE_F_MOVE);
        if (n <= 0) {
            if (n == 0) break;
            ec.assign(errno, std::generic_category());
            return false;
        }
        ssize_t m = splice(splice_pipe_[0], nullptr, static_cast<int>(sock_fd), nullptr, n, SPLICE_F_MOVE);
        if (m < 0) {
            ec.assign(errno, std::generic_category());
            return false;
        }
        remaining -= m;
    }
    return true;
#endif
}

} // namespace usbipdcpp
