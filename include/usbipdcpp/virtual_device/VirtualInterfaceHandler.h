#pragma once

#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>

#include "usbipdcpp/DeviceHandler/TransferOperator.h"
#include "usbipdcpp/InterfaceHandler/InterfaceHandler.h"
#include "usbipdcpp/protocol.h"
#include "usbipdcpp/virtual_device/InEndpointChannel.h"

namespace usbipdcpp {

class VirtualDeviceHandler;

class USBIPDCPP_API VirtualInterfaceHandler : public AbstInterfaceHandler {
public:
    explicit VirtualInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool,
                                     std::unique_ptr<TransferOperator> op = nullptr) :
        AbstInterfaceHandler(handle_interface), string_pool(string_pool),
        transfer_op_(op ? std::move(op) : std::make_unique<GenericTransferOperator>()) {

        string_interface = string_pool.new_string(L"Usbipdcpp Virtual Interface");
    }

    // ========== 连接生命周期 API ==========

    /**
     * @brief 设置所属的 DeviceHandler
     * @param handler DeviceHandler 指针
     */
    void set_device_handler(VirtualDeviceHandler *handler) {
        device_handler = handler;
    }

    /** setup_interface_handlers 末尾回调，此时 device_handler 已设置，子类可在此做初始化 */
    virtual void on_setup_interface_handlers() {
    }

    /**
     * @brief 新的客户端连接时会调这个函数
     * @param current_session
     * @param ec 发生的ec
     * @note 子类重写时必须调用父类实现，在函数开头调用，父类会设置session指针
     */
    void on_new_connection(Session &current_session, error_code &ec) override {
        session = &current_session;
    }

    /**
     * @brief 当发生错误、客户端detach、主动关闭服务器等情况需要完全终止传输时会调用这个函数。被调用后不可以再提交消息。
     * @note 子类重写时必须调用父类实现，在函数末尾调用，父类会清理session指针
     */
    void on_disconnection(error_code &ec) override {
        session = nullptr;
    }

    // ========== 数据面回调（默认回 EPIPE，功能设备必须重写） ==========

    virtual void handle_bulk_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                                      std::uint32_t transfer_buffer_length, TransferHandle transfer, error_code &ec);
    virtual void handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                                           std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                           error_code &ec);
    virtual void handle_isochronous_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                                             std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                             int num_iso_packets, error_code &ec);

    virtual void handle_non_standard_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                              std::uint32_t transfer_flags,
                                                              std::uint32_t transfer_buffer_length,
                                                              const SetupPacket &setup, TransferHandle transfer,
                                                              std::error_code &ec);
    virtual void handle_non_standard_request_type_control_urb_to_endpoint(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                                          std::uint32_t transfer_flags,
                                                                          std::uint32_t transfer_buffer_length,
                                                                          const SetupPacket &setup,
                                                                          TransferHandle transfer, std::error_code &ec);

    // ========== 标准请求回调（默认实现：接受并回成功，子类按需重写）==========
    // 默认实现都很简单，放头文件内联：头文件即文档，子类无需跳去 cpp 看默认行为

    virtual void request_clear_feature(std::uint16_t feature_selector, std::uint32_t *p_status) {
        *p_status = 0;
    }
    virtual void request_endpoint_clear_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                                std::uint32_t *p_status) {
        *p_status = 0;
    }

    virtual std::uint8_t request_get_interface(std::uint32_t *p_status) {
        // 返回当前 alternate setting（设备级在 SET_INTERFACE 成功时已更新）
        *p_status = 0;
        return handle_interface.current_altsetting;
    }
    virtual void request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) {
        // 只接受设备定义里存在的 alt（endpoints 外层下标即 alt 号）：请求不存在的
        // alt 回 EPIPE，否则 GET_INTERFACE 返回的值与端点集合不一致（假装成功
        // 会让主机驱动读到矛盾的接口状态）
        if (alternate_setting < handle_interface.endpoints.size()) {
            *p_status = 0;
        }
        else {
            *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
        }
    }

    virtual std::uint16_t request_get_status(std::uint32_t *p_status) {
        *p_status = 0;
        return 0;
    }
    virtual std::uint16_t request_endpoint_get_status(std::uint8_t ep_address, std::uint32_t *p_status) {
        *p_status = 0;
        return 0;
    }

    /**
     * @brief this function is not necessary for all device,
     * HID device is required to implement this function
     * @param type
     * @param language_id
     * @param descriptor_length
     * @param p_status
     * @return
     */
    virtual data_type request_get_descriptor(std::uint8_t type, std::uint8_t language_id,
                                             std::uint16_t descriptor_length, std::uint32_t *p_status) {
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
        return {};
    }

    virtual void request_set_feature(std::uint16_t feature_selector, std::uint32_t *p_status) {
        *p_status = 0;
    }
    virtual void request_endpoint_set_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                              std::uint32_t *p_status) {
        *p_status = 0;
    }

    /**
     * @brief Only use for isochronous transfer, so give a default empty implement.
     * @param ep_address
     * @param p_status
     * @return
     */
    virtual std::uint16_t request_endpoint_sync_frame(std::uint8_t ep_address, std::uint32_t *p_status) {
        return 0;
    }


    /// 类描述符（挂在配置描述符的接口描述符之后），默认空表示没有类描述符
    [[nodiscard]] virtual data_type get_class_specific_descriptor() {
        return {};
    }

    /**
     * @brief class-specific 描述符是否放在所有 alternate setting 之后
     *
     * 默认 false：仅放 alt 0（UVC 等描述符较大的类，放所有 alt 会撑爆
     * 配置描述符 255 字节上限导致 Windows 截断解析失败）。
     * UAC 的 AS 接口描述符很小，且 alt 1 必须有格式描述符供驱动解析，需返回 true。
     */
    [[nodiscard]] virtual bool put_class_specific_descriptor_in_all_alts() const {
        return false;
    }

    // ========== TransferOperator ==========

    TransferOperator *get_transfer_operator() {
        return transfer_op_.get();
    }

    void set_transfer_operator(std::unique_ptr<TransferOperator> op) {
        // 注意：HID 等基于 GenericTransfer 的类（handle_control_urb、
        // handle_interrupt_transfer、send_input_report 里的
        // GenericTransfer::from_handle）硬性依赖 handle 是 GenericTransfer。
        // 默认 GenericTransferOperator 符合此假设；仅存储类（MSC 的
        // StorageTransferOperator，走 Bulk 不经 from_handle）例外。除非你
        // 清楚自己在做什么，否则不要给依赖 GenericTransfer 的接口换非
        // Generic 的 op，否则 from_handle 强转是未定义行为
        transfer_op_ = std::move(op);
    }

    // ========== 工具函数 ==========

    [[nodiscard]] virtual std::uint8_t get_string_interface_value() const {
        return string_interface;
    }

    [[nodiscard]] virtual std::wstring get_string_interface() const {
        return string_pool.get_string(string_interface).value_or(L"");
    }

    void change_string_interface(const std::wstring &new_str) {
        string_pool.change_string(string_interface, new_str);
    }

    /// 使 iInterface 与另一个 handler 一致（USBCCGP 移除 IAD 后，同功能接口靠 iInterface 分组）
    void sync_string_interface_from(const VirtualInterfaceHandler &other) {
        string_interface = other.string_interface;
    }

protected:
    Session *session = nullptr;
    VirtualDeviceHandler *device_handler = nullptr;
    std::unique_ptr<TransferOperator> transfer_op_;

    std::uint8_t string_interface;

    StringPool &string_pool;
};

} // namespace usbipdcpp
