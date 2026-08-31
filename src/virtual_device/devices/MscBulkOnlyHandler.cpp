#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_INFO

#include "usbipdcpp/virtual_device/devices/MscBulkOnlyHandler.h"

#include <algorithm>
#include <cstring>
#include <spdlog/spdlog.h>

#include "usbipdcpp/Session.h"
#include "usbipdcpp/SetupPacket.h"
#include "usbipdcpp/constant.h"
#include "usbipdcpp/virtual_device/VirtualDeviceHandler.h"
#include "usbipdcpp/virtual_device/storage_backends/StorageTransferOperator.h"

using namespace usbipdcpp;

static std::string wstr_to_ascii(const std::wstring &ws, const std::string &fallback) {
    std::string result;
    for (wchar_t c: ws) {
        if (c > 0 && c < 128)
            result += static_cast<char>(c);
    }
    return result.empty() ? fallback : result;
}

MscBulkOnlyHandler::MscBulkOnlyHandler(UsbInterface &handle_interface, StringPool &string_pool,
                                       std::unique_ptr<StorageBackend> backend, MscConfig config, bool read_only) :
    VirtualInterfaceHandler(handle_interface, string_pool, std::make_unique<StorageTransferOperator>(this)),
    backend_(std::move(backend)), read_only_(read_only), config_(std::move(config)) {
}

void MscBulkOnlyHandler::on_setup_interface_handlers() {
    if (config_.vendor.empty())
        config_.vendor = wstr_to_ascii(device_handler->get_string_manufacturer(), "USBIPDC ");
    if (config_.product.empty())
        config_.product = wstr_to_ascii(device_handler->get_string_product(), "USB Flash Drive ");
    if (config_.serial.empty())
        config_.serial = wstr_to_ascii(device_handler->get_string_serial(), "USBIPDCPSN");
    if (config_.revision.empty())
        config_.revision = "1.00";

    // 容量 > 2^32-1 块（2TB @ 512B）时 READ/WRITE/READ CAPACITY (10) 的 32 位 LBA
    // 无法寻址。本项目只提供 10 字节 CDB 的读写，超限只能报错提示用户缩容
    if (backend_ && backend_->block_count() > 0xFFFFFFFFull) {
        SPDLOG_ERROR("存储容量 {} 块（{} 字节）超过 2TB，10 字节 CDB 无法寻址，请缩小镜像",
                     backend_->block_count(), backend_->block_count() * backend_->block_size());
    }
}

void MscBulkOnlyHandler::on_new_connection(TransferResponder &current_session, error_code &ec) {
    VirtualInterfaceHandler::on_new_connection(current_session, ec);
    state_ = BotState::Idle;
    current_cbw_ = {};
    staging_data_.clear();
    staging_offset_ = 0;
    data_residue_ = 0;
    command_failed_ = false;
    data_out_unmap_ = false;
    data_out_write_same_ = false;
    read_mmap_base_ = nullptr;
    read_total_size_ = 0;
    write_mmap_base_ = nullptr;
    write_accumulated_ = 0;
}

void MscBulkOnlyHandler::on_disconnection(error_code &ec) {
    state_ = BotState::Idle;
    current_cbw_ = {};
    staging_data_.clear();
    staging_offset_ = 0;
    data_residue_ = 0;
    command_failed_ = false;
    data_out_unmap_ = false;
    data_out_write_same_ = false;
    read_mmap_base_ = nullptr;
    read_total_size_ = 0;
    write_mmap_base_ = nullptr;
    write_accumulated_ = 0;
    VirtualInterfaceHandler::on_disconnection(ec);
}

void MscBulkOnlyHandler::handle_non_standard_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                                      std::uint32_t transfer_flags,
                                                                      std::uint32_t transfer_buffer_length,
                                                                      const SetupPacket &setup_packet,
                                                                      TransferHandle transfer, std::error_code &ec) {
    responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
}

/** 为 OUT 传输提供目标缓冲区，由 StorageTransferOperator::alloc_transfer_handle 调用。
 *  Idle: CBW 走 fallback_data（返回 nullptr）
 *  DataOut: 写数据直入 mmap 或累积到 staging
 *  其他状态为协议异常，记录警告 */
void *MscBulkOnlyHandler::prepare_out_buffer(std::size_t length, StorageIoTransfer *trx) {
    SPDLOG_DEBUG("MSC::prepare_out len={} state={}", length, static_cast<int>(state_));
    switch (state_) {
        case BotState::Idle:
            SPDLOG_DEBUG("MSC::prepare_out CBW→nullptr");
            return nullptr; // CBW 走 fallback_data

        case BotState::DataOut:
            if (write_mmap_base_) {
                // 零拷贝 WRITE：socket 用 splice 直写入文件，仅回退时用 mmap 指针
                trx->direct_io = true;
                trx->file_lba = write_lba_;
                trx->file_offset = write_accumulated_;
                SPDLOG_DEBUG("MSC::prepare_out WRITE mmap lba={} offset={}", write_lba_, write_accumulated_);
                return static_cast<char *>(write_mmap_base_) + write_accumulated_;
            }
            // 非 mmap WRITE / UNMAP：socket 直读到 staging 尾部
            {
                auto old_size = staging_data_.size();
                staging_data_.resize(old_size + length);
                SPDLOG_DEBUG("MSC::prepare_out WRITE staging old={} new={}", old_size, old_size + length);
                return staging_data_.data() + old_size;
            }

        default:
            SPDLOG_WARN("MSC::prepare_out unexpected state={}", static_cast<int>(state_));
            return nullptr;
    }
}

/** BOT 协议状态机：CBW 解析 → DataIn/DataOut → CSW。
 *  OUT 数据经 prepare_out_buffer → recv_transfer_data → on_out_data_received 进入本函数。
 *  staging_data_ 清空延迟到下一个 CBW（Idle 分支），防止上一个 IN 传输的 sender 线程还在读 */
void MscBulkOnlyHandler::on_out_data_received(StorageIoTransfer *trx, std::size_t length) {
    SPDLOG_DEBUG("MSC::on_out_data_recv len={} state={}", length, static_cast<int>(state_));
    switch (state_) {
        case BotState::Idle: {
            // 上一个命令的 sender 线程已全部发完（否则 host 不会发新的 CBW），安全清空旧 staging
            staging_data_.clear();
            read_mmap_base_ = nullptr;
            read_total_size_ = 0;
            write_mmap_base_ = nullptr;
            write_accumulated_ = 0;

            // CBW 在 fallback_data 中
            if (trx->fallback_data.size() < sizeof(CBW)) {
                SPDLOG_ERROR("CBW 太短: {} 字节", trx->fallback_data.size());
                command_failed_ = true;
                state_ = BotState::Status;
                return;
            }
            std::memcpy(&current_cbw_, trx->fallback_data.data(), sizeof(CBW));

            if (current_cbw_.dCBWSignature != CBW_SIGNATURE) {
                SPDLOG_ERROR("无效 CBW 签名: 0x{:08X}", current_cbw_.dCBWSignature);
                command_failed_ = true;
                state_ = BotState::Status;
                return;
            }

            std::uint8_t cmd = current_cbw_.CBWCB[0];
            bool is_data_in = (current_cbw_.bmCBWFlags & 0x80) != 0;
            auto transfer_len = current_cbw_.dCBWDataTransferLength;

            SPDLOG_DEBUG("CBW cmd=0x{:02X} dir={} len={}", cmd, is_data_in ? "IN" : "OUT", transfer_len);

            switch (cmd) {
                case ScsiCmd::TestUnitReady:
                    command_failed_ = !(backend_ != nullptr);
                    state_ = BotState::Status;
                    break;

                case ScsiCmd::RequestSense: {
                    // REQUEST SENSE：固定格式，无错误时为 0x70 + 附加长度 10
                    SenseData sense{};
                    sense.valid_response_code = 0x70;
                    sense.additional_length = 10;
                    auto len = std::min(transfer_len, std::uint32_t(sizeof(SenseData)));
                    staging_offset_ = 0;
                    staging_data_.assign(reinterpret_cast<const std::uint8_t *>(&sense),
                                         reinterpret_cast<const std::uint8_t *>(&sense) + len);
                    state_ = BotState::DataIn;
                    break;
                }
                case ScsiCmd::Inquiry: {
                    // INQUIRY (标准 or VPD)
                    bool evpd = (current_cbw_.CBWCB[1] & 0x01) != 0;
                    std::uint8_t page = current_cbw_.CBWCB[2];
                    SPDLOG_DEBUG("INQUIRY evpd={} page=0x{:02X} len={}", evpd, page, transfer_len);
                    if (!evpd) {
                        // 标准 INQUIRY：vendor(8) + product(16) + revision(4) 来自 config_
                        auto pad = [](const std::string &s, std::size_t n) {
                            std::string r = s;
                            r.resize(n, ' '); // 不足补空格，超出截断
                            return r;
                        };
                        InquiryData inquiry{};
                        inquiry.rmb = 0x80; // 可移动介质
                        inquiry.version = 0x07; // SPC-4
                        inquiry.hisup_format = 0x12; // HiSup=1, Response Format=2
                        inquiry.additional_length = sizeof(InquiryData) - 5;
                        inquiry.cmdque = 0x02; // CmdQue=1（byte 7 bit1）
                        std::memcpy(inquiry.vendor_id, pad(config_.vendor, 8).c_str(), 8);
                        std::memcpy(inquiry.product_id, pad(config_.product, 16).c_str(), 16);
                        std::memcpy(inquiry.product_revision, pad(config_.revision, 4).c_str(), 4);
                        auto len = std::min(transfer_len, std::uint32_t(sizeof(InquiryData)));
                        staging_offset_ = 0;
                        staging_data_.assign(reinterpret_cast<const std::uint8_t *>(&inquiry),
                                             reinterpret_cast<const std::uint8_t *>(&inquiry) + len);
                    }
                    else if (page == 0x00) {
                        // Supported VPD Pages：0x00 0x80 0xB0 0xB1 0xB2
                        VpdSupportedPages vpd{};
                        vpd.page_length = 5;
                        vpd.pages[0] = 0x00;
                        vpd.pages[1] = 0x80;
                        vpd.pages[2] = 0xB0;
                        vpd.pages[3] = 0xB1;
                        vpd.pages[4] = 0xB2;
                        auto len = std::min(transfer_len, std::uint32_t(sizeof(VpdSupportedPages)));
                        staging_offset_ = 0;
                        staging_data_.assign(reinterpret_cast<const std::uint8_t *>(&vpd),
                                             reinterpret_cast<const std::uint8_t *>(&vpd) + len);
                    }
                    else if (page == 0x80) {
                        // Unit Serial Number，来自 config_.serial
                        VpdUnitSerialNumber vpd{};
                        vpd.page_code = 0x80;
                        auto sn_len = std::min<std::size_t>(config_.serial.size(), sizeof(vpd.serial));
                        vpd.page_length = static_cast<std::uint8_t>(sn_len);
                        std::memcpy(vpd.serial, config_.serial.data(), sn_len);
                        auto len = std::min(transfer_len, std::uint32_t(4 + sn_len));
                        staging_offset_ = 0;
                        staging_data_.assign(reinterpret_cast<const std::uint8_t *>(&vpd),
                                             reinterpret_cast<const std::uint8_t *>(&vpd) + len);
                    }
                    else if (page == 0xB0) {
                        // Block Device Characteristics：全零 = 非旋转介质、无特殊特性。
                        // （UNMAP 相关能力在 0xB1 Block Limits 中宣告）
                        VpdBlockDeviceCharacteristics vpd{};
                        vpd.page_code = 0xB0;
                        put_be16(vpd.page_length, sizeof(vpd.data));
                        auto len = std::min(transfer_len, std::uint32_t(sizeof(VpdBlockDeviceCharacteristics)));
                        staging_offset_ = 0;
                        staging_data_.assign(reinterpret_cast<const std::uint8_t *>(&vpd),
                                             reinterpret_cast<const std::uint8_t *>(&vpd) + len);
                    }
                    else if (page == 0xB1) {
                        // Block Limits (SBC-4)：宣告 UNMAP 与 WRITE SAME 能力，
                        // 主机 sd 层据此启用 trim / zeroout 路径
                        VpdBlockLimits vpd{};
                        vpd.page_code = 0xB1;
                        put_be16(vpd.page_length, 0x3C);
                        put_be32(vpd.max_unmap_lba_count, 65536); // 32 MiB
                        put_be32(vpd.max_unmap_block_desc_count, 64);
                        put_be32(vpd.opt_unmap_granularity, 8); // 4096 B
                        put_be32(vpd.unmap_granularity_alignment, 0x80000008); // bit31=UGAVALID
                        put_be32(vpd.max_write_same_length, 65535);
                        auto len = std::min(transfer_len, std::uint32_t(sizeof(VpdBlockLimits)));
                        staging_offset_ = 0;
                        staging_data_.assign(reinterpret_cast<const std::uint8_t *>(&vpd),
                                             reinterpret_cast<const std::uint8_t *>(&vpd) + len);
                    }
                    else if (page == 0xB2) {
                        // Logical Block Provisioning：宣告支持 UNMAP（LBPU=1）
                        VpdLogicalBlockProvisioning vpd{};
                        vpd.page_code = 0xB2;
                        put_be16(vpd.page_length, 0x0004);
                        vpd.lbpu = 0x80;
                        vpd.provisioning = 0x02;
                        auto len = std::min(transfer_len, std::uint32_t(sizeof(VpdLogicalBlockProvisioning)));
                        staging_offset_ = 0;
                        staging_data_.assign(reinterpret_cast<const std::uint8_t *>(&vpd),
                                             reinterpret_cast<const std::uint8_t *>(&vpd) + len);
                    }
                    else {
                        // 不支持的 VPD page — 回空
                        staging_offset_ = 0;
                        staging_data_.clear();
                    }
                    state_ = BotState::DataIn;
                    break;
                }
                case ScsiCmd::ModeSense6: {
                    // MODE SENSE (6)：4 字节模式头，无块描述符/页面
                    ModeSense6Data mode{};
                    mode.mode_data_length = sizeof(ModeSense6Data) - 1;
                    if (read_only_)
                        mode.wp = 0x80;
                    auto len = std::min(transfer_len, std::uint32_t(sizeof(ModeSense6Data)));
                    staging_offset_ = 0;
                    staging_data_.assign(reinterpret_cast<const std::uint8_t *>(&mode),
                                         reinterpret_cast<const std::uint8_t *>(&mode) + len);
                    state_ = BotState::DataIn;
                    break;
                }
                case ScsiCmd::PreventAllowMediumRemoval:
                    state_ = BotState::Status;
                    break;

                case ScsiCmd::ReadFormatCapacities: {
                    // READ FORMAT CAPACITIES（Windows 客户端会发）
                    ReadFormatCapacitiesData buf{};
                    auto blocks = backend_ ? backend_->block_count() : 0;
                    std::uint32_t bs = backend_ ? backend_->block_size() : 512;
                    put_be16(buf.list_length, 8); // 一个 8 字节描述符
                    put_be32(buf.capacity, static_cast<std::uint32_t>(blocks));
                    buf.format_type = 0x02; // formatted media
                    put_be24(buf.block_size, bs);
                    auto len = std::min(transfer_len, std::uint32_t(sizeof(ReadFormatCapacitiesData)));
                    staging_offset_ = 0;
                    staging_data_.assign(reinterpret_cast<const std::uint8_t *>(&buf),
                                         reinterpret_cast<const std::uint8_t *>(&buf) + len);
                    state_ = BotState::DataIn;
                    break;
                }

                case ScsiCmd::ReadCapacity16: {
                    // READ CAPACITY (16)
                    SPDLOG_DEBUG("READ CAPACITY (16)");
                    ReadCapacity16Data buf{};
                    put_be64(buf.last_lba, backend_ ? backend_->block_count() - 1 : 0);
                    put_be32(buf.block_size, backend_->block_size());
                    auto len = std::min(transfer_len, std::uint32_t(12)); // 低 12 字节即可（LBA+块大小）
                    staging_offset_ = 0;
                    staging_data_.assign(reinterpret_cast<const std::uint8_t *>(&buf),
                                         reinterpret_cast<const std::uint8_t *>(&buf) + len);
                    state_ = BotState::DataIn;
                    break;
                }
                case ScsiCmd::ReadCapacity10: {
                    // READ CAPACITY (10)
                    ReadCapacity10Data buf{};
                    put_be32(buf.last_lba, static_cast<std::uint32_t>(backend_ ? backend_->block_count() - 1 : 0));
                    put_be32(buf.block_size, backend_->block_size());
                    auto len = std::min(transfer_len, std::uint32_t(sizeof(ReadCapacity10Data)));
                    staging_offset_ = 0;
                    staging_data_.assign(reinterpret_cast<const std::uint8_t *>(&buf),
                                         reinterpret_cast<const std::uint8_t *>(&buf) + len);
                    state_ = BotState::DataIn;
                    break;
                }
                case ScsiCmd::Read10:
                case ScsiCmd::Write10: {
                    const auto *cdb = reinterpret_cast<const ReadWrite10Cdb *>(current_cbw_.CBWCB);
                    auto lba = get_be32(cdb->lba);
                    auto count = get_be16(cdb->block_count);
                    if (count == 0)
                        count = 256;

                    if (lba + count > (backend_ ? backend_->block_count() : 0)) {
                        SPDLOG_WARN("SCSI cmd 0x{:02X} LBA={} count={} 超出范围", cmd, lba, count);
                        command_failed_ = true;
                        state_ = BotState::Status;
                        break;
                    }

                    if (cmd == ScsiCmd::Read10) {
                        // READ：优先 mmap 直发（sendfile 路径），否则回退 staging
                        staging_offset_ = 0;
                        read_lba_ = lba;
                        read_mmap_base_ = backend_->get_direct_buffer(lba);
                        if (read_mmap_base_) {
                            read_total_size_ = static_cast<std::size_t>(count) * backend_->block_size();
                            staging_data_.clear();
                        }
                        else {
                            staging_data_.resize(read_total_size_ =
                                                         static_cast<std::size_t>(count) * backend_->block_size());
                            backend_->read(lba, count, staging_data_.data());
                        }
                        state_ = BotState::DataIn;
                    }
                    else if (read_only_) {
                        command_failed_ = true;
                        state_ = BotState::Status;
                    }
                    else {
                        // WRITE：优先 mmap 直写（socket 直读入 mmap），否则回退 staging
                        write_lba_ = lba;
                        write_count_ = count;
                        write_accumulated_ = 0;
                        write_mmap_base_ = backend_->get_direct_buffer(lba);
                        if (!write_mmap_base_) {
                            staging_data_.clear();
                            staging_data_.reserve(static_cast<std::size_t>(count) * backend_->block_size());
                        }
                        state_ = BotState::DataOut;
                    }
                    break;
                }
                case ScsiCmd::StartStopUnit:
                case ScsiCmd::Verify10:
                    state_ = BotState::Status;
                    break;
                case ScsiCmd::SynchronizeCache: {
                    // SYNCHRONIZE CACHE：虚拟设备没有写缓存，数据早已落盘，
                    // 直接成功（对齐内核 do_synchronize_cache）
                    state_ = BotState::Status;
                    break;
                }
                case ScsiCmd::ModeSense10: {
                    // MODE SENSE (10)：8 字节模式头，无块描述符/页面。
                    // WP 位位置与 6 字节版不同（对齐内核 do_mode_sense）
                    ModeSense10Data mode{};
                    put_be16(mode.mode_data_length, sizeof(ModeSense10Data) - 2);
                    if (read_only_)
                        mode.wp = 0x80;
                    auto len = std::min(transfer_len, std::uint32_t(sizeof(ModeSense10Data)));
                    staging_offset_ = 0;
                    staging_data_.assign(reinterpret_cast<const std::uint8_t *>(&mode),
                                         reinterpret_cast<const std::uint8_t *>(&mode) + len);
                    state_ = BotState::DataIn;
                    break;
                }
                case ScsiCmd::WriteSame10:
                case ScsiCmd::WriteSame16: {
                    // WRITE SAME：CDB[1] 位布局（SBC-3 rev 26+）：
                    //   bit7-5 = WRPROTECT、bit4 = ANCHOR、bit3 = UNMAP、
                    //   bit2 = PBDATA、bit1 = LBDATA、bit0 = NDOB
                    // UNMAP=1：无数据阶段，直接 punch_hole（trim）
                    // UNMAP=0：DATA-OUT 收 1 个逻辑块，用该数据填充整个 LBA 范围
                    // 块数 0 = 到介质末尾（与 READ/WRITE 10 的 0=256 块语义不同）
                    bool unmap = (current_cbw_.CBWCB[1] & 0x08) != 0;
                    std::uint64_t lba;
                    std::uint64_t cnt;
                    if (cmd == ScsiCmd::WriteSame10) {
                        const auto *cdb = reinterpret_cast<const WriteSame10Cdb *>(current_cbw_.CBWCB);
                        lba = get_be32(cdb->lba);
                        cnt = get_be16(cdb->block_count);
                    }
                    else {
                        const auto *cdb = reinterpret_cast<const WriteSame16Cdb *>(current_cbw_.CBWCB);
                        lba = get_be64(cdb->lba);
                        cnt = get_be32(cdb->block_count);
                    }
                    auto blocks = backend_ ? backend_->block_count() : 0;
                    if (lba >= blocks) {
                        SPDLOG_WARN("WRITE SAME LBA={} 超出范围", lba);
                        command_failed_ = true;
                        state_ = BotState::Status;
                        break;
                    }
                    if (cnt == 0)
                        cnt = blocks - lba; // 0 = 直到介质末尾
                    if (read_only_ || cnt > blocks - lba) {
                        SPDLOG_WARN("WRITE SAME LBA={} cnt={} 超出范围或只读", lba, cnt);
                        command_failed_ = true;
                        state_ = BotState::Status;
                        break;
                    }
                    SPDLOG_DEBUG("WRITE SAME cmd=0x{:02X} unmap={} lba={} cnt={}", cmd, unmap, lba, cnt);
                    if (unmap) {
                        backend_->punch_hole(lba, cnt);
                        state_ = BotState::Status;
                    }
                    else {
                        write_same_lba_ = lba;
                        write_same_count_ = cnt;
                        data_out_write_same_ = true;
                        staging_offset_ = 0;
                        staging_data_.clear();
                        // 主机应传 1 个逻辑块，多余字节视为协议偏差丢弃
                        data_residue_ = transfer_len > backend_->block_size() ? transfer_len - backend_->block_size() : 0;
                        state_ = BotState::DataOut;
                    }
                    break;
                }
                case ScsiCmd::AtaPassThrough:
                    command_failed_ = true;
                    state_ = BotState::Status;
                    break;
                case ScsiCmd::Unmap: {
                    // UNMAP，数据长度以 CBW.dCBWDataTransferLength 为准（某些内核 CDB 参数长度为 0）
                    auto data_len = current_cbw_.dCBWDataTransferLength;
                    SPDLOG_DEBUG("UNMAP CBW tag=0x{:08X} dataLen={}", current_cbw_.dCBWTag, data_len);
                    if (read_only_) {
                        command_failed_ = true;
                        state_ = BotState::Status;
                        break;
                    }
                    if (data_len == 0) {
                        state_ = BotState::Status;
                        break;
                    }
                    write_count_ = data_len;
                    data_out_unmap_ = true;
                    staging_offset_ = 0;
                    staging_data_.clear();
                    state_ = BotState::DataOut;
                    break;
                }
                default:
                    SPDLOG_WARN("不支持的 SCSI 命令: 0x{:02X}", cmd);
                    command_failed_ = true;
                    state_ = BotState::Status;
                    break;
            }
            break;
        }

        case BotState::DataOut: {
            if (data_out_unmap_) {
                if (staging_data_.size() >= write_count_) {
                    auto &d = staging_data_;
                    for (std::size_t i = 8; i + 16 <= d.size(); i += 16) {
                        const auto *desc = reinterpret_cast<const UnmapBlockDescriptor *>(&d[i]);
                        auto lba = get_be64(desc->lba);
                        auto cnt = get_be32(desc->block_count);
                        SPDLOG_DEBUG("UNMAP punch lba={} cnt={}", lba, cnt);
                        backend_->punch_hole(lba, cnt);
                    }
                    staging_data_.clear();
                    data_out_unmap_ = false;
                    data_residue_ = 0;
                    state_ = BotState::Status;
                }
            }
            else if (data_out_write_same_) {
                // WRITE SAME 填充：收满 1 个逻辑块后逐块写入整个范围。
                // 填充数据只有 1 块，而 write() 的 data 缓冲需完整 count 块，
                // 故每次只写 1 块（不可批量，否则越界读）
                auto bs = backend_->block_size();
                if (staging_data_.size() >= bs) {
                    auto lba = write_same_lba_;
                    auto cnt = write_same_count_;
                    while (cnt > 0) {
                        backend_->write(lba, 1, staging_data_.data());
                        lba += 1;
                        cnt -= 1;
                    }
                    staging_data_.clear();
                    data_out_write_same_ = false;
                    state_ = BotState::Status;
                }
            }
            else if (write_mmap_base_) {
                // 零拷贝 WRITE：数据已直读入 mmap，叠加偏移
                write_accumulated_ += length;
                if (write_accumulated_ >= static_cast<std::size_t>(write_count_) * backend_->block_size()) {
                    write_mmap_base_ = nullptr;
                    write_accumulated_ = 0;
                    data_residue_ = 0;
                    state_ = BotState::Status;
                }
            }
            else {
                // 非 mmap WRITE 回退：累积 staging 后写盘
                if (write_lba_ + write_count_ <= backend_->block_count()) {
                    if (staging_data_.size() >= static_cast<std::size_t>(write_count_) * backend_->block_size()) {
                        backend_->write(write_lba_, write_count_, staging_data_.data());
                        staging_data_.clear();
                        data_residue_ = 0;
                        state_ = BotState::Status;
                    }
                }
            }
            break;
        }

        default:
            break;
    }
}

void MscBulkOnlyHandler::handle_bulk_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                                              std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                              std::error_code &ec) {
    SPDLOG_DEBUG("BULK {} ep={:02x} len={} state={}", ep.is_in() ? "IN" : "OUT", ep.address, transfer_buffer_length,
                 static_cast<int>(state_));

    if (ep.is_in()) {
        switch (state_) {
            case BotState::DataIn: {
                auto total = read_mmap_base_ ? read_total_size_ : staging_data_.size();
                auto remaining = total - staging_offset_;
                auto len = std::min(static_cast<std::size_t>(transfer_buffer_length), remaining);
                if (len > 0) {
                    auto *trx = StorageIoTransfer::from_handle(transfer.get());
                    if (read_mmap_base_) {
                        // 零拷贝发送：external_buf 直指 mmap，file_lba/file_offset 供 send_direct
                        trx->direct_io = true;
                        trx->external_buf = static_cast<char *>(read_mmap_base_) + staging_offset_;
                        trx->file_lba = read_lba_;
                        trx->file_offset = staging_offset_;
                        SPDLOG_DEBUG("MSC::hb IN mmap handle={:p} lba={} offset={} len={}",
                                     static_cast<const void *>(transfer.get()), read_lba_, staging_offset_, len);
                    }
                    else {
                        trx->external_buf = staging_data_.data() + staging_offset_;
                        SPDLOG_DEBUG("MSC::hb IN staging handle={:p} offset={} len={}",
                                     static_cast<const void *>(transfer.get()), staging_offset_, len);
                    }
                    trx->actual_length = len;
                    staging_offset_ += len;
                    responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                            seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK),
                            static_cast<std::uint32_t>(len), std::move(transfer)));
                }
                else {
                    responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(
                            seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK), 0));
                }
                // 全部发完 → Status，不清 staging/mmap（sender 还在排队发送）
                if (staging_offset_ >= total) {
                    staging_offset_ = 0;
                    data_residue_ = 0;
                    state_ = BotState::Status;
                }
                break;
            }

            case BotState::Status: {
                CSW csw{};
                csw.dCSWSignature = CSW_SIGNATURE;
                csw.dCSWTag = current_cbw_.dCBWTag;
                if (command_failed_) {
                    // 对齐内核 fsg：失败时 residue = 应传未传字节数。本项目失败
                    // 均发生在数据阶段前（实际传了 0 字节），故 = dCBWDataTransferLength
                    csw.dCSWDataResidue = current_cbw_.dCBWDataTransferLength;
                    csw.bCSWStatus = 1;
                    command_failed_ = false;
                }
                else {
                    csw.dCSWDataResidue = data_residue_;
                }

                auto *trx = StorageIoTransfer::from_handle(transfer.get());
                SPDLOG_DEBUG("MSC::hb Status handle={:p} CSW_tag=0x{:08X} status={}",
                             static_cast<const void *>(transfer.get()), csw.dCSWTag, csw.bCSWStatus);
                trx->fallback_data.resize(sizeof(CSW));
                std::memcpy(trx->fallback_data.data(), &csw, sizeof(CSW));
                trx->external_buf = trx->fallback_data.data();
                trx->actual_length = sizeof(CSW);
                responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                        seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK), sizeof(CSW), std::move(transfer)));
                // CSW 入队后切回 Idle，旧 staging 延迟到下一个 CBW 的 Idle 分支清空，
                // 保证 sender 线程有足够时间消费完外部指针（avoid use-after-free）
                state_ = BotState::Idle;
                break;
            }

            default:
                send_stall(seqnum);
                break;
        }
    }
    else {
        SPDLOG_DEBUG("MSC::hb OUT drop handle={:p}", static_cast<const void *>(transfer.get()));
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(
                seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK), transfer_buffer_length));
    }
}

void MscBulkOnlyHandler::send_stall(std::uint32_t seqnum) {
    responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
}
