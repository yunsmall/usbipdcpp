#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "usbipdcpp/virtual_device/MscConstants.h"
#include "usbipdcpp/virtual_device/VirtualInterfaceHandler.h"
#include "usbipdcpp/virtual_device/storage_backends/StorageBackend.h"
#include "usbipdcpp/virtual_device/storage_backends/StorageIoTransfer.h"

namespace usbipdcpp {

class StorageTransferOperator;

/** SCSI INQUIRY / VPD 返回的标识字符串。
 *  空字符串表示从 VirtualDeviceHandler 的 USB 描述符自动读取。 */
struct MscConfig {
    std::string vendor; // INQUIRY 8 字节厂商名
    std::string product; // INQUIRY 16 字节产品名
    std::string revision; // INQUIRY 4 字节版本号
    std::string serial; // VPD 0x80 序列号
};

/** MSC Bulk-Only Transport 协议处理器。
 *
 * BOT 是同步协议（CBW→Data→CSW），所有 IN 数据在收到 CBW 时已就绪，
 * 主机 IN 请求立即可响应，因此无需 EndpointRequestQueue（对比 HID/CDC ACM
 * 等异步产生数据的设备，需要队列暂存 IN 请求等待数据就绪）。 */
class USBIPDCPP_API MscBulkOnlyHandler : public VirtualInterfaceHandler {
public:
    MscBulkOnlyHandler(UsbInterface &handle_interface, StringPool &string_pool, std::unique_ptr<StorageBackend> backend,
                       MscConfig config = {}, bool read_only = false);

    /**
     * @brief 创建 USB Mass Storage 批量传输接口（描述符模板，未绑定 handler）
     *
     * 接口定义：Mass Storage 类、SCSI transparent 子类、Bulk-Only 协议
     * （08/06/50），一个 Bulk IN + 一个 Bulk OUT 端点（High speed，mps=512）。
     * @param in_ep Bulk IN 端点地址（设备→主机，命令/数据响应）
     * @param out_ep Bulk OUT 端点地址（主机→设备，CBW/数据）
     * @return 未绑定 handler 的 UsbInterface
     */
    static UsbInterface make_interface(std::uint8_t in_ep, std::uint8_t out_ep) {
        UsbInterface i{
                .interface_class = static_cast<std::uint8_t>(ClassCode::MassStorage),
                .interface_subclass = 0x06, // SCSI transparent
                .interface_protocol = 0x50, // Bulk-Only Transport
                .endpoints = {{UsbEndpoint{.address = in_ep,
                                           .attributes = static_cast<std::uint8_t>(EndpointAttributes::Bulk),
                                           .max_packet_size = 512,
                                           .interval = 0},
                               UsbEndpoint{.address = out_ep,
                                           .attributes = static_cast<std::uint8_t>(EndpointAttributes::Bulk),
                                           .max_packet_size = 512,
                                           .interval = 0}}},
        };
        return i;
    }

    /** Bulk-Only Transport 核心：IN 数据发送（DataIn/Status），OUT 仅 ack（数据已在 on_out_data_received 处理） */
    void handle_bulk_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                              std::uint32_t transfer_buffer_length, TransferHandle transfer,
                              std::error_code &ec) override;

    void handle_non_standard_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                      std::uint32_t transfer_flags,
                                                      std::uint32_t transfer_buffer_length,
                                                      const SetupPacket &setup_packet, TransferHandle transfer,
                                                      std::error_code &ec) override;

    /** 为 OUT 传输提供目标缓冲区（Idle→fallback / DataOut→mmap或staging） */
    void *prepare_out_buffer(std::size_t length, StorageIoTransfer *trx);
    /** OUT 数据收完后回调，驱动 BOT 状态机：CBW 解析 or 写盘 or UNMAP */
    void on_out_data_received(StorageIoTransfer *trx, std::size_t length);

    StorageBackend *get_backend() const {
        return backend_.get();
    }

    /** device_handler 已设置后回调，从 USB 字符串补全 MscConfig 空字段 */
    void on_setup_interface_handlers() override;
    /** 客户端连接时重置 BOT 状态机 */
    void on_new_connection(TransferResponder &current_session, error_code &ec) override;
    /** 客户端断开时重置 BOT 状态机 */
    void on_disconnection(error_code &ec) override;

private:
    std::unique_ptr<StorageBackend> backend_;
    bool read_only_ = false;
    MscConfig config_; // on_setup_interface_handlers 中补全空字段

    /** BOT 状态机：Idle → DataIn/DataOut → Status → Idle */
    BotState state_ = BotState::Idle;
    /** 最近收到的 CBW，CSW 需原样回传 dCBWTag */
    CBW current_cbw_{};

    /** IN/OUT 数据暂存或零拷贝 mmap 指针。
     *  清空时机延迟到下一个 CBW（Idle 分支），防止上一命令的 sender 线程还在读 */
    std::vector<std::uint8_t> staging_data_;
    std::size_t staging_offset_ = 0; // IN 传输时已发送的字节数（staging / mmap 共用）

    /** 传输差额：dCBWDataTransferLength - 实际收发字节数，CSW 需要此值 */
    std::uint32_t data_residue_ = 0;
    /** CBW 解析阶段设为 true，Status 发 CSW 后清除 */
    bool command_failed_ = false;

    /** 最近一次失败命令的 sense（REQUEST SENSE 返回；新命令开始时清零）。
     *  SBC/SPC 要求 CHECK CONDITION 后主机能通过 REQUEST SENSE 拿到原因 */
    std::uint8_t sense_key_ = 0;
    std::uint8_t sense_asc_ = 0;
    std::uint8_t sense_ascq_ = 0;

    /** WRITE 命令的目标 LBA 和块数（10 字节 CDB，LBA 为 32 位） */
    std::uint64_t write_lba_ = 0;
    std::uint16_t write_count_ = 0;
    /** true 时 DataOut 走 UNMAP 描述符解析，不走 WRITE 写盘 */
    bool data_out_unmap_ = false;

    /** WRITE SAME 填充写：目标 LBA、块数（16 字节 CDB 可达 32 位块） */
    std::uint64_t write_same_lba_ = 0;
    std::uint64_t write_same_count_ = 0;
    /** true 时 DataOut 走 WRITE SAME 填充模式（收 1 块数据写满整个范围） */
    bool data_out_write_same_ = false;

    /** READ 零拷贝：mmap 首地址、起始 LBA、总字节数 */
    std::uint64_t read_lba_ = 0;
    void *read_mmap_base_ = nullptr;
    std::size_t read_total_size_ = 0;
    /** WRITE 零拷贝：mmap 首地址、已收字节数 */
    void *write_mmap_base_ = nullptr;
    std::size_t write_accumulated_ = 0;

    void send_stall(std::uint32_t seqnum);

    /** 记录失败命令的 sense，REQUEST SENSE 原样返回 */
    void set_sense(std::uint8_t key, std::uint8_t asc, std::uint8_t ascq = 0) {
        sense_key_ = key;
        sense_asc_ = asc;
        sense_ascq_ = ascq;
    }

public:
    // ========== 标准请求默认实现 ==========
    // 端点 feature/status 交给基类的 ENDPOINT_HALT 状态机（USB 2.0 §9.4.5
    // 要求中断/批量端点必须实现 Halt）；接口级 feature 基类默认 STALL；
    // SET/GET_INTERFACE 用基类实现——MSC 接口只有 alt 0，基类会拒绝其他
    // alt（USB 2.0 §9.4.10：不存在的 alternate setting 回 Request Error），
    // 不能无条件回成功

    std::uint16_t request_get_status(std::uint32_t *p_status) override {
        *p_status = 0;
        return 0;
    }

    [[nodiscard]] data_type get_class_specific_descriptor() override {
        return {};
    }
};

} // namespace usbipdcpp
