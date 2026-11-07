#include "usbipdcpp/virtual_device/SimpleVirtualDeviceHandler.h"

#include <algorithm>

#include "usbipdcpp/Session.h"
#include "usbipdcpp/protocol.h"
#include "usbipdcpp/virtual_device/UsbClassConstants.h"
#include "usbipdcpp/virtual_device/VirtualInterfaceHandler.h"

using namespace usbipdcpp;

namespace {

/**
 * @brief 把配置描述符里的端点 wMaxPacketSize 从高速值改成全速值
 *
 * USB 2.0 §9.6.4 / Table 9-11：other_speed_configuration 描述设备在另一速度
 * 下的配置，端点包长必须是那个速度下的值。本实现只在高速设备上提供该描述符，
 * 另一速度即全速：全速 ISO 最大 1023，Bulk/Interrupt/Control 最大 64
 * （USB 2.0 §5.7/§5.8），且全速端点没有 bits12..11 的每微帧多事务编码。
 * @param desc 配置描述符（原地修改，bDescriptorType 已改成 OtherSpeedConfiguration）
 */
void convert_endpoints_to_full_speed(usbipdcpp::data_type &desc) {
    std::size_t pos = 0;
    while (pos + 2 <= desc.size()) {
        auto length = desc[pos];
        // 描述符链由 bLength 串起来，长度非法就不能继续往后走（避免死循环/越界）
        if (length < 2 || pos + length > desc.size()) {
            break;
        }
        if (desc[pos + 1] == static_cast<std::uint8_t>(usbipdcpp::DescriptorType::Endpoint) &&
            length == sizeof(usbipdcpp::EndpointDesc)) {
            auto *ep = reinterpret_cast<usbipdcpp::EndpointDesc *>(&desc[pos]);
            // 线格式是小端：htole 双向对称，读取时同样用于转成主机序
            auto value = static_cast<std::uint16_t>(usbipdcpp::htole(ep->wMaxPacketSize) & 0x07FF);
            auto is_iso = (ep->bmAttributes & 0x03) ==
                          static_cast<std::uint8_t>(usbipdcpp::EndpointAttributes::Isochronous);
            auto max_size = static_cast<std::uint16_t>(is_iso ? 1023 : 64);
            ep->wMaxPacketSize = usbipdcpp::htole(static_cast<std::uint16_t>(std::min(value, max_size)));
        }
        pos += length;
    }
}

} // namespace

void usbipdcpp::SimpleVirtualDeviceHandler::handle_non_standard_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet, TransferHandle transfer, std::error_code &ec) {
    SPDLOG_ERROR("Unimplement non standard control transfer request to simple device");
    // TransferHandle 析构时会自动释放
    responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
}


void usbipdcpp::SimpleVirtualDeviceHandler::request_set_address(std::uint16_t address, std::uint32_t *status) {
    SPDLOG_WARN("Unimplement request_set_address to simple device");
}

void usbipdcpp::SimpleVirtualDeviceHandler::request_set_configuration(std::uint16_t configuration_value,
                                                                      std::uint32_t *p_status) {
    if (configuration_value == 0 || configuration_value == handle_device.configuration_value)
        *p_status = 0;
    else
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
}

void usbipdcpp::SimpleVirtualDeviceHandler::request_set_descriptor(std::uint8_t desc_type, std::uint8_t desc_index,
                                                                   std::uint16_t language_id,
                                                                   std::uint16_t descriptor_length,
                                                                   const data_type &descriptor,
                                                                   std::uint32_t *p_status) {
    SPDLOG_WARN("Unimplement request_set_descriptor to simple device");
    // 设备不支持 SET_DESCRIPTOR（USB 2.0 §9.4.8 该请求可选），按规范回 STALL，
    // 不能留默认 OK 让主机误以为设置成功
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
}

usbipdcpp::data_type usbipdcpp::SimpleVirtualDeviceHandler::get_other_speed_descriptor(std::uint8_t language_id,
                                                                                       std::uint16_t descriptor_length,
                                                                                       std::uint32_t *p_status) {
    // USB 2.0 §9.6.4: other_speed_configuration 与 Device_Qualifier 一样只由
    // 高速能力设备提供；全速/低速设备收到该请求按规范回 STALL
    if (static_cast<UsbSpeed>(handle_device.speed) != UsbSpeed::High) {
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
        return {};
    }
    // 结构与配置描述符相同，但描述的是另一速度（高速设备的另一速度 = 全速）：
    // bDescriptorType 改 0x07，端点 wMaxPacketSize 换成全速值
    auto desc = get_configuration_descriptor(language_id, descriptor_length, p_status);
    if (!desc.empty() && desc.size() >= 2) {
        desc[1] = static_cast<std::uint8_t>(DescriptorType::OtherSpeedConfiguration);
        convert_endpoints_to_full_speed(desc);
    }
    return desc;
}

void usbipdcpp::SimpleVirtualDeviceHandler::set_descriptor(std::uint16_t configuration_value) {
    SPDLOG_WARN("Unimplement set_descriptor to simple device");
}
