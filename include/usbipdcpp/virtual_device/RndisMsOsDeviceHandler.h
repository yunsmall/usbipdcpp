#pragma once

#include "usbipdcpp/virtual_device/MSOSSimpleVirtualDeviceHandler.h"

namespace usbipdcpp {
/**
 * @brief RNDIS 专用 MS OS 设备处理器（final）
 *
 * 预设 MS OS 1.0 Compatible ID "RNDIS" + SubCompatibleID "5162001"：Windows
 * 的 rndismp 网卡驱动按 `USB\MS_COMP_RNDIS&MS_SUBCOMP_5162001` 匹配
 * （rndiscmp.inf），SubCompatibleID 为空匹配失败、设备退回 usbser 绑成串口。
 * 对齐内核 f_rndis 把 os_desc 的 ext_compat_id 绑定进功能定义——RNDIS 设备
 * 装配时直接 with_handler<本类> 即可，配置固定不对外暴露。
 */
class USBIPDCPP_API RndisMsOsDeviceHandler final : public MSOSSimpleVirtualDeviceHandler {
public:
    RndisMsOsDeviceHandler(UsbDevice &handle_device, StringPool &string_pool) :
        MSOSSimpleVirtualDeviceHandler(handle_device, string_pool) {
        set_ms_os_compatible_id("RNDIS");
        set_ms_os_sub_compatible_id("5162001");
    }

private:
    // RNDIS 的 MS OS 配置是功能定义的一部分（预设固定）：把基类的配置
    // setter 隐藏为 private，防止外部改动破坏 rndismp 匹配
    using MSOSSimpleVirtualDeviceHandler::set_ms_os_compatible_id;
    using MSOSSimpleVirtualDeviceHandler::set_ms_os_sub_compatible_id;
    using MSOSSimpleVirtualDeviceHandler::set_ms_os_vendor_code;
};
} // namespace usbipdcpp
