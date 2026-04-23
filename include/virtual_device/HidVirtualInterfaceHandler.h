#pragma once


#include <optional>
#include <mutex>

#include <asio.hpp>

#include "protocol.h"
#include "virtual_device/VirtualInterfaceHandler.h"
#include "SetupPacket.h"
#include "constant.h"
#include "HidConstants.h"


namespace usbipdcpp {
/**
 * @brief HID 设备接口处理器基类
 *
 * 提供中断传输的默认实现，用户只需实现报告描述符和控制请求处理。
 */
class HidVirtualInterfaceHandler : public VirtualInterfaceHandler {
public:
    HidVirtualInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool) :
        VirtualInterfaceHandler(handle_interface, string_pool) {
    }

    void handle_non_standard_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                      std::uint32_t transfer_flags,
                                                      std::uint32_t transfer_buffer_length,
                                                      const SetupPacket &setup_packet,
                                                      TransferHandle transfer, std::error_code &ec) override;

    /**
     * @brief 处理中断传输（默认实现）
     *
     * 中断 IN：主机请求输入报告，调用 on_input_report_requested() 获取数据
     * 中断 OUT：主机发送输出报告，调用 on_output_report_received()
     */
    void handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                   std::uint32_t transfer_flags,
                                   std::uint32_t transfer_buffer_length,
                                   TransferHandle transfer, std::error_code &ec) override;

    virtual void handle_non_hid_request_type_control_urb(std::uint32_t seqnum,
                                                         const UsbEndpoint &ep,
                                                         std::uint32_t transfer_flags,
                                                         std::uint32_t transfer_buffer_length,
                                                         const SetupPacket &setup_packet,
                                                         TransferHandle transfer, std::error_code &ec);

    data_type request_get_descriptor(std::uint8_t type, std::uint8_t language_id,
                                     std::uint16_t descriptor_length, std::uint32_t *p_status) override;

    [[nodiscard]] data_type get_class_specific_descriptor() override;

    virtual data_type get_report_descriptor() =0;
    virtual std::uint16_t get_report_descriptor_size() =0;

    // ========== 发送输入报告 ==========

    /**
     * @brief 发送输入报告（零拷贝）
     *
     * 如果有挂起的中断请求，立即响应。
     *
     * @param data 报告数据（可以使用栈上的 std::array + asio::buffer）
     * @return true 如果数据已发送，false 如果没有挂起请求
     */
    bool send_input_report(asio::const_buffer data);

    // ========== 子类可重写的回调 ==========

    /**
     * @brief 主机请求输入报告时回调
     *
     * @warning 不推荐使用此函数！
     *          每次主机轮询中断端点时都会调用此函数，如果返回非空数据，
     *          主机会立即取走数据并再次轮询，导致 CPU 占用非常高。
     *          推荐使用 send_input_report() 在有数据时主动推送。
     *
     * @param length 主机请求的数据长度
     * @return 报告数据，返回空则挂起请求等待 send_input_report
     */
    virtual data_type on_input_report_requested(std::uint16_t length);

    /**
     * @brief 收到输出报告时回调
     *
     * @param data 输出报告数据
     */
    virtual void on_output_report_received(asio::const_buffer data);

    // ========== HID 类特定请求 ==========

    /**
     * @brief Rarely implemented, this is optional for unbooted devices
     * @param p_status
     * @return
     */
    virtual std::uint8_t request_get_protocol(std::uint32_t *p_status) {
        SPDLOG_WARN("unhandled request_get_protocol");
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
        return 0;
    };

    /**
     * @brief Rarely implemented, this is optional for unbooted devices
     * @param type
     * @param p_status
     */
    virtual void request_set_protocol(std::uint16_t type, std::uint32_t *p_status) {
        SPDLOG_WARN("unhandled request_set_protocol");
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
    };

    virtual data_type request_get_report(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                                         std::uint32_t *p_status);
    virtual void request_set_report(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                                    const data_type &data,
                                    std::uint32_t *p_status);

    virtual data_type request_get_idle(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                                       std::uint32_t *p_status);
    virtual void request_set_idle(std::uint8_t speed, std::uint32_t *p_status);

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

    // ========== 连接生命周期 ==========

    void on_disconnection(std::error_code &ec) override;

    // ========== UNLINK 处理 ==========

    void handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) override;

protected:
    /**
     * @brief 挂起的中断请求（USB协议规定一个端点同一时间只能有一个挂起请求）
     */
    struct IntRequest {
        std::uint32_t seqnum;
        std::uint32_t length;
        TransferHandle transfer;
    };
    std::optional<IntRequest> pending_interrupt_request_;
    std::mutex interrupt_mutex_;
};
}
