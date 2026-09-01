#include "usbipdcpp/virtual_device/MSOSSimpleVirtualDeviceHandler.h"

#include <algorithm>
#include <cstring>

#include "usbipdcpp/DeviceHandler/DeviceHandler.h"
#include "usbipdcpp/Session.h"
#include "usbipdcpp/protocol.h"
#include "usbipdcpp/virtual_device/MsOsConstants.h"

using namespace usbipdcpp;

std::optional<data_type> usbipdcpp::MSOSSimpleVirtualDeviceHandler::get_special_string_descriptor(
        std::uint8_t string_index) {
    // 只对字符串索引 0xEE 提供 MS OS 签名串（Windows 枚举设备时的标准探测流程）；
    // 未配置 Compatible ID 时返回 nullopt，走父类默认逻辑（string_pool 查找）
    if (string_index == MS_OS_STRING_INDEX && !ms_os_compatible_id_.empty()) {
        data_type desc;
        MsOsStringDesc os_desc{};
        os_desc.b_length = static_cast<std::uint8_t>(sizeof(MsOsStringDesc));
        os_desc.b_descriptor_type = static_cast<std::uint8_t>(DescriptorType::String);
        std::memcpy(os_desc.qw_signature.data(), MS_OS_SIGNATURE_UTF16LE.data(), MS_OS_SIGNATURE_UTF16LE.size());
        os_desc.b_ms_vendor_code = ms_os_vendor_code_;
        os_desc.b_pad = 0;
        os_desc.append_to(desc);
        return desc;
    }
    return std::nullopt;
}

void usbipdcpp::MSOSSimpleVirtualDeviceHandler::handle_non_standard_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet, TransferHandle transfer, std::error_code &ec) {
    // MS OS 1.0 Compatible ID 请求：Vendor|Device|IN(0xC0) + bMS_VendorCode +
    // wIndex=0x0004 + wValue 低字节 0（对齐内核 composite.c 的 os_desc 分发：
    // w_value & 0xff == 0 是 Compatible ID，1 是 Extended Properties）
    constexpr std::uint8_t ms_os_compat_request_type =
            static_cast<std::uint8_t>(RequestType::Vendor) |
            static_cast<std::uint8_t>(RequestRecipient::Device) | 0x80u;
    if (!ms_os_compatible_id_.empty() && setup_packet.request_type == ms_os_compat_request_type &&
        setup_packet.request == ms_os_vendor_code_ && setup_packet.index == MS_OS_COMPAT_ID_WINDEX &&
        (setup_packet.value & 0xFF) == 0) {
        // 响应 40 字节 = 16 字节头 + 24 字节功能节（对齐 composite.c
        // fill_ext_compat：dwLength=40、bcdVersion=0x0100、bCount=1、
        // 功能节 reserved1=0x01、接口号 0）。Windows 按此节的
        // CompatibleID 加载对应内置驱动（RNDIS → rndismp）
        data_type data;
        MsOsCompatIdHeader{40, 0x0100, MS_OS_COMPAT_ID_WINDEX, 1, {0, 0, 0, 0, 0, 0, 0}}.append_to(data);
        MsOsCompatIdSection section{};
        section.b_first_interface = 0;
        section.reserved1 = 0x01;
        std::memcpy(section.compatible_id.data(), ms_os_compatible_id_.data(),
                    std::min<std::size_t>(ms_os_compatible_id_.size(), sizeof(section.compatible_id)));
        // SubCompatibleID：Windows 的驱动匹配可能依赖它（如 rndismp 按
        // MS_SUBCOMP_5162001 匹配，见 rndiscmp.inf），空值导致匹配失败
        std::memcpy(section.sub_compatible_id.data(), ms_os_sub_compatible_id_.data(),
                    std::min<std::size_t>(ms_os_sub_compatible_id_.size(), sizeof(section.sub_compatible_id)));
        section.append_to(data);
        if (data.size() > transfer_buffer_length) {
            data.resize(transfer_buffer_length);
        }
        auto *trx = GenericTransfer::from_handle(transfer.get());
        trx->data = std::move(data);
        responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
                seqnum, static_cast<std::uint32_t>(trx->data.size()), std::move(transfer)));
        return;
    }
    // 不是 MS OS 描述符请求：走父类默认（EPIPE）
    SimpleVirtualDeviceHandler::handle_non_standard_request_type_control_urb(
            seqnum, ep, transfer_flags, transfer_buffer_length, setup_packet, std::move(transfer), ec);
}
