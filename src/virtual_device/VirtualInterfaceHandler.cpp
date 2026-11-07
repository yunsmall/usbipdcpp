#include "usbipdcpp/virtual_device/VirtualInterfaceHandler.h"

#include "usbipdcpp/Session.h"
#include "usbipdcpp/SetupPacket.h"
#include "usbipdcpp/protocol.h"

using namespace usbipdcpp;

void VirtualInterfaceHandler::handle_bulk_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                   std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
                                                   TransferHandle transfer, std::error_code &ec) {
    SPDLOG_TRACE("虚拟接口在端口{:04x}默认实现的块传输实现", ep.address);
    // TransferHandle 析构时会自动释放
    responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
}

void VirtualInterfaceHandler::handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                        std::uint32_t transfer_flags,
                                                        std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                                        std::error_code &ec) {
    SPDLOG_TRACE("虚拟接口在端口{:04x}默认实现的中断传输实现", ep.address);
    // TransferHandle 析构时会自动释放
    responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
}

void VirtualInterfaceHandler::handle_isochronous_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                          std::uint32_t transfer_flags,
                                                          std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                                          int num_iso_packets, std::error_code &ec) {
    SPDLOG_TRACE("虚拟接口在端口{:04x}默认实现的等时传输实现", ep.address);
    // TransferHandle 析构时会自动释放
    responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
}

void VirtualInterfaceHandler::handle_non_standard_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup, TransferHandle transfer, std::error_code &ec) {
    SPDLOG_TRACE("虚拟接口在端口{:04x}的默认非标准控制传输实现", ep.address);
    // TransferHandle 析构时会自动释放
    responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
}

void VirtualInterfaceHandler::handle_non_standard_request_type_control_urb_to_endpoint(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup, TransferHandle transfer, std::error_code &ec) {
    SPDLOG_TRACE("接受者为端口地址{:04x}的默认非标准控制传输实现", ep.address);
    // TransferHandle 析构时会自动释放
    responder->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
}

// 标准请求回调的默认实现在头文件内联（都很简单，头文件即文档），cpp 只保留数据面

// ========== 端点级标准请求（ENDPOINT_HALT 状态机） ==========
//
// 真实设备由 UDC 维护端点 halt：SET_FEATURE 置位后该端点的 URB 全部以
// -EPIPE 完成，CLEAR_FEATURE 清除后恢复；端点 0 不可 halt（USB 2.0 §9.4.5）。
// 虚拟设备没有硬件 halt，但主机会依赖 GET_STATUS 的 halt 位和 STALL 行为
// （驱动出错恢复时会先 CLEAR_FEATURE 再重试），因此这里维护一份软件状态。

void VirtualInterfaceHandler::reset_endpoint_halt_state() {
    // USB 2.0 §9.4.5：SetConfiguration()/SetInterface() 之后 Halt 必须清零，
    // 即使配置/接口与当前相同。遍历所有 alternate setting，避免只清当前 alt
    for (auto &alt: handle_interface.endpoints) {
        for (auto &ep: alt) {
            endpoint_halted[ep.address].store(false, std::memory_order_relaxed);
        }
    }
}

void VirtualInterfaceHandler::request_endpoint_set_feature(std::uint16_t feature_selector,
                                                           std::uint8_t ep_address, std::uint32_t *p_status) {
    // 控制端点（0x00/0x80）不可设置 halt，按规范回 STALL
    if (feature_selector == SetupPacket::USB_ENDPOINT_HALT && ep_address != 0x00 && ep_address != 0x80) {
        endpoint_halted[ep_address].store(true, std::memory_order_relaxed);
        *p_status = 0;
        return;
    }
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
}

void VirtualInterfaceHandler::request_endpoint_clear_feature(std::uint16_t feature_selector,
                                                             std::uint8_t ep_address, std::uint32_t *p_status) {
    if (feature_selector == SetupPacket::USB_ENDPOINT_HALT && ep_address != 0x00 && ep_address != 0x80) {
        endpoint_halted[ep_address].store(false, std::memory_order_relaxed);
        *p_status = 0;
        return;
    }
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
}

std::uint16_t VirtualInterfaceHandler::request_endpoint_get_status(std::uint8_t ep_address,
                                                                   std::uint32_t *p_status) {
    // bit0 = HALT（USB 2.0 §9.4.5），其余位保留为 0
    *p_status = 0;
    return is_endpoint_halted(ep_address) ? 0x0001 : 0x0000;
}
