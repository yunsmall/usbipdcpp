// #define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_DEBUG

#include "usbipdcpp/virtual_device/VirtualDeviceTransferOperator.h"

#include <spdlog/spdlog.h>
#include "usbipdcpp/constant.h"

using namespace usbipdcpp;

TransferOperator *VirtualDeviceTransferOperator::get_operator_for_ep(std::uint8_t ep) {
    auto it = ep_operators_.find(ep);
    return (it != ep_operators_.end()) ? it->second : &generic_op_;
}

void *VirtualDeviceTransferOperator::alloc_transfer_handle(std::size_t buffer_length, int num_iso_packets,
                                                           const UsbIpHeaderBasic &header,
                                                           const SetupPacket &setup_packet) {
    // header.ep 是 USB/IP 线格式端点号（不带方向位），需按 direction 补上方向位
    // 再查注册表（ep_operators_ 的键是含方向位的完整端点地址，见
    // setup_interface_handlers 的 register_endpoint_operator）。与
    // UsbIpCmdSubmit::from_socket 还原 real_ep 的逻辑保持一致
    std::uint8_t real_ep = static_cast<std::uint8_t>(header.ep);
    if (header.direction == UsbIpDirection::In)
        real_ep |= 0x80;
    auto *leaf_op = get_operator_for_ep(real_ep);
    return leaf_op->alloc_transfer_handle(buffer_length, num_iso_packets, header, setup_packet);
}

void VirtualDeviceTransferOperator::free_transfer_handle(void *handle) {
    // leaf op 已存入 TransferHandle，正常路径不会走到这里。
    // 如果走了，说明 caller 没有正确使用 TransferHandle::get_operator()。
    SPDLOG_ERROR("VDTO::free_transfer_handle handle={:p} 不应被调用", static_cast<const void *>(handle));
}

std::size_t VirtualDeviceTransferOperator::get_actual_length(void *handle) {
    return generic_op_.get_actual_length(handle);
}

bool VirtualDeviceTransferOperator::transfer_is_in(void *handle) {
    // 防御路径：正常流程 TransferHandle 绑定 leaf op（见 protocol.cpp 的
    // set_handle），to_socket 不会拿到本路由 op
    return generic_op_.transfer_is_in(handle);
}

UsbIpIsoPacketDescriptor VirtualDeviceTransferOperator::get_iso_descriptor(void *handle, int index) {
    return generic_op_.get_iso_descriptor(handle, index);
}

void VirtualDeviceTransferOperator::set_iso_descriptor(void *handle, int index, const UsbIpIsoPacketDescriptor &desc) {
    generic_op_.set_iso_descriptor(handle, index, desc);
}

void VirtualDeviceTransferOperator::send_transfer_data(void *handle, asio::ip::tcp::socket &sock, std::size_t length,
                                                       std::error_code &ec) {
    generic_op_.send_transfer_data(handle, sock, length, ec);
}

void VirtualDeviceTransferOperator::recv_transfer_data(void *handle, asio::ip::tcp::socket &sock, std::size_t length,
                                                       std::error_code &ec) {
    generic_op_.recv_transfer_data(handle, sock, length, ec);
}
