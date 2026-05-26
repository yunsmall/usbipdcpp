// #define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE

#include "virtual_device/devices/MscBulkOnlyHandler.h"

#include <spdlog/spdlog.h>
#include <cstring>
#include <algorithm>

#include "Session.h"
#include "SetupPacket.h"
#include "constant.h"

using namespace usbipdcpp;

MscBulkOnlyHandler::MscBulkOnlyHandler(UsbInterface &handle_interface, StringPool &string_pool,
                                       std::unique_ptr<StorageBackend> backend, bool read_only) :
    VirtualInterfaceHandler(handle_interface, string_pool), backend_(std::move(backend)), read_only_(read_only) {
}

void MscBulkOnlyHandler::handle_non_standard_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep,
        std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet,
        TransferHandle transfer, std::error_code &ec) {
    // MSC 只有批量传输
    session->submit_ret_submit(
            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
}

void MscBulkOnlyHandler::handle_bulk_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                              std::uint32_t transfer_flags,
                                              std::uint32_t transfer_buffer_length,
                                              TransferHandle transfer, std::error_code &ec) {
    SPDLOG_DEBUG("BULK {} ep={:02x} len={} state={}", ep.is_in() ? "IN" : "OUT", ep.address, transfer_buffer_length,
                 static_cast<int>(state_));
    if (ep.is_in()) {
        // ===== Bulk IN =====
        switch (state_) {
            case BotState::DataIn: {
                // 分块发送，用偏移量避免 erase O(n)
                auto remaining = staging_data_.size() - staging_offset_;
                auto len = std::min(static_cast<std::size_t>(transfer_buffer_length), remaining);
                if (len > 0) {
                    auto *trx = GenericTransfer::from_handle(transfer.get());
                    trx->data.assign(staging_data_.begin() + staging_offset_,
                                     staging_data_.begin() + staging_offset_ + len);
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
                // 全部数据已发送完毕，清理并进入状态阶段
                if (staging_offset_ >= staging_data_.size()) {
                    staging_data_.clear();
                    staging_offset_ = 0;
                    data_residue_ = 0;
                    state_ = BotState::Status;
                }
                break;
            }
            case BotState::Status: {
                // 发送 CSW
                CSW csw{};
                csw.dCSWSignature = CSW_SIGNATURE;
                csw.dCSWTag = current_cbw_.dCBWTag;
                csw.dCSWDataResidue = data_residue_;
                if (command_failed_) {
                    csw.bCSWStatus = 1; // Failed
                    command_failed_ = false;
                }

                auto *trx = GenericTransfer::from_handle(transfer.get());
                trx->data.resize(sizeof(CSW));
                std::memcpy(trx->data.data(), &csw, sizeof(CSW));
                trx->actual_length = sizeof(CSW);
                session->submit_ret_submit(
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                                seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK),
                                sizeof(CSW), std::move(transfer)));
                state_ = BotState::Idle;
                break;
            }
            default:
                send_stall(seqnum);
                break;
        }
    }
    else {
        // ===== Bulk OUT =====
        switch (state_) {
            case BotState::Idle: {
                // 接收 CBW；do_cbw 有数据阶段会设 DataIn/DataOut
                do_cbw(seqnum, transfer);
                if (state_ == BotState::Idle) {
                    state_ = BotState::Status; // 无数据阶段的命令直接进 Status
                }
                // OUT 传输 actual_length 必须等于 transfer_buffer_length，否则主机认为传输失败
                session->submit_ret_submit(
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(
                                seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK), transfer_buffer_length));
                break;
            }
            case BotState::DataOut: {
                // 接收 WRITE 数据流，累积到完整块数后写入
                auto *trx = GenericTransfer::from_handle(transfer.get());
                auto &rx = trx->data;
                if (write_lba_ + write_count_ <= backend_->block_count()) {
                    staging_data_.insert(staging_data_.end(), rx.begin(), rx.end());
                    if (staging_data_.size() >= static_cast<std::size_t>(write_count_) * backend_->block_size()) {
                        backend_->write(write_lba_, write_count_, staging_data_.data());
                        staging_data_.clear();
                        data_residue_ = 0;
                        state_ = BotState::Status;
                    }
                }
                session->submit_ret_submit(
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(
                                seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK), transfer_buffer_length));
                break;
            }
            default:
                send_stall(seqnum);
                break;
        }
    }
}

void MscBulkOnlyHandler::do_cbw(std::uint32_t seqnum, TransferHandle &transfer) {
    auto *trx = GenericTransfer::from_handle(transfer.get());
    if (trx->data.size() < sizeof(CBW)) {
        SPDLOG_ERROR("CBW 太短: {} 字节", trx->data.size());
        command_failed_ = true;
        return;
    }

    std::memcpy(&current_cbw_, trx->data.data(), sizeof(CBW));

    if (current_cbw_.dCBWSignature != CBW_SIGNATURE) {
        SPDLOG_ERROR("无效 CBW 签名: 0x{:08X}", current_cbw_.dCBWSignature);
        command_failed_ = true;
        return;
    }

    std::uint8_t cmd = current_cbw_.CBWCB[0];
    bool is_data_in = (current_cbw_.bmCBWFlags & 0x80) != 0;
    auto transfer_len = current_cbw_.dCBWDataTransferLength;

    SPDLOG_TRACE("CBW tag=0x{:08X} cmd=0x{:02X} dir={} len={}",
                 current_cbw_.dCBWTag, cmd, is_data_in ? "IN" : "OUT", transfer_len);

    switch (cmd) {
        case 0x00: // TEST UNIT READY
            command_failed_ = !(backend_ != nullptr);
            break;
        case 0x03: {
            // REQUEST SENSE
            std::uint8_t sense[18] = {};
            sense[0] = 0x70;
            sense[7] = 10;
            auto len = std::min(transfer_len, std::uint32_t(18));
            auto data = std::vector<std::uint8_t>(sense, sense + len);
            staging_offset_ = 0;
            state_ = BotState::DataIn;
            staging_data_ = std::move(data);
            break;
        }
        case 0x12: {
            // INQUIRY
            std::uint8_t inquiry[36] = {};
            inquiry[0] = 0x00;                                 // 直接存取块设备 (SBC)
            inquiry[1] = 0x80;                                 // RMB=1 可移动介质
            inquiry[2] = 0x06;                                 // SPC-4
            inquiry[3] = 0x02;                                 // SPC-3 响应格式
            inquiry[4] = 31;                                   // 附加数据长度 (n-5)
            std::memcpy(inquiry + 8, "USBIPDC ", 8);           // T10 厂商 ID
            std::memcpy(inquiry + 16, "USB Flash Drive ", 16); // 产品 ID
            std::memcpy(inquiry + 32, "1.00", 4);              // 产品版本
            auto len = std::min(transfer_len, std::uint32_t(36));
            staging_offset_ = 0;
            state_ = BotState::DataIn;
            staging_data_ = std::vector<std::uint8_t>(inquiry, inquiry + len);
            break;
        }
        case 0x1A: {
            // MODE SENSE (6)
            std::uint8_t mode[4] = {};
            mode[0] = 3;
            if (read_only_)
                mode[2] = 0x80; // WP
            auto len = std::min(transfer_len, std::uint32_t(4));
            staging_offset_ = 0;
            state_ = BotState::DataIn;
            staging_data_ = std::vector<std::uint8_t>(mode, mode + len);
            break;
        }
        case 0x1E: // PREVENT ALLOW MEDIUM REMOVAL — no-op
            break;
        case 0x25: {
            // READ CAPACITY (10)
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
            state_ = BotState::DataIn;
            staging_data_ = std::vector<std::uint8_t>(buf, buf + len);
            break;
        }
        case 0x28: // READ (10)
        case 0x2A: {
            // WRITE (10)
            auto lba = (std::uint64_t(current_cbw_.CBWCB[2]) << 24) |
                       (std::uint64_t(current_cbw_.CBWCB[3]) << 16) |
                       (std::uint64_t(current_cbw_.CBWCB[4]) << 8) |
                       (std::uint64_t(current_cbw_.CBWCB[5]));
            auto count = (std::uint16_t(current_cbw_.CBWCB[7]) << 8) |
                         (std::uint16_t(current_cbw_.CBWCB[8]));
            if (count == 0)
                count = 256; // SCSI: 0 表示 256 块

            if (lba + count > (backend_ ? backend_->block_count() : 0)) {
                SPDLOG_WARN("SCSI cmd 0x{:02X} LBA={} count={} 超出范围", cmd, lba, count);
                command_failed_ = true;
                break;
            }

            if (cmd == 0x28) {
                // READ
                staging_data_ = backend_->read(lba, count);
                state_ = BotState::DataIn;
            }
            else if (read_only_) {
                command_failed_ = true;
            }
            else {
                // WRITE
                write_lba_ = lba;
                write_count_ = count;
                staging_offset_ = 0;
                staging_data_.clear();
                state_ = BotState::DataOut;
            }
            break;
        }
        case 0x1B: // START STOP UNIT
        case 0x2F: // VERIFY
            break;
        default:
            SPDLOG_WARN("不支持的 SCSI 命令: 0x{:02X}", cmd);
            command_failed_ = true;
            break;
    }
}

void MscBulkOnlyHandler::send_stall(std::uint32_t seqnum) {
    session->submit_ret_submit(
            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
}
