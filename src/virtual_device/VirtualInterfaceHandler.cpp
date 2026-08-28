#include "usbipdcpp/virtual_device/VirtualInterfaceHandler.h"

#include "usbipdcpp/Session.h"
#include "usbipdcpp/protocol.h"

using namespace usbipdcpp;

void VirtualInterfaceHandler::handle_bulk_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                   std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
                                                   TransferHandle transfer, std::error_code &ec) {
    SPDLOG_TRACE("虚拟接口在端口{:04x}默认实现的块传输实现", ep.address);
    // TransferHandle 析构时会自动释放
    session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
}

void VirtualInterfaceHandler::handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                        std::uint32_t transfer_flags,
                                                        std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                                        std::error_code &ec) {
    SPDLOG_TRACE("虚拟接口在端口{:04x}默认实现的中断传输实现", ep.address);
    // TransferHandle 析构时会自动释放
    session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
}

void VirtualInterfaceHandler::handle_isochronous_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                          std::uint32_t transfer_flags,
                                                          std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                                          int num_iso_packets, std::error_code &ec) {
    SPDLOG_TRACE("虚拟接口在端口{:04x}默认实现的等时传输实现", ep.address);
    // TransferHandle 析构时会自动释放
    session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
}

void VirtualInterfaceHandler::handle_non_standard_request_type_control_urb(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup, TransferHandle transfer, std::error_code &ec) {
    SPDLOG_TRACE("虚拟接口在端口{:04x}的默认非标准控制传输实现", ep.address);
    // TransferHandle 析构时会自动释放
    session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
}

void VirtualInterfaceHandler::handle_non_standard_request_type_control_urb_to_endpoint(
        std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
        const SetupPacket &setup, TransferHandle transfer, std::error_code &ec) {
    SPDLOG_TRACE("接受者为端口地址{:04x}的默认非标准控制传输实现", ep.address);
    // TransferHandle 析构时会自动释放
    session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
}

// 标准请求回调的默认实现在头文件内联（都很简单，头文件即文档），cpp 只保留数据面
