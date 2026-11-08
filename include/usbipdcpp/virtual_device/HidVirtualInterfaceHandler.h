#pragma once


#include <asio.hpp>

#include "usbipdcpp/virtual_device/HidConstants.h"
#include "usbipdcpp/SetupPacket.h"
#include "usbipdcpp/constant.h"
#include "usbipdcpp/protocol.h"
#include "usbipdcpp/virtual_device/InEndpointChannel.h"
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
        // 报告堆积上限：主机长期不读中断 IN 时防止内存无限增长（超限丢最旧）
        input_channel.set_max_pending_messages(MAX_PENDING_INPUT_REPORTS);
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
     * @brief 获取当前协议（Boot/Report）
     * HID 1.11 §7.2.6：设备上电后默认使用 Report Protocol，
     * 因此未切换过时 GET_PROTOCOL 必须返回 1
     * @param p_status
     * @return 协议值（0=Boot，1=Report）
     */
    virtual std::uint8_t request_get_protocol(std::uint32_t *p_status) {
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusOK);
        return 1;
    };

    /**
     * @brief 设置协议。HID 1.11 §7.2.6：只有 Boot 子类接口
     *        （bInterfaceSubClass=1）才实现该请求，其余 stall。
     *        基类不保存状态，需要真正切换 Boot 报告格式的子类重写
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
    // 端点 feature/status 交给基类的 ENDPOINT_HALT 状态机（USB 2.0 §9.4.5
    // 要求中断/批量端点必须实现 Halt）；接口级 feature 基类默认 STALL；
    // SET/GET_INTERFACE 用基类实现——HID 接口只有 alt 0，基类会拒绝其他
    // alt（USB 2.0 §9.4.10：不存在的 alternate setting 回 Request Error），
    // 不能无条件回成功

    std::uint16_t request_get_status(std::uint32_t *p_status) override {
        *p_status = 0;
        return 0;
    }

    // ========== 内部实现（子类无需关心） ==========

    void on_new_connection(TransferResponder &current_session, error_code &ec) override;

    void on_disconnection(std::error_code &ec) override;

    bool cancel_pending_urb(std::uint32_t unlink_seqnum) override;

protected:
    /**
     * @brief IN 输入报告通道（消息模式）
     *
     * 封装「挂起-应答」：主机 IN 请求先挂起，send_input_report() 推入数据
     * 时匹配应答；缓冲超上限丢最旧（见 MAX_PENDING_INPUT_REPORTS）
     */
    MessageInChannel input_channel;

    /**
     * @brief 待发送输入报告上限，超过丢最旧（主机长期不发起中断 IN 请求时
     * 报告会堆积，防止内存无限增长）
     */
    static constexpr std::size_t MAX_PENDING_INPUT_REPORTS = 1024;

    bool has_pending_input_reports() const {
        return input_channel.size() != 0;
    }
};
} // namespace usbipdcpp
