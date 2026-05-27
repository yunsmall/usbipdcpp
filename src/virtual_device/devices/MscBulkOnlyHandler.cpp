#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_INFO

#include "virtual_device/devices/MscBulkOnlyHandler.h"

#include <spdlog/spdlog.h>
#include <cstring>
#include <algorithm>

#include "Session.h"
#include "SetupPacket.h"
#include "constant.h"
#include "virtual_device/storage_backends/StorageTransferOperator.h"
#include "virtual_device/VirtualDeviceHandler.h"

using namespace usbipdcpp;

static std::string wstr_to_ascii(const std::wstring &ws, const std::string &fallback) {
    std::string result;
    for (wchar_t c : ws) {
        if (c > 0 && c < 128) result += static_cast<char>(c);
    }
    return result.empty() ? fallback : result;
}

MscBulkOnlyHandler::MscBulkOnlyHandler(UsbInterface &handle_interface, StringPool &string_pool,
                                       std::unique_ptr<StorageBackend> backend, MscConfig config,
                                       bool read_only) :
    VirtualInterfaceHandler(handle_interface, string_pool,
                            std::make_unique<StorageTransferOperator>(this)),
    backend_(std::move(backend)), read_only_(read_only), config_(std::move(config)) {
}

void MscBulkOnlyHandler::on_setup_interface_handlers() {
    if (config_.vendor.empty())
        config_.vendor = wstr_to_ascii(device_handler->get_string_manufacturer(), "USBIPDC ");
    if (config_.product.empty())
        config_.product = wstr_to_ascii(device_handler->get_string_product(), "USB Flash Drive ");
    if (config_.serial.empty())
        config_.serial = wstr_to_ascii(device_handler->get_string_serial(), "USBIPDCPSN");
    if (config_.revision.empty()) config_.revision = "1.00";
}

void MscBulkOnlyHandler::on_new_connection(Session &current_session, error_code &ec) {
    VirtualInterfaceHandler::on_new_connection(current_session, ec);
    state_ = BotState::Idle;
    current_cbw_ = {};
    staging_data_.clear();
    staging_offset_ = 0;
    data_residue_ = 0;
    command_failed_ = false;
    data_out_unmap_ = false;
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
    read_mmap_base_ = nullptr;
    read_total_size_ = 0;
    write_mmap_base_ = nullptr;
    write_accumulated_ = 0;
    VirtualInterfaceHandler::on_disconnection(ec);
}

void MscBulkOnlyHandler::handle_non_standard_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep,
        std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet,
        TransferHandle transfer, std::error_code &ec) {
    session->submit_ret_submit(
            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
}

/** 为 OUT 传输提供目标缓冲区，由 StorageTransferOperator::alloc_transfer_handle 调用。
 *  Idle: CBW 走 fallback_data（返回 nullptr）
 *  DataOut: 写数据直入 mmap 或累积到 staging
 *  其他状态为协议异常，记录警告 */
void* MscBulkOnlyHandler::prepare_out_buffer(std::size_t length, StorageIoTransfer* trx) {
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
                SPDLOG_DEBUG("MSC::prepare_out WRITE mmap lba={} offset={}",
                             write_lba_, write_accumulated_);
                return static_cast<char*>(write_mmap_base_) + write_accumulated_;
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
void MscBulkOnlyHandler::on_out_data_received(StorageIoTransfer* trx, std::size_t length) {
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
                case 0x00:
                    command_failed_ = !(backend_ != nullptr);
                    state_ = BotState::Status;
                    break;

                case 0x03: { // REQUEST SENSE
                    std::uint8_t sense[18] = {};
                    sense[0] = 0x70; sense[7] = 10;
                    auto len = std::min(transfer_len, std::uint32_t(18));
                    staging_offset_ = 0;
                    staging_data_ = std::vector<std::uint8_t>(sense, sense + len);
                    state_ = BotState::DataIn;
                    break;
                }
                case 0x12: { // INQUIRY (标准 or VPD)
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
                        std::uint8_t inquiry[36] = {};
                        inquiry[0] = 0x00; inquiry[1] = 0x80; inquiry[2] = 0x07;
                        inquiry[3] = 0x12; // HiSup=1, Response Format=2 (SPC-4)
                        inquiry[4] = 31;
                        inquiry[6] |= 0x02; // CmdQue=1
                        std::memcpy(inquiry + 8, pad(config_.vendor, 8).c_str(), 8);
                        std::memcpy(inquiry + 16, pad(config_.product, 16).c_str(), 16);
                        std::memcpy(inquiry + 32, pad(config_.revision, 4).c_str(), 4);
                        auto len = std::min(transfer_len, std::uint32_t(36));
                        staging_offset_ = 0;
                        staging_data_ = std::vector<std::uint8_t>(inquiry, inquiry + len);
                    } else if (page == 0x00) {
                        // Supported VPD Pages：0x00 0x80 0xB0 0xB2
                        std::vector<std::uint8_t> vpd{0x00, 0x00, 0x00, 0x04, 0x00, 0x80, 0xB0, 0xB2};
                        auto len = std::min(transfer_len, std::uint32_t(vpd.size()));
                        staging_offset_ = 0;
                        staging_data_ = std::vector<std::uint8_t>(vpd.begin(), vpd.begin() + len);
                    } else if (page == 0x80) {
                        // Unit Serial Number，来自 config_.serial
                        const auto &sn = config_.serial;
                        std::vector<std::uint8_t> vpd{0x00, 0x80, 0x00, static_cast<std::uint8_t>(sn.size())};
                        vpd.insert(vpd.end(), sn.begin(), sn.end());
                        auto len = std::min(transfer_len, std::uint32_t(vpd.size()));
                        staging_offset_ = 0;
                        staging_data_ = std::vector<std::uint8_t>(vpd.begin(), vpd.begin() + len);
                    } else if (page == 0xB0) {
                        // Block Limits VPD: 告之最大 UNMAP LBA 数和粒度
                        std::vector<std::uint8_t> vpd(64, 0);
                        vpd[0] = 0x00; vpd[1] = 0xB0; vpd[2] = 0x00; vpd[3] = 0x3C;
                        // max_unmap_lba_count (bytes 20-23, big-endian): 65536 LBAs = 32 MiB
                        vpd[21] = 0x01;
                        // max_unmap_block_desc_count (bytes 24-27, big-endian): 64
                        vpd[27] = 64;
                        // optimal_unmap_granularity (bytes 28-31, big-endian): 8 LBAs = 4096 B
                        vpd[31] = 8;
                        // unmap_granularity_alignment (bytes 32-35, big-endian), bit31=UGAVALID
                        vpd[32] = 0x80; vpd[35] = 8;
                        auto len = std::min(transfer_len, std::uint32_t(vpd.size()));
                        staging_offset_ = 0;
                        staging_data_ = std::vector<std::uint8_t>(vpd.begin(), vpd.begin() + len);
                    } else if (page == 0xB2) {
                        // Logical Block Provisioning: 宣告支持 UNMAP
                        std::vector<std::uint8_t> vpd{0x00, 0xB2, 0x00, 0x04, 0x00, 0x80, 0x02, 0x00};
                        auto len = std::min(transfer_len, std::uint32_t(vpd.size()));
                        staging_offset_ = 0;
                        staging_data_ = std::vector<std::uint8_t>(vpd.begin(), vpd.begin() + len);
                    } else {
                        // 不支持的 VPD page — 回空
                        staging_offset_ = 0;
                        staging_data_.clear();
                    }
                    state_ = BotState::DataIn;
                    break;
                }
                case 0x1A: { // MODE SENSE (6)
                    std::uint8_t mode[4] = {};
                    mode[0] = 3;
                    if (read_only_) mode[2] = 0x80;
                    auto len = std::min(transfer_len, std::uint32_t(4));
                    staging_offset_ = 0;
                    staging_data_ = std::vector<std::uint8_t>(mode, mode + len);
                    state_ = BotState::DataIn;
                    break;
                }
                case 0x1E:
                    state_ = BotState::Status;
                    break;

                case 0x23: { // READ FORMAT CAPACITIES（Windows 客户端会发）
                    auto blocks = backend_ ? backend_->block_count() : 0;
                    std::uint32_t bs = backend_ ? backend_->block_size() : 512;
                    std::uint8_t buf[12] = {};
                    buf[3] = 8; // 一个 8 字节描述符
                    buf[4] = (blocks >> 24) & 0xFF;
                    buf[5] = (blocks >> 16) & 0xFF;
                    buf[6] = (blocks >> 8) & 0xFF;
                    buf[7] = blocks & 0xFF;
                    buf[8] = 0x02; // formatted media
                    buf[9] = (bs >> 16) & 0xFF;
                    buf[10] = (bs >> 8) & 0xFF;
                    buf[11] = bs & 0xFF;
                    auto len = std::min(transfer_len, std::uint32_t(12));
                    staging_offset_ = 0;
                    staging_data_ = std::vector<std::uint8_t>(buf, buf + len);
                    state_ = BotState::DataIn;
                    break;
                }

                case 0x9E: { // READ CAPACITY (16)
                    SPDLOG_DEBUG("READ CAPACITY (16)");
                    auto last_lba = backend_ ? backend_->block_count() - 1 : 0;
                    std::uint32_t bs = backend_->block_size();
                    std::uint8_t buf[12] = {};
                    buf[0] = (last_lba >> 56) & 0xFF;
                    buf[1] = (last_lba >> 48) & 0xFF;
                    buf[2] = (last_lba >> 40) & 0xFF;
                    buf[3] = (last_lba >> 32) & 0xFF;
                    buf[4] = (last_lba >> 24) & 0xFF;
                    buf[5] = (last_lba >> 16) & 0xFF;
                    buf[6] = (last_lba >> 8) & 0xFF;
                    buf[7] = last_lba & 0xFF;
                    buf[8] = (bs >> 24) & 0xFF;
                    buf[9] = (bs >> 16) & 0xFF;
                    buf[10] = (bs >> 8) & 0xFF;
                    buf[11] = bs & 0xFF;
                    auto len = std::min(transfer_len, std::uint32_t(12));
                    staging_offset_ = 0;
                    staging_data_ = std::vector<std::uint8_t>(buf, buf + len);
                    state_ = BotState::DataIn;
                    break;
                }
                case 0x25: { // READ CAPACITY (10)
                    auto last_lba = backend_ ? backend_->block_count() - 1 : 0;
                    std::uint32_t bs = backend_->block_size();
                    std::uint8_t buf[8] = {};
                    buf[0] = (last_lba >> 24) & 0xFF;
                    buf[1] = (last_lba >> 16) & 0xFF;
                    buf[2] = (last_lba >> 8) & 0xFF;
                    buf[3] = last_lba & 0xFF;
                    buf[4] = (bs >> 24) & 0xFF;
                    buf[5] = (bs >> 16) & 0xFF;
                    buf[6] = (bs >> 8) & 0xFF;
                    buf[7] = bs & 0xFF;
                    auto len = std::min(transfer_len, std::uint32_t(8));
                    staging_offset_ = 0;
                    staging_data_ = std::vector<std::uint8_t>(buf, buf + len);
                    state_ = BotState::DataIn;
                    break;
                }
                case 0x28: // READ (10)
                case 0x2A: { // WRITE (10)
                    auto lba = (std::uint64_t(current_cbw_.CBWCB[2]) << 24) |
                               (std::uint64_t(current_cbw_.CBWCB[3]) << 16) |
                               (std::uint64_t(current_cbw_.CBWCB[4]) << 8) |
                               (std::uint64_t(current_cbw_.CBWCB[5]));
                    auto count = (std::uint16_t(current_cbw_.CBWCB[7]) << 8) |
                                 (std::uint16_t(current_cbw_.CBWCB[8]));
                    if (count == 0) count = 256;

                    if (lba + count > (backend_ ? backend_->block_count() : 0)) {
                        SPDLOG_WARN("SCSI cmd 0x{:02X} LBA={} count={} 超出范围", cmd, lba, count);
                        command_failed_ = true;
                        state_ = BotState::Status;
                        break;
                    }

                    if (cmd == 0x28) {
                        // READ：优先 mmap 直发（sendfile 路径），否则回退 staging
                        staging_offset_ = 0;
                        read_lba_ = lba;
                        read_mmap_base_ = backend_->get_direct_buffer(lba);
                        if (read_mmap_base_) {
                            read_total_size_ = static_cast<std::size_t>(count) * backend_->block_size();
                            staging_data_.clear();
                        } else {
                            staging_data_.resize(read_total_size_ = static_cast<std::size_t>(count) * backend_->block_size());
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
                case 0x1B: case 0x2F:
                    state_ = BotState::Status;
                    break;
                case 0x85:
                    command_failed_ = true;
                    state_ = BotState::Status;
                    break;
                case 0x42: { // UNMAP，数据长度以 CBW.dCBWDataTransferLength 为准（某些内核 CDB 参数长度为 0）
                    auto data_len = current_cbw_.dCBWDataTransferLength;
                    SPDLOG_DEBUG("UNMAP CBW tag=0x{:08X} dataLen={}", current_cbw_.dCBWTag, data_len);
                    if (read_only_) { command_failed_ = true; state_ = BotState::Status; break; }
                    if (data_len == 0) { state_ = BotState::Status; break; }
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
                        auto lba = (std::uint64_t(d[i]) << 56) | (std::uint64_t(d[i + 1]) << 48) |
                                   (std::uint64_t(d[i + 2]) << 40) | (std::uint64_t(d[i + 3]) << 32) |
                                   (std::uint64_t(d[i + 4]) << 24) | (std::uint64_t(d[i + 5]) << 16) |
                                   (std::uint64_t(d[i + 6]) << 8) | (std::uint64_t(d[i + 7]));
                        auto cnt = (std::uint32_t(d[i + 8]) << 24) | (std::uint32_t(d[i + 9]) << 16) |
                                   (std::uint32_t(d[i + 10]) << 8) | (std::uint32_t(d[i + 11]));
                        SPDLOG_DEBUG("UNMAP punch lba={} cnt={}", lba, cnt);
                        backend_->punch_hole(lba, cnt);
                    }
                    staging_data_.clear();
                    data_out_unmap_ = false;
                    data_residue_ = 0;
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

void MscBulkOnlyHandler::handle_bulk_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                              std::uint32_t transfer_flags,
                                              std::uint32_t transfer_buffer_length,
                                              TransferHandle transfer, std::error_code &ec) {
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
                        trx->external_buf = static_cast<char*>(read_mmap_base_) + staging_offset_;
                        trx->file_lba = read_lba_;
                        trx->file_offset = staging_offset_;
                        SPDLOG_DEBUG("MSC::hb IN mmap handle={:p} lba={} offset={} len={}",
                                     static_cast<const void*>(transfer.get()), read_lba_, staging_offset_, len);
                    } else {
                        trx->external_buf = staging_data_.data() + staging_offset_;
                        SPDLOG_DEBUG("MSC::hb IN staging handle={:p} offset={} len={}",
                                     static_cast<const void*>(transfer.get()), staging_offset_, len);
                    }
                    trx->actual_length = len;
                    staging_offset_ += len;
                    session->submit_ret_submit(
                            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                                    seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK),
                                    static_cast<std::uint32_t>(len), std::move(transfer)));
                }
                else {
                    session->submit_ret_submit(
                            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(
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
                csw.dCSWDataResidue = data_residue_;
                if (command_failed_) { csw.bCSWStatus = 1; command_failed_ = false; }

                auto *trx = StorageIoTransfer::from_handle(transfer.get());
                SPDLOG_DEBUG("MSC::hb Status handle={:p} CSW_tag=0x{:08X} status={}",
                             static_cast<const void*>(transfer.get()), csw.dCSWTag, csw.bCSWStatus);
                trx->fallback_data.resize(sizeof(CSW));
                std::memcpy(trx->fallback_data.data(), &csw, sizeof(CSW));
                trx->external_buf = trx->fallback_data.data();
                trx->actual_length = sizeof(CSW);
                session->submit_ret_submit(
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                                seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK),
                                sizeof(CSW), std::move(transfer)));
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
        SPDLOG_DEBUG("MSC::hb OUT drop handle={:p}", static_cast<const void*>(transfer.get()));
        session->submit_ret_submit(
                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(
                        seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK),
                        transfer_buffer_length));
    }
}

void MscBulkOnlyHandler::send_stall(std::uint32_t seqnum) {
    session->submit_ret_submit(
            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
}
