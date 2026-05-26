#pragma once

#include <cstdint>
#include <memory>

#include "virtual_device/MscConstants.h"
#include "virtual_device/storage_backends/StorageBackend.h"
#include "virtual_device/VirtualInterfaceHandler.h"

namespace usbipdcpp {

class USBIPDCPP_API MscBulkOnlyHandler : public VirtualInterfaceHandler {
public:
    /**
     * @param handle_interface 接口描述
     * @param string_pool      字符串池
     * @param backend          存储后端（所有权转移）
     */
    MscBulkOnlyHandler(UsbInterface &handle_interface, StringPool &string_pool,
                       std::unique_ptr<StorageBackend> backend, bool read_only = false);

    void handle_bulk_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                              std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
                              TransferHandle transfer, std::error_code &ec) override;

    void handle_non_standard_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                      std::uint32_t transfer_flags,
                                                      std::uint32_t transfer_buffer_length,
                                                      const SetupPacket &setup_packet,
                                                      TransferHandle transfer, std::error_code &ec) override;

private:
    std::unique_ptr<StorageBackend> backend_;
    bool read_only_ = false;
    BotState state_ = BotState::Idle;
    CBW current_cbw_{};
    std::vector<std::uint8_t> staging_data_;
    std::size_t staging_offset_ = 0; // 读取进度（避免 erase O(n)）
    std::uint32_t data_residue_ = 0;
    bool command_failed_ = false;
    std::uint64_t write_lba_ = 0;
    std::uint16_t write_count_ = 0;
    bool data_out_unmap_ = false; // DataOut 阶段是 UNMAP 参数数据而非 WRITE 数据

    void do_cbw(std::uint32_t seqnum, TransferHandle &transfer);
    void send_stall(std::uint32_t seqnum);

    // ========== 标准请求默认实现 ==========

    void request_clear_feature(std::uint16_t feature_selector, std::uint32_t *p_status) override {
        *p_status = 0;
    }

    void request_endpoint_clear_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                        std::uint32_t *p_status) override {
        *p_status = 0;
    }

    std::uint8_t request_get_interface(std::uint32_t *p_status) override {
        *p_status = 0;
        return 0;
    }

    void request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) override {
        *p_status = 0;
    }

    std::uint16_t request_get_status(std::uint32_t *p_status) override {
        *p_status = 0;
        return 0;
    }

    std::uint16_t request_endpoint_get_status(std::uint8_t ep_address, std::uint32_t *p_status) override {
        *p_status = 0;
        return 0;
    }

    void request_set_feature(std::uint16_t feature_selector, std::uint32_t *p_status) override {
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
    }

    void request_endpoint_set_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                      std::uint32_t *p_status) override {
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
    }

    [[nodiscard]] data_type get_class_specific_descriptor() override {
        return {};
    }
};

} // namespace usbipdcpp
