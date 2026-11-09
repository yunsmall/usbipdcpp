#include "usbipdcpp/virtual_device/VirtualInterfaceHandler.h"

#include "usbipdcpp/Session.h"
#include "usbipdcpp/SetupPacket.h"
#include "usbipdcpp/protocol.h"
#include "usbipdcpp/utils/StringPool.h"
#include "usbipdcpp/virtual_device/VirtualDeviceHandler.h"

using namespace usbipdcpp;

namespace {

/**
 * @brief 查找实现 Halt 特性的端点
 * @param interface 端点所属接口
 * @param ep_address 端点地址（含方向位）
 * @return 端点存在且为 Bulk/Interrupt 时返回该端点，否则返回 nullptr。
 *         USB 2.0 §9.4.5 只要求中断和 Bulk 端点实现 Halt，等时/控制端点没有
 *         该特性，SET/CLEAR_FEATURE(ENDPOINT_HALT) 必须回 Request Error
 */
const UsbEndpoint *find_haltable_endpoint(UsbInterface &interface, std::uint8_t ep_address) {
    for (const auto &endpoint: interface.current_endpoints()) {
        if (endpoint.address != ep_address) {
            continue;
        }
        auto type = static_cast<EndpointAttributes>(endpoint.attributes & 0x03);
        if (type == EndpointAttributes::Bulk || type == EndpointAttributes::Interrupt) {
            return &endpoint;
        }
        return nullptr;
    }
    return nullptr;
}

} // namespace

void VirtualInterfaceHandler::set_device_handler(VirtualDeviceHandler *handler) {
    device_handler = handler;
}

void VirtualInterfaceHandler::register_strings(StringPool &pool) {
    string_pool = &pool;
    // 索引 0 保留给 Language ID；0 也表示还没登记，重复 setup 不重复占索引
    if (string_interface == 0) {
        string_interface = pool.new_string(interface_string_to_register());
    }
}

StringPool &VirtualInterfaceHandler::get_string_pool() const {
    return *string_pool;
}

std::wstring VirtualInterfaceHandler::get_string_interface() const {
    if (!string_pool || string_interface == 0) {
        return interface_string_to_register();
    }
    if (auto value = get_string_pool().get_string(string_interface)) {
        return *value;
    }
    return interface_string_to_register();
}

void VirtualInterfaceHandler::sync_string_interface_from(const VirtualInterfaceHandler &other) {
    // 只复制索引，不动任何一张池：调用方是同一设备的接口（UVC 的 VC/VS），
    // 原索引对应的字符串留在池里当空闲槽就行，下次分配会复用。
    // 对方还没登记时保持原值，等它登记后由它复制过来
    if (other.string_interface != 0) {
        string_interface = other.string_interface;
    }
}

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

void VirtualInterfaceHandler::handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) {
    bool cancelled = cancel_pending_urb(unlink_seqnum);
    // 从队列中真的取消了待处理 URB → 回 -ECONNRESET（URB 被取消，且不再发
    // RET_SUBMIT，请求已从队列移除）；找不到（URB 已完成/不存在）→ 回 0。
    // 与内核 stub_tx.c 及本项目 LibusbDeviceHandler 的 unlink 范本一致
    responder->submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(
            cmd_seqnum, cancelled ? static_cast<std::uint32_t>(UrbStatusType::StatusECONNRESET) : 0));
}

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
    // 控制端点（0x00/0x80）与等时端点不实现 Halt，按规范回 STALL
    if (feature_selector == SetupPacket::USB_ENDPOINT_HALT &&
        find_haltable_endpoint(handle_interface, ep_address)) {
        endpoint_halted[ep_address].store(true, std::memory_order_relaxed);
        *p_status = 0;
        return;
    }
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
}

void VirtualInterfaceHandler::request_endpoint_clear_feature(std::uint16_t feature_selector,
                                                             std::uint8_t ep_address, std::uint32_t *p_status) {
    if (feature_selector == SetupPacket::USB_ENDPOINT_HALT &&
        find_haltable_endpoint(handle_interface, ep_address)) {
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
