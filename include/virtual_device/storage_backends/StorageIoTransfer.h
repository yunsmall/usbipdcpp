#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace usbipdcpp {

/**
 * @brief MSC 存储 I/O 专用 Transfer，配合 StorageTransferOperator 使用
 *
 * 不分配内部 GenericTransfer::data，所有 I/O 通过本结构体的成员直达目标地址。
 * 支持两种数据路径：零拷贝（mmap 文件内存）和回退（staging_data_ / fallback_data）。
 *
 * === IN 方向（设备 → host，READ / CSW） ===
 * handle_bulk_transfer 中设置 external_buf 和 actual_length，sender 线程通过
 * send_transfer_data 发送。优先走 sendfile / TransmitFile（direct_io），回退 asio::write。
 * - READ mmap：external_buf 指向 RawImageBackend 映射的文件内存，direct_io = true
 * - READ 回退：external_buf 指向 staging_data_ 内部
 * - CSW：external_buf 指向本结构体 fallback_data，direct_io = false
 *
 * === OUT 方向（host → 设备，CBW / WRITE / UNMAP） ===
 * alloc_transfer_handle 时 prepare_out_buffer 设置 external_buf，
 * recv_transfer_data 优先 splice 直写文件（direct_io），回退 asio::read 到 external_buf，
 * 随后 on_out_data_received 解析 CBW 或累积写数据。
 * - CBW：external_buf 为 nullptr，走 fallback_data，direct_io = false
 * - WRITE mmap：external_buf 指向 mmap 文件内存，direct_io = true
 * - WRITE 回退 / UNMAP：external_buf 指向 staging_data_ 尾部预留空间
 */
struct StorageIoTransfer {
    // ===== 由 MscBulkOnlyHandler 设置 =====

    /**
     * @brief 直读/直写的目标地址
     *
     * IN(READ mmap)：指向 RawImageBackend 映射的文件内存；
     * IN(READ staging)：指向 staging_data_ 内部偏移；
     * IN(CSW)：指向 fallback_data 内 CSW 内容；
     * OUT(WRITE mmap)：指向 mmap 映射内存，socket 直读入或 splice 直写；
     * OUT(WRITE staging)：指向 staging_data_ 尾部预留空间；
     * OUT(Idle)：nullptr，CBW 走 fallback_data 读入。
     */
    void *external_buf = nullptr;

    /**
     * @brief IN 方向待发送的字节数
     *
     * MscBulkOnlyHandler 在 handle_bulk_transfer DataIn/Status 中设置，
     * 即 send_transfer_data 的 length 参数。OUT 方向未使用。
     */
    std::size_t actual_length = 0;

    /**
     * @brief 文件 I/O 的起始 LBA 编号
     *
     * send_direct / recv_direct 据此和 block_size 算出文件偏移。
     * IN(READ)：由 handle_bulk_transfer DataIn 设置为 read_lba_；
     * OUT(WRITE)：由 prepare_out_buffer 设置为 write_lba_；
     * CSW / CBW：保持 0（direct_io = false 时不触发此路径）。
     */
    std::uint64_t file_lba = 0;

    /**
     * @brief LBA 内的字节偏移
     *
     * 与 file_lba 配合确定精确的文件位置。
     * IN(READ)：即 staging_offset_；
     * OUT(WRITE)：即 write_accumulated_。
     */
    std::size_t file_offset = 0;

    /**
     * @brief 是否走零拷贝 sendfile / splice 路径
     *
     * true 仅当 external_buf 指向 RawImageBackend 的 mmap 文件内存时设置：
     * - IN(READ mmap)：handle_bulk_transfer DataIn 中设为 true；
     * - OUT(WRITE mmap)：prepare_out_buffer 中设为 true。
     *
     * CSW / CBW / staging 回退路径均为 false，send_transfer_data / recv_transfer_data
     * 据此跳过 send_direct / recv_direct，直接走 asio 读写。
     */
    bool direct_io = false;

    // ===== 本结构体自用缓冲区 =====

    /**
     * @brief external_buf 为空时的后备缓冲区
     *
     * - OUT(Idle)：CBW（31 字节）读入此处，再由 on_out_data_received memcpy 到
     *   current_cbw_，防止 CBW 过长损坏栈；
     * - IN(Status)：CSW（13 字节）构造于此，external_buf 指向其 data()。
     */
    std::vector<std::uint8_t> fallback_data;

    static StorageIoTransfer *from_handle(void *handle) {
        return static_cast<StorageIoTransfer *>(handle);
    }
};

} // namespace usbipdcpp
