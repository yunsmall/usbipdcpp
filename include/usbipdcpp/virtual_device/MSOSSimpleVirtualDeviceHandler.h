#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "usbipdcpp/virtual_device/MsOsConstants.h"
#include "usbipdcpp/virtual_device/SimpleVirtualDeviceHandler.h"

namespace usbipdcpp {
/**
 * @brief 支持 Microsoft OS 1.0 描述符的简单设备处理器
 *
 * Windows 对部分设备类（如 RNDIS 网卡）依赖 MS OS 描述符加载内置驱动：
 * 探测字符串索引 0xEE（"MSFT100" 签名串）成功后，再用 bMS_VendorCode 请求
 * Compatible ID 特征描述符，按 Compatible ID 绑定对应驱动——没有它，RNDIS
 * 设备会被 usbser 误绑成串口（实测出现 COM 口而非网卡）。对齐内核 gadget
 * 的 configfs os_desc 支持（composite.c 的 usb_os_string / fill_ext_compat）。
 *
 * 默认不调用 set_ms_os_compatible_id = 不提供 MS OS 描述符（行为同
 * SimpleVirtualDeviceHandler）。
 */
class USBIPDCPP_API MSOSSimpleVirtualDeviceHandler : public SimpleVirtualDeviceHandler {
public:
    using SimpleVirtualDeviceHandler::SimpleVirtualDeviceHandler;

    /**
     * @brief 设置 MS OS 1.0 Compatible ID（如 "RNDIS"），启用 MS OS 描述符响应
     * @param compatible_id 8 字节内的 ASCII 兼容 ID（短则补 0）
     */
    void set_ms_os_compatible_id(std::string compatible_id) {
        ms_os_compatible_id_ = std::move(compatible_id);
    }

    /**
     * @brief 设置 MS OS 1.0 SubCompatibleID（如 RNDIS 的 "5162001"）
     *
     * Windows 的 rndismp 驱动按 `USB\MS_COMP_RNDIS&MS_SUBCOMP_5162001` 匹配
     * （rndiscmp.inf）——SubCompatibleID 为空则匹配失败，设备退回 usbser。
     * 真实 RNDIS 设备（安卓手机等）响应里都带 "5162001"，须对齐
     * @param sub_compatible_id 8 字节内的 ASCII 子兼容 ID（短则补 0）
     */
    void set_ms_os_sub_compatible_id(std::string sub_compatible_id) {
        ms_os_sub_compatible_id_ = std::move(sub_compatible_id);
    }

    /// 当前配置的 Compatible ID（空 = 未启用 MS OS 描述符）
    [[nodiscard]] const std::string &get_ms_os_compatible_id() const {
        return ms_os_compatible_id_;
    }

    /// 当前配置的 SubCompatibleID
    [[nodiscard]] const std::string &get_ms_os_sub_compatible_id() const {
        return ms_os_sub_compatible_id_;
    }

    /**
     * @brief 设置 MS OS 描述符请求号（0xEE 签名串里的 bMS_VendorCode，
     * 默认 0x01，对齐内核 configfs 惯例）。须在连接前设置
     */
    void set_ms_os_vendor_code(std::uint8_t vendor_code) {
        ms_os_vendor_code_ = vendor_code;
    }

    /// 当前 MS OS 描述符请求号（bMS_VendorCode）
    [[nodiscard]] std::uint8_t get_ms_os_vendor_code() const {
        return ms_os_vendor_code_;
    }

    /// MS OS 1.0 签名串（字符串索引 0xEE："MSFT100" UTF-16LE + bMS_VendorCode）
    std::optional<data_type> get_special_string_descriptor(std::uint8_t string_index) override;

protected:
    /// MS OS 1.0 Compatible ID 特征描述符（Vendor|Device|IN + bMS_VendorCode/
    /// wIndex=4）；不匹配时走 SimpleVirtualDeviceHandler 的 EPIPE
    void handle_non_standard_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                      std::uint32_t transfer_flags,
                                                      std::uint32_t transfer_buffer_length,
                                                      const SetupPacket &setup_packet, TransferHandle transfer,
                                                      std::error_code &ec) override;

private:
    /// MS OS 1.0 Compatible ID（空 = 不提供 MS OS 描述符）
    std::string ms_os_compatible_id_;
    /// MS OS 1.0 SubCompatibleID（默认空；RNDIS 需 "5162001" 才能匹配 rndismp）
    std::string ms_os_sub_compatible_id_;
    /// MS OS 描述符请求号（0xEE 签名串里的 bMS_VendorCode）
    std::uint8_t ms_os_vendor_code_ = MS_OS_DEFAULT_VENDOR_CODE;
};
} // namespace usbipdcpp
