#include "DeviceHandler/VirtualDeviceHandler.h"

#include "InterfaceHandler/VirtualInterfaceHandler.h"
#include "Session.h"
#include "protocol.h"

using namespace usbipdcpp;

void VirtualDeviceHandler::dispatch_urb(Session &session, const UsbIpCommand::UsbIpCmdSubmit &cmd, std::uint32_t seqnum,
                                        const UsbEndpoint &ep, std::optional<UsbInterface> &interface,
                                        std::uint32_t transfer_flags,
                                        std::uint32_t transfer_buffer_length, const SetupPacket &setup_packet,
                                        const data_type &out_data,
                                        const std::vector<UsbIpIsoPacketDescriptor> &iso_packet_descriptors,
                                        usbipdcpp::error_code &ec) {
    // if (se)
    DeviceHandlerBase::dispatch_urb(session, cmd, seqnum, ep, interface, transfer_flags, transfer_buffer_length,
                                    setup_packet, out_data,
                                    iso_packet_descriptors, ec);
}

void VirtualDeviceHandler::on_new_connection(error_code &ec) {
    for (auto &intf: handle_device.interfaces) {
        if (intf.handler) {
            intf.handler->on_new_connection(ec);
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
        //In the following two standards, you start out in high speed mode,
        //so you fill in 64, and then when you switch to super fast it changes to 512,
        //so you fill in 9 here
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

void VirtualDeviceHandler::handle_control_urb(Session &session,
                                              std::uint32_t seqnum,
                                              const UsbEndpoint &ep,
                                              std::uint32_t transfer_flags,
                                              std::uint32_t transfer_buffer_length,
                                              const SetupPacket &setup_packet,
                                              const std::vector<std::uint8_t> &out_data,
                                              std::error_code &ec) {
    auto unlink_ret = session.get_unlink_seqnum(seqnum);
    if (!std::get<0>(unlink_ret)) {
        auto recipient = static_cast<RequestRecipient>(setup_packet.calc_recipient());
        //标准的请求全在这里处理了
        if (setup_packet.calc_request_type() == static_cast<std::uint8_t>(RequestType::Standard)) {
            auto status = static_cast<std::uint32_t>(UrbStatusType::StatusOK);
            switch (recipient) {
                case RequestRecipient::Device: {
                    SPDLOG_TRACE("发给设备");
                    auto std_request = static_cast<StandardRequest>(setup_packet.
                        calc_standard_request());

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
                                request_set_descriptor(setup_packet.value >> 8, setup_packet.value & 0x00FF,
                                                       setup_packet.index,
                                                       setup_packet.length, out_data, &status);
                                break;
                            }
                            case StandardRequest::SetFeature: {
                                SPDLOG_TRACE("设备SetFeature");
                                request_set_feature(setup_packet.value, &status);
                                break;
                            }
                            default: {
                                SPDLOG_WARN("Device Unhandled StandardRequest {}",
                                            static_cast<int>(std_request));
                                status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
                            }
                        }
                        session.submit_ret_submit(
                                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(
                                        seqnum,
                                        status
                                        )
                                );
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
                        session.submit_ret_submit(
                                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                                        seqnum, status, result)
                                );

                    }
                    break;
                }
                case RequestRecipient::Interface: {
                    SPDLOG_TRACE("发给接口");
                    auto intf_idx = setup_packet.index;
                    auto handler = handle_device.interfaces[intf_idx].handler;
                    if (handler) {
                        StandardRequest std_request = static_cast<StandardRequest>(setup_packet.
                            calc_standard_request());
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
                                    SPDLOG_TRACE("接口request_set_interface");
                                    handler->request_set_interface(setup_packet.value, &status);
                                    break;
                                }
                                default: {
                                    SPDLOG_WARN("Interface Unhandled StandardRequest {}",
                                                static_cast<int>(std_request));
                                }
                            }
                            session.submit_ret_submit(
                                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(
                                            seqnum,
                                            status
                                            )
                                    );
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
                                    result = handler->request_get_descriptor(
                                            setup_packet.value >> 8, setup_packet.value & 0x00FF,
                                            setup_packet.length, &status);
                                    if (setup_packet.length < result.size()) {
                                        result.resize(setup_packet.length);
                                    }
                                    break;;
                                }
                                default: {
                                    SPDLOG_WARN("Interface Unhandled StandardRequest {}",
                                                static_cast<int>(std_request));
                                }
                            }
                            session.submit_ret_submit(
                                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                                            seqnum, status, result)
                                    );
                        }

                    }
                    else {
                        SPDLOG_ERROR("接口未注册handler，无法处理发去接口的信息");
                        ec = make_error_code(ErrorType::INVALID_ARG);
                        session.submit_ret_submit(
                                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum));
                        return;
                    }
                    break;
                }
                case RequestRecipient::Endpoint: {
                    SPDLOG_TRACE("发给端点");
                    auto find_ret = handle_device.find_ep(setup_packet.index);
                    if (find_ret) {
                        // auto &target_ep = find_ret->first;
                        auto &intf = find_ret->second;
                        if (intf) {
                            auto handler = intf->handler;
                            if (handler) {
                                StandardRequest std_request = static_cast<StandardRequest>(setup_packet.
                                    calc_standard_request());
                                if (setup_packet.is_out()) {
                                    switch (std_request) {
                                        case StandardRequest::ClearFeature: {
                                            SPDLOG_TRACE("端点request_endpoint_clear_feature");
                                            handler->request_endpoint_clear_feature(
                                                    setup_packet.value, setup_packet.index, &status);
                                            break;
                                        }
                                        case StandardRequest::SetFeature: {
                                            SPDLOG_TRACE("端点request_endpoint_set_feature");
                                            handler->request_endpoint_set_feature(
                                                    setup_packet.value, setup_packet.index, &status);
                                            break;
                                        }
                                        default: {
                                            SPDLOG_WARN("Endpoint {:04x} Unhandled StandardRequest {}",
                                                        setup_packet.index,
                                                        static_cast<int>(std_request));
                                        }
                                    }
                                    session.submit_ret_submit(
                                            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(
                                                    seqnum,
                                                    status
                                                    )
                                            );
                                }
                                else {
                                    data_type result{};
                                    switch (std_request) {
                                        case StandardRequest::GetStatus: {
                                            SPDLOG_TRACE("端点request_endpoint_get_status");
                                            auto gotten_status = handler->request_endpoint_get_status(
                                                    setup_packet.index, &status);
                                            vector_append_to_net(result, gotten_status);
                                            break;
                                        }
                                        case StandardRequest::SynchFrame: {
                                            SPDLOG_TRACE("端点request_endpoint_sync_frame");
                                            handler->request_endpoint_sync_frame(setup_packet.index, &status);
                                            break;
                                        }
                                        default: {
                                            SPDLOG_WARN("Endpoint {:04x} Unhandled StandardRequest {}",
                                                        setup_packet.index,
                                                        static_cast<int>(std_request));
                                        }
                                    }
                                    session.submit_ret_submit(
                                            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(
                                                    seqnum,
                                                    status,
                                                    result)
                                            );
                                }
                            }
                            else {
                                SPDLOG_ERROR("端点{:04x}所在的接口没注册对应handler", setup_packet.value);
                                ec = make_error_code(ErrorType::INVALID_ARG);
                                session.submit_ret_submit(
                                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum));
                                return;
                            }
                        }
                        else {
                            SPDLOG_ERROR("端点{:04x}没有对应的接口", setup_packet.value);
                            ec = make_error_code(ErrorType::INVALID_ARG);
                            session.submit_ret_submit(
                                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum));
                            return;
                        }
                    }
                    break;
                }
                case RequestRecipient::Other: {
                    SPDLOG_TRACE("发给其他");
                    SPDLOG_WARN("未实现去其他地方的包");
                    session.submit_ret_submit(
                            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum));
                    break;
                }
                default: {
                    SPDLOG_WARN("未知去往目标");
                    session.submit_ret_submit(
                            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum));
                }
            }
        }
        else {
            switch (recipient) {
                case RequestRecipient::Device: {
                    SPDLOG_TRACE("发给设备的非标准控制传输包");
                    handle_non_standard_request_type_control_urb(session, seqnum, ep, transfer_flags,
                                                                 transfer_buffer_length,
                                                                 setup_packet,
                                                                 out_data, ec);
                    break;
                }
                case RequestRecipient::Interface: {
                    SPDLOG_TRACE("发给{}号接口的非标准控制传输包", setup_packet.index);
                    auto intf_idx = setup_packet.index;
                    auto handler = handle_device.interfaces[intf_idx].handler;
                    if (handler) {
                        handler->handle_non_standard_request_type_control_urb(session, seqnum, ep, transfer_flags,
                                                                              transfer_buffer_length,
                                                                              setup_packet,
                                                                              out_data, ec);
                    }
                    else {
                        SPDLOG_ERROR("接口未注册handler，无法处理发往接口的信息");
                        ec = make_error_code(ErrorType::INVALID_ARG);
                        session.submit_ret_submit(
                                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum));
                        return;
                    }
                    break;
                }
                case RequestRecipient::Endpoint: {
                    SPDLOG_TRACE("发给{}号接口的{:04x}号地址端口的非标准控制传输包", setup_packet.index, ep.address);
                    auto intf_idx = setup_packet.index;
                    auto handler = handle_device.interfaces[intf_idx].handler;
                    if (handler) {
                        handler->handle_non_standard_request_type_control_urb_to_endpoint(
                                session, seqnum, ep, transfer_flags,
                                transfer_buffer_length,
                                setup_packet,
                                out_data, ec);
                    }
                    else {
                        SPDLOG_ERROR("接口未注册handler，无法处理发往接口的信息");
                        ec = make_error_code(ErrorType::INVALID_ARG);
                        session.submit_ret_submit(
                                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum));
                        return;
                    }
                    break;
                }
                case RequestRecipient::Other: {
                    SPDLOG_WARN("未实现去其他地方的包");
                    session.submit_ret_submit(
                            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum));
                    break;
                }
                default: {
                    SPDLOG_WARN("未知去往目标");
                    session.submit_ret_submit(
                            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum));
                }
            }

        }
    }
    else {
        session.submit_ret_unlink_and_then_remove_seqnum_unlink(
                UsbIpResponse::UsbIpRetUnlink::create_ret_unlink_success(
                        std::get<1>(unlink_ret)
                        ),
                seqnum
                );
    }
}

void VirtualDeviceHandler::handle_bulk_transfer(Session &session,
                                                std::uint32_t seqnum,
                                                const UsbEndpoint &ep,
                                                UsbInterface &interface,
                                                std::uint32_t transfer_flags,
                                                std::uint32_t transfer_buffer_length,
                                                const data_type &out_data,
                                                std::error_code &ec) {
    auto unlink_ret = session.get_unlink_seqnum(seqnum);
    if (!std::get<0>(unlink_ret)) {
        if (interface.handler) {
            interface.handler->handle_bulk_transfer(
                    session,
                    seqnum,
                    ep,
                    transfer_flags,
                    transfer_buffer_length,
                    out_data,
                    ec
                    );
        }
        else {
            SPDLOG_ERROR("端点{:04x}所在的接口没注册handler", ep.address);
            session.submit_ret_submit(
                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum));
        }
    }
    else {
        session.submit_ret_unlink_and_then_remove_seqnum_unlink(
                UsbIpResponse::UsbIpRetUnlink::create_ret_unlink_success(
                        std::get<1>(unlink_ret)
                        ),
                seqnum
                );
    }
}

void VirtualDeviceHandler::handle_interrupt_transfer(Session &session,
                                                     std::uint32_t seqnum,
                                                     const UsbEndpoint &ep,
                                                     UsbInterface &interface,
                                                     std::uint32_t transfer_flags,
                                                     std::uint32_t transfer_buffer_length,
                                                     const data_type &out_data,
                                                     std::error_code &ec) {

    auto unlink_ret = session.get_unlink_seqnum(seqnum);
    if (!std::get<0>(unlink_ret)) {
        if (interface.handler) {
            interface.handler->handle_interrupt_transfer(
                    session,
                    seqnum,
                    ep,
                    transfer_flags,
                    transfer_buffer_length,
                    out_data,
                    ec
                    );
        }
        else {
            SPDLOG_ERROR("端点{:04x}所在的接口没注册handler", ep.address);
            session.submit_ret_submit(
                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum));
        }
    }
    else {
        session.submit_ret_unlink_and_then_remove_seqnum_unlink(
                UsbIpResponse::UsbIpRetUnlink::create_ret_unlink_success(
                        std::get<1>(unlink_ret)
                        ),
                seqnum
                );
    }
}

void VirtualDeviceHandler::handle_isochronous_transfer(Session &session,
                                                       std::uint32_t seqnum,
                                                       const UsbEndpoint &ep,
                                                       UsbInterface &interface,
                                                       std::uint32_t transfer_flags,
                                                       std::uint32_t transfer_buffer_length,
                                                       const data_type &req,
                                                       const std::vector<UsbIpIsoPacketDescriptor> &
                                                       iso_packet_descriptors,
                                                       std::error_code &ec) {
    auto unlink_ret = session.get_unlink_seqnum(seqnum);
    if (!std::get<0>(unlink_ret)) {
        if (interface.handler) {
            interface.handler->handle_isochronous_transfer(
                    session,
                    seqnum,
                    ep,
                    transfer_flags,
                    transfer_buffer_length,
                    req,
                    iso_packet_descriptors,
                    ec
                    );
        }
        else {
            SPDLOG_ERROR("端点{:04x}所在的接口没注册handler", ep.address);
            session.submit_ret_submit(
                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum));
        }
    }
    else {
        session.submit_ret_unlink_and_then_remove_seqnum_unlink(
                UsbIpResponse::UsbIpRetUnlink::create_ret_unlink_success(
                        std::get<1>(unlink_ret)
                        ),
                seqnum
                );
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
            SPDLOG_INFO("请求非标准描述符 {:08b}", type);
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
    auto handler = handle_device.interfaces[intf].handler;
    if (handler) {
        handler->request_get_interface(p_status);
    }
    else {
        SPDLOG_ERROR("接口未注册handler，无法处理");
        *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
        return;
    }
}

data_type VirtualDeviceHandler::get_device_descriptor(std::uint16_t language_id, std::uint16_t descriptor_length,
                                                      std::uint32_t *p_status
        ) {
    std::shared_lock lock(data_mutex);
    std::uint16_t version_bcd = usb_version;
    data_type desc = {
            0x12, // bLength
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
            handle_device.num_configurations
    };

    if (descriptor_length < desc.size()) {
        desc.resize(descriptor_length);
    }
    return desc;

}

data_type VirtualDeviceHandler::get_bos_descriptor(std::uint16_t language_id, std::uint16_t descriptor_length,
                                                   std::uint32_t *p_status) {
    std::shared_lock lock(data_mutex);
    data_type desc = {
            0x05, // bLength
            static_cast<std::uint8_t>(DescriptorType::BOS), // bDescriptorType: BOS
            0x05, 0x00, // wTotalLength
            0x00 // bNumCapabilities
    };
    if (descriptor_length < desc.size()) {
        desc.resize(descriptor_length);
    }
    return desc;
}

data_type VirtualDeviceHandler::get_configuration_descriptor(
        std::uint16_t language_id, std::uint16_t descriptor_length, std::uint32_t *p_status) {
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
            0x32, // bMaxPower 100mA
    };
    for (std::size_t i = 0; i < handle_device.interfaces.size(); i++) {
        auto &intf = handle_device.interfaces[i];
        data_type intf_desc = {
                0x09, // bLength
                static_cast<std::uint8_t>(DescriptorType::Interface), // bDescriptorType: Interface
                static_cast<std::uint8_t>(i), // bInterfaceNum
                0x00, // bAlternateSettings
                static_cast<std::uint8_t>(intf.endpoints.size()), // bNumEndpoints
                intf.interface_class, // bInterfaceClass
                intf.interface_subclass, // bInterfaceSubClass
                intf.interface_protocol, // bInterfaceProtocol
                intf.handler->get_string_interface_value(), //iInterface
        };
        auto class_specific_descriptor = intf.handler->get_class_specific_descriptor();
        intf_desc.insert(intf_desc.end(), class_specific_descriptor.begin(), class_specific_descriptor.end());
        for (auto &endpoint: intf.endpoints) {
            data_type ep_desc = {
                    0x07, // bLength
                    static_cast<std::uint8_t>(DescriptorType::Endpoint),
                    endpoint.address,
                    endpoint.attributes,
                    static_cast<std::uint8_t>(endpoint.max_packet_size),
                    static_cast<std::uint8_t>(endpoint.max_packet_size >> 8),
                    endpoint.interval
            };
            intf_desc.insert(intf_desc.end(), ep_desc.begin(), ep_desc.end());
        }
        desc.insert(desc.end(), intf_desc.begin(), intf_desc.end());
    }
    desc[2] = static_cast<std::uint8_t>(desc.size());
    desc[3] = static_cast<std::uint8_t>(desc.size() >> 8);
    if (descriptor_length < desc.size()) {
        desc.resize(descriptor_length);
    }
    return desc;
}

data_type VirtualDeviceHandler::get_string_descriptor(std::uint8_t language_id, std::uint16_t descriptor_length,
                                                      std::uint32_t *p_status
        ) {
    std::shared_lock lock(data_mutex);
    if (language_id == 0) {
        // language ids
        data_type desc = {
                4,
                static_cast<std::uint8_t>(DescriptorType::String),
                0x09,
                0x04
        };
        if (descriptor_length < desc.size()) {
            desc.resize(descriptor_length);
        }
        return desc;
    }
    else if (auto string_ret = string_pool.get_string(language_id)) {
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
    std::shared_lock lock(data_mutex);
    data_type desc = {
            0x0A,
            static_cast<std::uint8_t>(DescriptorType::DeviceQualifier),
            usb_version.minor,
            usb_version.major,
            handle_device.device_class,
            handle_device.device_subclass,
            handle_device.device_protocol,
            static_cast<std::uint8_t>(handle_device.ep0_in.max_packet_size),
            handle_device.num_configurations,
            0x00,
    };
    if (descriptor_length < desc.size()) {
        desc.resize(descriptor_length);
    }
    return desc;
}

data_type VirtualDeviceHandler::get_custom_descriptor(std::uint8_t type, std::uint8_t language_id,
                                                      std::uint16_t descriptor_length, std::uint32_t *p_status) {
    return {};
}
