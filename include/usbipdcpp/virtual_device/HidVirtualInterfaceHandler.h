#pragma once


#include <deque>
#include <mutex>

#include <asio.hpp>

#include "usbipdcpp/virtual_device/HidConstants.h"
#include "usbipdcpp/SetupPacket.h"
#include "usbipdcpp/constant.h"
#include "usbipdcpp/protocol.h"
#include "usbipdcpp/virtual_device/VirtualInterfaceHandler.h"


namespace usbipdcpp {
/**
 * @brief HID 设备接口处理器基类
 *
 * 提供中断传输的默认实现，用户只需实现报告描述符和控制请求处理。
 */
class USBIPDCPP_API HidVirtualInterfaceHandler : public VirtualInterfaceHandler {
public:
    HidVirtualInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool) :
        VirtualInterfaceHandler(handle_interface, string_pool) {
    }

    // ========== 内部实现（子类无需关心） ==========

    void handle_non_standard_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                      std::uint32_t transfer_flags,
                                                      std::uint32_t transfer_buffer_length,
                                                      const SetupPacket &setup_packet, TransferHandle transfer,
                                                      std::error_code &ec) override;

    /**
     * @brief 处理中断传输（默认实现）
     *
     * 中断 IN：主机请求输入报告，调用 on_input_report_requested() 获取数据
     * 中断 OUT：主机发送输出报告，调用 on_output_report_received()
     */
    void handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                                   std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                   std::error_code &ec) override;

    virtual void handle_non_hid_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                         std::uint32_t transfer_flags,
                                                         std::uint32_t transfer_buffer_length,
                                                         const SetupPacket &setup_packet, TransferHandle transfer,
                                                         std::error_code &ec);

    data_type request_get_descriptor(std::uint8_t type, std::uint8_t language_id, std::uint16_t descriptor_length,
                                     std::uint32_t *p_status) override;

    [[nodiscard]] data_type get_class_specific_descriptor() override;

    // ========== 子类必须实现的虚函数 ==========

    /**
     * @brief 获取 HID 报告描述符
     * @return 报告描述符数据
     */
    virtual data_type get_report_descriptor() = 0;

    /**
     * @brief 获取 HID 报告描述符大小
     * @return 描述符长度（字节）
     */
    virtual std::uint16_t get_report_descriptor_size() = 0;

    // ========== 发送数据 API ==========

    /**
     * @brief 发送输入报告（零拷贝）
     *
     * 如果有队列中的请求，立即响应第一个；否则存储数据等待下一个请求。
     *
     * @param data 报告数据（可以使用栈上的 std::array + asio::buffer）
     */
    void send_input_report(asio::const_buffer data);

    // ========== 子类可选重写的回调 ==========

    /**
     * @brief 主机请求输入报告时回调
     *
     * @warning 每次主机轮询中断端点时都会调用此函数。在函数内部
     *          因没有获取任何锁可以直接调用send_input_report等函数。
     *          如果每次调用都调用send_input_report()，主机会立即取走数据并再次轮询，
     *          这会导致 CPU 占用非常高。非特殊情况请不要在这个函数中
     *          每次都调用 send_input_report()。
     *
     * @param length 主机请求的数据长度
     */
    virtual void on_input_report_requested(std::uint16_t length);

    /**
     * @brief 收到输出报告时回调
     *
     * @param data 输出报告数据
     */
    virtual void on_output_report_received(asio::const_buffer data);

    // ========== HID 类特定请求（子类可选重写） ==========

    /**
     * @brief 获取当前协议（Boot/Report），形式响应对齐内核 f_hid.c：
     *        内核 GET_PROTOCOL 返回 protocol 值（configfs 初始 0），
     *        即使设备不支持切换也照常应答。子类实现协议切换时重写返回实际值
     * @param p_status
     * @return 协议值（0=Boot，1=Report）
     */
    virtual std::uint8_t request_get_protocol(std::uint32_t *p_status) {
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusOK);
        return 0;
    };

    /**
     * @brief 设置协议，对齐内核 f_hid.c：仅 Boot 子类接口（bInterfaceSubClass=1）
     *        接受 SET_PROTOCOL（wValue ≤ 1），其余 stall。协议值仅内部记录，
     *        本项目默认不做 Boot 报告切换，接受即可；子类可重写
     * @param type wValue（0=Boot 协议，1=Report 协议）
     * @param p_status
     */
    virtual void request_set_protocol(std::uint16_t type, std::uint32_t *p_status) {
        if (handle_interface.interface_subclass == 0x01 /* USB_INTERFACE_SUBCLASS_BOOT */
            && type <= static_cast<std::uint16_t>(HIDProtocolType::Report)) {
            *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusOK);
        }
        else {
            *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
        }
    };

    virtual data_type request_get_report(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                                         std::uint32_t *p_status);
    virtual void request_set_report(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                                    const data_type &data, std::uint32_t *p_status);

    /**
     * @brief 获取空闲节流值，形式响应对齐内核 f_hid.c：GET_IDLE 返回
     *        idle 值（初始 0），设备默认不做 idle 节流；子类可重写
     */
    virtual data_type request_get_idle(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                                       std::uint32_t *p_status) {
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusOK);
        return {0x00};
    };

    /**
     * @brief 设置空闲节流值，对齐内核 f_hid.c：无条件接受 SET_IDLE
     *        （idle 只是设备侧节流参数，本项目默认不做节流，接受即可）
     */
    virtual void request_set_idle(std::uint8_t speed, std::uint32_t *p_status) {
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusOK);
    };

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

    // ========== 内部实现（子类无需关心） ==========

    void on_disconnection(std::error_code &ec) override;

    void handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) override;

protected:
    /**
     * @brief 保护 pending_input_report_ 的互斥锁
     */
    mutable std::mutex input_mutex_;

    /**
     * @brief 待发送的输入报告队列
     *
     * 主机长期不发起中断 IN 请求时报告会堆积，超过上限丢最旧（见 send_input_report）
     */
    static constexpr std::size_t MAX_PENDING_INPUT_REPORTS = 1024;
    std::deque<data_type> pending_input_reports_;

    bool has_pending_input_reports() const {
        std::lock_guard<std::mutex> lock(input_mutex_);
        return !pending_input_reports_.empty();
    }
};
} // namespace usbipdcpp
