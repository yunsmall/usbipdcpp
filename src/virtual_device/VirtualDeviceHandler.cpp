#include "virtual_device/VirtualDeviceHandler.h"

#include "Session.h"
#include "constant.h"
#include "protocol.h"
#include "virtual_device/UvcConstants.h"
#include "virtual_device/VirtualInterfaceHandler.h"

using namespace usbipdcpp;

void VirtualDeviceHandler::receive_urb(UsbIpCommand::UsbIpCmdSubmit cmd, UsbEndpoint ep,
                                       std::optional<UsbInterface> interface, usbipdcpp::error_code &ec) {
    auto seqnum = cmd.header.seqnum;
    auto transfer_flags = cmd.transfer_flags;
    auto transfer_buffer_length = cmd.transfer_buffer_length;
    const auto &setup_packet = cmd.setup;

    dispatch_urb(cmd, seqnum, ep, interface, transfer_flags, transfer_buffer_length, setup_packet, ec);
}

void VirtualDeviceHandler::dispatch_urb(const UsbIpCommand::UsbIpCmdSubmit &cmd, std::uint32_t seqnum,
                                        const UsbEndpoint &ep, std::optional<UsbInterface> &interface,
                                        std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
                                        const SetupPacket &setup_packet, usbipdcpp::error_code &ec) {
    // 控制传输较少，Bulk/Interrupt 更常见
    // bmAttributes 只取 bit0-1 传输类型，bit2-3 是 ISO sync type，bit4-5 是 usage type
    const auto xfer_type = ep.attributes & 0x03;
    if (xfer_type == static_cast<std::uint8_t>(EndpointAttributes::Control)) [[unlikely]] {
        SPDLOG_TRACE("控制传输: type={:02x} req={:02x} val={:04x} idx={:04x} len={}", setup_packet.request_type,
                    setup_packet.request, setup_packet.value, setup_packet.index, setup_packet.length);
        handle_control_urb(seqnum, ep, transfer_flags, transfer_buffer_length, setup_packet, std::move(cmd.transfer),
                           ec);
    }
    else if (interface.has_value()) [[likely]] {
        auto &intf = interface.value();
        // Bulk 和 Interrupt 最常见
        if (xfer_type == static_cast<std::uint8_t>(EndpointAttributes::Bulk)) [[likely]] {
            SPDLOG_TRACE("块传输 ep={:02x} len={}", ep.address, transfer_buffer_length);
            handle_bulk_transfer(seqnum, ep, intf, transfer_flags, transfer_buffer_length, std::move(cmd.transfer), ec);
        }
        else if (xfer_type == static_cast<std::uint8_t>(EndpointAttributes::Interrupt)) {
            SPDLOG_TRACE("中断传输 ep={:02x} len={}", ep.address, transfer_buffer_length);
            handle_interrupt_transfer(seqnum, ep, intf, transfer_flags, transfer_buffer_length, std::move(cmd.transfer),
                                      ec);
        }
        else if (xfer_type == static_cast<std::uint8_t>(EndpointAttributes::Isochronous)) {
            SPDLOG_TRACE("等时传输 ep={:02x} len={} iso_packets={}", ep.address, transfer_buffer_length,
                        cmd.number_of_packets);
            int num_iso = (cmd.number_of_packets != 0 && cmd.number_of_packets != 0xFFFFFFFF)
                                  ? static_cast<int>(cmd.number_of_packets)
                                  : 0;
            handle_isochronous_transfer(seqnum, ep, intf, transfer_flags, transfer_buffer_length,
                                        std::move(cmd.transfer), num_iso, ec);
        }
        else [[unlikely]] {
            SPDLOG_DEBUG("端口{:02x}的未知传输类型：{}", ep.address, ep.attributes);
            ec = make_error_code(ErrorType::INVALID_ARG);
        }
    }
    else [[unlikely]] {
        SPDLOG_ERROR("非控制传输却不存在目标接口");
        ec = make_error_code(ErrorType::INTERNAL_ERROR);
    }
}

void VirtualDeviceHandler::setup_interface_handlers() {
    // 将各接口的 TransferOperator 注册到设备级路由表，按端点分发
    auto *device_op = static_cast<VirtualDeviceTransferOperator *>(get_transfer_operator());

    for (auto &intf: handle_device.interfaces) {
        if (intf.handler) {
            auto *virtual_handler = intf.handler.get();
            if (virtual_handler) {
                virtual_handler->set_device_handler(this);
                // 把接口级 TransferOperator 注册到该接口所有 alt 的所有端点
                auto *if_op = virtual_handler->get_transfer_operator();
                for (auto &alt_eps: intf.endpoints) {
                    for (auto &ep: alt_eps) {
                        device_op->register_endpoint_operator(ep.address, if_op);
                    }
                }
                virtual_handler->on_setup_interface_handlers();
            }
        }
    }
}

void VirtualDeviceHandler::on_new_connection(Session &current_session, error_code &ec) {
    AbstDeviceHandler::on_new_connection(current_session, ec);
    for (auto &intf: handle_device.interfaces) {
        if (intf.handler) {
            intf.handler->on_new_connection(current_session, ec);
            if (ec) {
                break;
            }
        }
    }
}

void VirtualDeviceHandler::on_disconnection(error_code &ec) {
    for (auto &intf: handle_device.interfaces) {
        if (intf.handler) {
            intf.handler->on_disconnection(ec);
            if (ec) {
                break;
            }
        }
    }
    AbstDeviceHandler::on_disconnection(ec);
}

void VirtualDeviceHandler::change_device_ep0_max_size_by_speed() {
    auto speed = static_cast<UsbSpeed>(handle_device.speed);
    switch (speed) {
        case UsbSpeed::Unknown: {
            SPDLOG_WARN("Unknown device speed");
            [[fallthrough]];
        }
        case UsbSpeed::Low: {
            handle_device.ep0_in.max_packet_size = 8;
            handle_device.ep0_out.max_packet_size = 8;
            break;
        }
        case UsbSpeed::Full:
        case UsbSpeed::High:
        case UsbSpeed::Wireless:
        // In the following two standards, you start out in high speed mode,
        // so you fill in 64, and then when you switch to super fast it changes to 512,
        // so you fill in 9 here
        case UsbSpeed::Super:
        case UsbSpeed::SuperPlus: {
            handle_device.ep0_in.max_packet_size = 64;
            handle_device.ep0_out.max_packet_size = 64;
            break;
        }
        default: {
            SPDLOG_WARN("invalid speed value");
            break;
        }
    }
}

void VirtualDeviceHandler::handle_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep, std::uint32_t transfer_flags,
                                              std::uint32_t transfer_buffer_length, const SetupPacket &setup_packet,
                                              TransferHandle transfer, std::error_code &ec) {
    // 获取 GenericTransfer 指针
    auto *trx = GenericTransfer::from_handle(transfer.get());

    auto recipient = static_cast<RequestRecipient>(setup_packet.calc_recipient());
    // 标准的请求全在这里处理了
    //  大多数控制请求是标准请求
    if (setup_packet.calc_request_type() == static_cast<std::uint8_t>(RequestType::Standard)) [[likely]] {
        auto status = static_cast<std::uint32_t>(UrbStatusType::StatusOK);
        switch (recipient) {
            case RequestRecipient::Device: {
                SPDLOG_TRACE("发给设备");
                auto std_request = static_cast<StandardRequest>(setup_packet.calc_standard_request());

                if (setup_packet.is_out()) {
                    switch (std_request) {
                        case StandardRequest::ClearFeature: {
                            SPDLOG_TRACE("设备ClearFeature");
                            request_clear_feature(setup_packet.value, &status);
                            break;
                        }
                        case StandardRequest::SetAddress: {
                            SPDLOG_TRACE("设备SetAddress");
                            request_set_address(setup_packet.value, &status);
                            break;
                        }
                        case StandardRequest::SetConfiguration: {
                            SPDLOG_TRACE("设备SetConfiguration");
                            request_set_configuration(setup_packet.value, &status);
                            break;
                        }
                        case StandardRequest::SetDescriptor: {
                            SPDLOG_TRACE("设备SetDescriptor");
                            // 从 transfer_handle 获取 OUT 数据
                            data_type out_data(trx->data.begin(), trx->data.begin() + transfer_buffer_length);
                            request_set_descriptor(setup_packet.value >> 8, setup_packet.value & 0x00FF,
                                                   setup_packet.index, setup_packet.length, out_data, &status);
                            break;
                        }
                        case StandardRequest::SetFeature: {
                            SPDLOG_TRACE("设备SetFeature");
                            request_set_feature(setup_packet.value, &status);
                            break;
                        }
                        default: {
                            SPDLOG_WARN("Device Unhandled StandardRequest {}", static_cast<int>(std_request));
                            status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
                        }
                    }
                    // transfer 析构时自动释放
                    session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(
                            seqnum, status, 0));
                }
                else {
                    data_type result{};
                    switch (std_request) {
                        case StandardRequest::GetConfiguration: {
                            SPDLOG_TRACE("设备GetConfiguration");
                            auto ret = request_get_configuration(&status);
                            vector_append_to_net(result, ret);
                            break;
                        }
                        case StandardRequest::GetDescriptor: {
                            SPDLOG_TRACE("设备GetDescriptor");
                            result = request_get_descriptor(setup_packet.value >> 8, setup_packet.value,
                                                            setup_packet.length, &status);
                            if (setup_packet.length < result.size()) {
                                result.resize(setup_packet.length);
                            }
                            break;
                        }
                        case StandardRequest::GetStatus: {
                            SPDLOG_TRACE("设备GetStatus");
                            auto gotten_status = request_get_status(&status);
                            vector_append_to_net(result, gotten_status);
                            break;
                        }
                        default: {
                            SPDLOG_WARN("Device Unhandled StandardRequest {}", static_cast<int>(std_request));
                        }
                    }
                    // 将数据写入 transfer_handle
                    trx->data = std::move(result);
                    trx->actual_length = trx->data.size();
                    trx->data_offset = 0; // 虚拟设备没有 setup 包偏移

                    session->submit_ret_submit(
                            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                                    seqnum, status, static_cast<std::uint32_t>(trx->actual_length),
                                    std::move(transfer)));
                }
                break;
            }
            case RequestRecipient::Interface: {
                SPDLOG_TRACE("发给接口");
                auto intf_idx = setup_packet.index;
                if (intf_idx >= handle_device.interfaces.size()) {
                    SPDLOG_WARN("接口号{}越界（总共{}个接口），返回EPIPE", intf_idx, handle_device.interfaces.size());
                    session->submit_ret_submit(
                            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                    return;
                }
                auto handler = handle_device.interfaces[intf_idx].handler;
                if (handler) {
                    StandardRequest std_request = static_cast<StandardRequest>(setup_packet.calc_standard_request());
                    if (setup_packet.is_out()) {
                        switch (std_request) {
                            case StandardRequest::ClearFeature: {
                                SPDLOG_TRACE("接口request_clear_feature");
                                handler->request_clear_feature(setup_packet.value, &status);
                                break;
                            }
                            case StandardRequest::SetFeature: {
                                SPDLOG_TRACE("接口request_set_feature");
                                handler->request_set_feature(setup_packet.value, &status);
                                break;
                            }
                            case StandardRequest::SetInterface: {
                                SPDLOG_DEBUG("SET_INTERFACE: intf={} alt={}", intf_idx, setup_packet.value);
                                handler->request_set_interface(setup_packet.value, &status);
                                if (status == 0 &&
                                    setup_packet.value < handle_device.interfaces[intf_idx].endpoints.size())
                                    handle_device.interfaces[intf_idx].current_altsetting =
                                            static_cast<std::uint8_t>(setup_packet.value);
                                break;
                            }
                            default: {
                                SPDLOG_WARN("Interface Unhandled StandardRequest {}", static_cast<int>(std_request));
                            }
                        }
                        // transfer 析构时自动释放
                        session->submit_ret_submit(
                                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(
                                        seqnum, status,
                                        0 // 控制传输 OUT 命令，无数据阶段
                                        ));
                    }
                    else {
                        data_type result{};
                        switch (std_request) {
                            case StandardRequest::GetInterface: {
                                SPDLOG_TRACE("接口request_get_interface");
                                auto ret = this->request_get_interface(setup_packet.index, &status);
                                vector_append_to_net(result, ret);
                                break;
                            }
                            case StandardRequest::GetStatus: {
                                SPDLOG_TRACE("接口request_get_status");
                                auto ret = handler->request_get_status(&status);
                                vector_append_to_net(result, ret);
                                break;
                            }
                            case StandardRequest::GetDescriptor: {
                                SPDLOG_TRACE("接口request_get_descriptor");
                                result = handler->request_get_descriptor(setup_packet.value >> 8,
                                                                         setup_packet.value & 0x00FF,
                                                                         setup_packet.length, &status);
                                if (setup_packet.length < result.size()) {
                                    result.resize(setup_packet.length);
                                }
                                break;
                            }
                            default: {
                                SPDLOG_WARN("Interface Unhandled StandardRequest {}", static_cast<int>(std_request));
                            }
                        }
                        // 将数据写入 transfer_handle
                        trx->data = std::move(result);
                        trx->actual_length = trx->data.size();
                        trx->data_offset = 0;

                        session->submit_ret_submit(
                                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                                        seqnum, status, static_cast<std::uint32_t>(trx->actual_length),
                                        std::move(transfer)));
                    }
                }
                else {
                    SPDLOG_ERROR("接口未注册handler，无法处理发去接口的信息");
                    ec = make_error_code(ErrorType::INVALID_ARG);
                    // transfer 析构时自动释放
                    session->submit_ret_submit(
                            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                    return;
                }
                break;
            }
            case RequestRecipient::Endpoint: {
                SPDLOG_TRACE("发给端点");
                auto find_ret = handle_device.find_ep(setup_packet.index);
                if (find_ret) [[likely]] {
                    // auto &target_ep = find_ret->first;
                    auto &intf = find_ret->second;
                    if (intf) [[likely]] {
                        auto handler = intf->handler;
                        if (handler) [[likely]] {
                            StandardRequest std_request =
                                    static_cast<StandardRequest>(setup_packet.calc_standard_request());
                            if (setup_packet.is_out()) {
                                switch (std_request) {
                                    case StandardRequest::ClearFeature: {
                                        SPDLOG_TRACE("端点request_endpoint_clear_feature");
                                        handler->request_endpoint_clear_feature(setup_packet.value, setup_packet.index,
                                                                                &status);
                                        break;
                                    }
                                    case StandardRequest::SetFeature: {
                                        SPDLOG_TRACE("端点request_endpoint_set_feature");
                                        handler->request_endpoint_set_feature(setup_packet.value, setup_packet.index,
                                                                              &status);
                                        break;
                                    }
                                    default: {
                                        SPDLOG_WARN("Endpoint {:04x} Unhandled StandardRequest {}", setup_packet.index,
                                                    static_cast<int>(std_request));
                                    }
                                }
                                // transfer 析构时自动释放
                                session->submit_ret_submit(
                                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(
                                                seqnum, status,
                                                0 // 控制传输 OUT 命令，无数据阶段
                                                ));
                            }
                            else {
                                data_type result{};
                                switch (std_request) {
                                    case StandardRequest::GetStatus: {
                                        SPDLOG_TRACE("端点request_endpoint_get_status");
                                        auto gotten_status =
                                                handler->request_endpoint_get_status(setup_packet.index, &status);
                                        vector_append_to_net(result, gotten_status);
                                        break;
                                    }
                                    case StandardRequest::SynchFrame: {
                                        SPDLOG_TRACE("端点request_endpoint_sync_frame");
                                        handler->request_endpoint_sync_frame(setup_packet.index, &status);
                                        break;
                                    }
                                    default: {
                                        SPDLOG_WARN("Endpoint {:04x} Unhandled StandardRequest {}", setup_packet.index,
                                                    static_cast<int>(std_request));
                                    }
                                }
                                // 将数据写入 transfer_handle
                                trx->data = std::move(result);
                                trx->actual_length = trx->data.size();
                                trx->data_offset = 0;

                                session->submit_ret_submit(
                                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                                                seqnum, status, static_cast<std::uint32_t>(trx->actual_length),
                                                std::move(transfer)));
                            }
                        }
                        else {
                            SPDLOG_ERROR("端点{:04x}所在的接口没注册对应handler", setup_packet.value);
                            ec = make_error_code(ErrorType::INVALID_ARG);
                            // transfer 析构时自动释放
                            session->submit_ret_submit(
                                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                            return;
                        }
                    }
                    else {
                        SPDLOG_ERROR("端点{:04x}没有对应的接口", setup_packet.value);
                        ec = make_error_code(ErrorType::INVALID_ARG);
                        // transfer 析构时自动释放
                        session->submit_ret_submit(
                                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                        return;
                    }
                }
                break;
            }
            case RequestRecipient::Other: {
                SPDLOG_TRACE("发给其他");
                SPDLOG_WARN("未实现去其他地方的包");
                // transfer 析构时自动释放
                session->submit_ret_submit(
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                break;
            }
            default: {
                SPDLOG_WARN("未知去往目标");
                // transfer 析构时自动释放
                session->submit_ret_submit(
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
            }
        }
    }
    else {
        switch (recipient) {
            case RequestRecipient::Device: {
                SPDLOG_TRACE("发给设备的非标准控制传输包");
                handle_non_standard_request_type_control_urb(seqnum, ep, transfer_flags, transfer_buffer_length,
                                                             setup_packet, std::move(transfer), ec);
                break;
            }
            case RequestRecipient::Interface: {
                SPDLOG_TRACE("发给{}号接口的非标准控制传输包", setup_packet.index);
                auto intf_idx = setup_packet.index & 0xFF;
                if (intf_idx >= handle_device.interfaces.size()) {
                    SPDLOG_WARN("接口号{}越界（总共{}个接口），返回EPIPE", intf_idx, handle_device.interfaces.size());
                    session->submit_ret_submit(
                            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                    return;
                }
                auto handler = handle_device.interfaces[intf_idx].handler;
                if (handler) {
                    handler->handle_non_standard_request_type_control_urb(
                            seqnum, ep, transfer_flags, transfer_buffer_length, setup_packet, std::move(transfer), ec);
                }
                else {
                    SPDLOG_ERROR("接口未注册handler，无法处理发往接口的信息");
                    ec = make_error_code(ErrorType::INVALID_ARG);
                    // transfer 析构时自动释放
                    session->submit_ret_submit(
                            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                    return;
                }
                break;
            }
            case RequestRecipient::Endpoint: {
                SPDLOG_TRACE("发给{}号地址端口的非标准控制传输包", setup_packet.index);
                auto find_ret = handle_device.find_ep(setup_packet.index);
                if (find_ret) {
                    auto &intf = find_ret->second;
                    if (intf) [[likely]] {
                        auto handler = intf->handler;
                        if (handler) [[likely]] {
                            handler->handle_non_standard_request_type_control_urb_to_endpoint(
                                    seqnum, ep, transfer_flags, transfer_buffer_length, setup_packet,
                                    std::move(transfer), ec);
                        }
                        else {
                            SPDLOG_ERROR("端点{:04x}所在的接口没注册对应handler", setup_packet.index);
                            ec = make_error_code(ErrorType::INVALID_ARG);
                            session->submit_ret_submit(
                                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                            return;
                        }
                    }
                    else {
                        SPDLOG_ERROR("端点{:04x}没有对应的接口", setup_packet.index);
                        ec = make_error_code(ErrorType::INVALID_ARG);
                        session->submit_ret_submit(
                                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                        return;
                    }
                }
                else {
                    SPDLOG_ERROR("找不到端点{:04x}", setup_packet.index);
                    ec = make_error_code(ErrorType::INVALID_ARG);
                    session->submit_ret_submit(
                            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                    return;
                }
                break;
            }
            case RequestRecipient::Other: {
                SPDLOG_WARN("未实现去其他地方的包");
                // transfer 析构时自动释放
                session->submit_ret_submit(
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                break;
            }
            default: {
                SPDLOG_WARN("未知去往目标");
                // transfer 析构时自动释放
                session->submit_ret_submit(
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
            }
        }
    }
}

void VirtualDeviceHandler::handle_bulk_transfer(std::uint32_t seqnum, const UsbEndpoint &ep, UsbInterface &interface,
                                                std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
                                                TransferHandle transfer, std::error_code &ec) {
    if (interface.handler) [[likely]] {
        interface.handler->handle_bulk_transfer(seqnum, ep, transfer_flags, transfer_buffer_length, std::move(transfer),
                                                ec);
    }
    else {
        SPDLOG_ERROR("端点{:04x}所在的接口没注册handler", ep.address);
        // transfer 析构时自动释放
        session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
    }
}

void VirtualDeviceHandler::handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                     UsbInterface &interface, std::uint32_t transfer_flags,
                                                     std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                                     std::error_code &ec) {

    if (interface.handler) [[likely]] {
        interface.handler->handle_interrupt_transfer(seqnum, ep, transfer_flags, transfer_buffer_length,
                                                     std::move(transfer), ec);
    }
    else {
        SPDLOG_ERROR("端点{:04x}所在的接口没注册handler", ep.address);
        // transfer 析构时自动释放
        session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
    }
}

void VirtualDeviceHandler::handle_isochronous_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                       UsbInterface &interface, std::uint32_t transfer_flags,
                                                       std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                                       int num_iso_packets, std::error_code &ec) {
    if (interface.handler) [[likely]] {
        interface.handler->handle_isochronous_transfer(seqnum, ep, transfer_flags, transfer_buffer_length,
                                                       std::move(transfer), num_iso_packets, ec);
    }
    else {
        SPDLOG_ERROR("端点{:04x}所在的接口没注册handler", ep.address);
        // transfer 析构时自动释放
        session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
    }
}

void VirtualDeviceHandler::handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) {
    for (auto &interface: handle_device.interfaces) {
        if (interface.handler) {
            interface.handler->handle_unlink_seqnum(unlink_seqnum, cmd_seqnum);
        }
    }
}

std::uint8_t VirtualDeviceHandler::request_get_configuration(std::uint32_t *p_status) {
    return handle_device.configuration_value;
}

data_type VirtualDeviceHandler::request_get_descriptor(std::uint8_t type, std::uint8_t language_id,
                                                       std::uint16_t descriptor_length, std::uint32_t *p_status) {
    auto desc_type = static_cast<DescriptorType>(type);
    switch (desc_type) {
        case DescriptorType::Device: {
            SPDLOG_TRACE("设备get_device_descriptor");
            return get_device_descriptor(language_id, descriptor_length, p_status);
            break;
        }
        case DescriptorType::Configuration: {
            SPDLOG_TRACE("设备get_configuration_descriptor");
            return get_configuration_descriptor(language_id, descriptor_length, p_status);
            break;
        }
        case DescriptorType::DeviceQualifier: {
            SPDLOG_TRACE("设备get_device_qualifier_descriptor");
            return get_device_qualifier_descriptor(language_id, descriptor_length, p_status);
            break;
        }
        case DescriptorType::BOS: {
            SPDLOG_TRACE("设备get_bos_descriptor");
            return get_bos_descriptor(language_id, descriptor_length, p_status);
            break;
        }
        case DescriptorType::String: {
            SPDLOG_TRACE("设备get_string_descriptor");
            return get_string_descriptor(language_id, descriptor_length, p_status);
            break;
        }
        case DescriptorType::OtherSpeedConfiguration: {
            SPDLOG_TRACE("设备get_other_speed_descriptor");
            return get_other_speed_descriptor(language_id, descriptor_length, p_status);
            break;
        }
        default: {
            SPDLOG_DEBUG("请求非标准描述符 {:08b}", type);
            return get_custom_descriptor(type, language_id, descriptor_length, p_status);
        }
    }
}

std::uint8_t VirtualDeviceHandler::request_get_interface(std::uint16_t intf, std::uint32_t *p_status) {
    auto handler = handle_device.interfaces[intf].handler;
    if (handler) {
        return handler->request_get_interface(p_status);
    }
    else {
        SPDLOG_ERROR("接口未注册handler，无法处理");
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
        return -1;
    }
}

void VirtualDeviceHandler::request_set_interface(std::uint16_t alternate_setting, std::uint16_t intf,
                                                 std::uint32_t *p_status) {
    auto &interface = handle_device.interfaces[intf];
    auto handler = interface.handler;
    if (handler) {
        handler->request_set_interface(alternate_setting, p_status);
        if (*p_status == 0 && alternate_setting < interface.endpoints.size()) {
            interface.current_altsetting = static_cast<std::uint8_t>(alternate_setting);
        }
    }
    else {
        SPDLOG_ERROR("接口未注册handler，无法处理");
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
        return;
    }
}

data_type VirtualDeviceHandler::get_device_descriptor(std::uint16_t language_id, std::uint16_t descriptor_length,
                                                      std::uint32_t *p_status) {
    std::shared_lock lock(data_mutex);
    std::uint16_t version_bcd = usb_version;
    data_type desc = {0x12, // bLength
                      static_cast<std::uint8_t>(DescriptorType::Device), // bDescriptorType: Device
                      static_cast<std::uint8_t>(version_bcd), // bcdUSB: USB 2.0
                      static_cast<std::uint8_t>(version_bcd >> 8),
                      handle_device.device_class, // bDeviceClass
                      handle_device.device_subclass, // bDeviceSubClass
                      handle_device.device_protocol, // bDeviceProtocol
                      static_cast<std::uint8_t>(handle_device.ep0_in.max_packet_size), // bMaxPacketSize0
                      static_cast<std::uint8_t>(handle_device.vendor_id), // idVendor
                      static_cast<std::uint8_t>(handle_device.vendor_id >> 8),
                      static_cast<std::uint8_t>(handle_device.product_id), // idProduct
                      static_cast<std::uint8_t>(handle_device.product_id >> 8),
                      handle_device.device_bcd.minor, // bcdDevice
                      handle_device.device_bcd.major,
                      string_manufacturer_value, // iManufacturer
                      string_product_value, // iProduct
                      string_serial_value, // iSerial
                      handle_device.num_configurations};

    if (descriptor_length < desc.size()) {
        desc.resize(descriptor_length);
    }
    return desc;
}

data_type VirtualDeviceHandler::get_bos_descriptor(std::uint16_t language_id, std::uint16_t descriptor_length,
                                                   std::uint32_t *p_status) {
    std::shared_lock lock(data_mutex);
    // BOS header (5) + USB 2.0 Extension Device Capability (7) = 12 bytes
    data_type desc = {
            0x05, // bLength
            static_cast<std::uint8_t>(DescriptorType::BOS), 0x0C, 0x00, // wTotalLength = 12
            0x01, // bNumCapabilities = 1
            // USB 2.0 Extension Device Capability
            0x07, // bLength
            0x10, // bDescriptorType: DEVICE CAPABILITY
            0x02, // bDevCapabilityType: USB 2.0 EXTENSION
            0x00, 0x00, 0x00, 0x00, // bmAttributes: no LPM
    };
    if (descriptor_length < desc.size()) {
        desc.resize(descriptor_length);
    }
    return desc;
}

data_type VirtualDeviceHandler::get_configuration_descriptor(std::uint16_t language_id, std::uint16_t descriptor_length,
                                                             std::uint32_t *p_status) {
    std::shared_lock lock(data_mutex);
    data_type desc = {
            0x09, // bLength
            static_cast<std::uint8_t>(DescriptorType::Configuration), // bDescriptorType: Configuration
            0x00,
            0x00, // wTotalLength: to be filled below
            static_cast<std::uint8_t>(handle_device.interfaces.size()), // bNumInterfaces
            handle_device.configuration_value, // bConfigurationValue
            string_configuration_value, // iConfiguration
            0x80, // bmAttributes Bus Powered
            0xFA, // bMaxPower 500mA
    };
    // IAD: Windows 要求多接口 UVC 设备在配置描述符里包含 IAD
    // UVC 1.5 Table 3-1: iFunction 必须等于 VC interface 的 iInterface
    if (handle_device.device_class == 0xEF) {
        auto iFunc = handle_device.interfaces[0].handler
                             ? handle_device.interfaces[0].handler->get_string_interface_value()
                             : std::uint8_t{0};
        desc.insert(desc.end(), {
                                        0x08, // bLength
                                        0x0B, // bDescriptorType: IAD
                                        0x00, // bFirstInterface
                                        static_cast<std::uint8_t>(handle_device.interfaces.size()), // bInterfaceCount
                                        handle_device.interfaces[0].interface_class, // bFunctionClass
                                        0x03, // bFunctionSubClass: SC_VIDEO_INTERFACE_COLLECTION
                                        0x00, // bFunctionProtocol (PC_PROTOCOL_UNDEFINED)
                                        iFunc, // iFunction: must equal VC iInterface per spec
                                });
    }
    for (std::size_t i = 0; i < handle_device.interfaces.size(); i++) {
        auto &intf = handle_device.interfaces[i];
        auto class_specific_descriptor = intf.handler->get_class_specific_descriptor();

        auto num_alts = intf.endpoints.size();
        if (num_alts == 0)
            num_alts = 1; // 至少保证 alt 0 如果没有初始化

        for (std::size_t alt = 0; alt < num_alts; alt++) {
            auto &alt_endpoints = (alt < intf.endpoints.size()) ? intf.endpoints[alt] : intf.endpoints[0];
            data_type intf_desc = {
                    0x09, // bLength
                    static_cast<std::uint8_t>(DescriptorType::Interface), // bDescriptorType: Interface
                    static_cast<std::uint8_t>(i), // bInterfaceNum
                    static_cast<std::uint8_t>(alt), // bAlternateSettings
                    static_cast<std::uint8_t>(alt_endpoints.size()), // bNumEndpoints
                    intf.interface_class, // bInterfaceClass
                    intf.interface_subclass, // bInterfaceSubClass
                    intf.interface_protocol, // bInterfaceProtocol
                    intf.handler->get_string_interface_value(), // iInterface
            };
            // class-specific 描述符只放 alt 0（TinyUSB 做法），alt>0 只放端点
            // 若放所有 alt，config descriptor 会被撑到超过 255 字节导致 Windows 截断解析失败
            if (alt == 0 && !class_specific_descriptor.empty()) {
                intf_desc.insert(intf_desc.end(), class_specific_descriptor.begin(), class_specific_descriptor.end());
            }
            for (auto &endpoint: alt_endpoints) {
                data_type ep_desc = {0x07, // bLength
                                     static_cast<std::uint8_t>(DescriptorType::Endpoint),
                                     endpoint.address,
                                     endpoint.attributes,
                                     static_cast<std::uint8_t>(endpoint.max_packet_size),
                                     static_cast<std::uint8_t>(endpoint.max_packet_size >> 8),
                                     endpoint.interval};
                intf_desc.insert(intf_desc.end(), ep_desc.begin(), ep_desc.end());

                // UVC 1.5 Table 3-12: VC interrupt endpoint requires class-specific endpoint descriptor
                if (intf.interface_class == CC_VIDEO && intf.interface_subclass == SC_VIDEOCONTROL &&
                    (endpoint.attributes & 0x03) == 0x03) {
                    data_type cs_ep = {
                            0x05, // bLength
                            CS_ENDPOINT, // bDescriptorType
                            EP_INTERRUPT, // bDescriptorSubType
                            static_cast<std::uint8_t>(endpoint.max_packet_size), // wMaxTransferSize low
                            static_cast<std::uint8_t>(endpoint.max_packet_size >> 8), // wMaxTransferSize high
                    };
                    intf_desc.insert(intf_desc.end(), cs_ep.begin(), cs_ep.end());
                }
            }
            desc.insert(desc.end(), intf_desc.begin(), intf_desc.end());
        }
    }
    desc[2] = static_cast<std::uint8_t>(desc.size());
    desc[3] = static_cast<std::uint8_t>(desc.size() >> 8);
    if (descriptor_length < desc.size()) {
        desc.resize(descriptor_length);
    }
    return desc;
}

data_type VirtualDeviceHandler::get_string_descriptor(std::uint8_t language_id, std::uint16_t descriptor_length,
                                                      std::uint32_t *p_status) {
    std::shared_lock lock(data_mutex);
    // 先尝试虚函数：允许子类处理特殊字符串索引（如 Microsoft OS 0xEE）
    if (auto special = get_special_string_descriptor(language_id)) {
        if (descriptor_length < special->size())
            special->resize(descriptor_length);
        return *special;
    }

    if (language_id == 0) [[unlikely]] {
        // language ids - 特殊情况，用于获取支持的语言ID列表
        data_type desc = {4, static_cast<std::uint8_t>(DescriptorType::String), 0x09, 0x04};
        if (descriptor_length < desc.size()) {
            desc.resize(descriptor_length);
        }
        return desc;
    }
    else if (auto string_ret = string_pool.get_string(language_id)) [[likely]] {
        auto &string = string_ret.value();
        data_type desc = {
                static_cast<std::uint8_t>((string.size() + 1) * 2),
                static_cast<std::uint8_t>(DescriptorType::String),
        };
        for (auto &i: string) {
            desc.push_back(static_cast<std::uint8_t>(i));
            desc.push_back(static_cast<std::uint8_t>(i >> 8));
        }
        if (descriptor_length < desc.size()) {
            desc.resize(descriptor_length);
        }
        return desc;
    }
    else {
        SPDLOG_ERROR("非法字符串描述符索引：{}", language_id);
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
        return {};
    }
}

data_type VirtualDeviceHandler::get_device_qualifier_descriptor(std::uint8_t language_id,
                                                                std::uint16_t descriptor_length,
                                                                std::uint32_t *p_status) {
    // USB 2.0 §9.6.2: 高速设备必须返回 other-speed 信息
    // 返回全速模式下的设备描述信息（bMaxPacketSize0 等与高速相同）
    std::shared_lock lock(data_mutex);
    data_type desc = {
            0x0A,                                                           // bLength
            static_cast<std::uint8_t>(DescriptorType::DeviceQualifier),     // bDescriptorType
            usb_version.minor, usb_version.major,                           // bcdUSB
            handle_device.device_class,                                     // bDeviceClass
            handle_device.device_subclass,                                  // bDeviceSubClass
            handle_device.device_protocol,                                  // bDeviceProtocol
            static_cast<std::uint8_t>(handle_device.ep0_in.max_packet_size), // bMaxPacketSize0 (FS)
            handle_device.num_configurations,                               // bNumConfigurations
            0x00,                                                           // bReserved
    };
    if (descriptor_length < desc.size()) {
        desc.resize(descriptor_length);
    }
    return desc;
}

std::optional<data_type> VirtualDeviceHandler::get_special_string_descriptor(std::uint8_t string_index) {
    (void)string_index;
    return std::nullopt;
}

data_type VirtualDeviceHandler::get_custom_descriptor(std::uint8_t type, std::uint8_t language_id,
                                                      std::uint16_t descriptor_length, std::uint32_t *p_status) {
    return {};
}
