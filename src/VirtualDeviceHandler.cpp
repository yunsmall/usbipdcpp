#include "VirtualDeviceHandler.h"

#include <variant>
#include <variant>
#include <variant>
#include <variant>

#include "InterfaceHandler.h"
#include "Session.h"
#include "protocol.h"

using namespace usbipcpp;

void VirtualDeviceHandler::dispatch_urb(Session &session, const UsbIpCommand::UsbIpCmdSubmit &cmd, std::uint32_t seqnum,
                                        const UsbEndpoint &ep, std::optional<UsbInterface> &interface,
                                        std::uint32_t transfer_flags,
                                        std::uint32_t transfer_buffer_length, const SetupPacket &setup_packet,
                                        const data_type &out_data,
                                        const std::vector<UsbIpIsoPacketDescriptor> &iso_packet_descriptors,
                                        usbipcpp::error_code &ec) {
    // if (se)
    DeviceHandlerBase::dispatch_urb(session, cmd, seqnum, ep, interface, transfer_flags, transfer_buffer_length,
                                    setup_packet, out_data,
                                    iso_packet_descriptors, ec);
}

void VirtualDeviceHandler::handle_unlink_seqnum(std::uint32_t seqnum) {
}

void VirtualDeviceHandler::stop_transfer() {
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
        RequestRecipient recipient = static_cast<RequestRecipient>(setup_packet.calc_recipient());
        //标准的请求全在这里处理了
        if (setup_packet.calc_request_type() == static_cast<std::uint8_t>(RequestType::Standard)) {
            switch (recipient) {
                case RequestRecipient::Device: {
                    StandardRequest std_request = static_cast<StandardRequest>(setup_packet.
                        calc_standard_request_type());

                    if (setup_packet.is_out()) {
                        switch (std_request) {
                            case StandardRequest::ClearFeature: {
                                request_clear_feature(setup_packet.value);
                                break;
                            }
                            case StandardRequest::SetAddress: {
                                request_set_address(setup_packet.value);
                                break;
                            }
                            case StandardRequest::SetConfiguration: {
                                request_set_configuration(setup_packet.value);
                                break;
                            }
                            case StandardRequest::SetDescriptor: {
                                request_set_descriptor(setup_packet.value >> 8, setup_packet.value & 0x00FF,
                                                       setup_packet.index,
                                                       setup_packet.length);
                                break;
                            }
                            case StandardRequest::SetFeature: {
                                request_set_feature(setup_packet.value);
                                break;
                            }
                            default: {
                                SPDLOG_WARN("Device Unhandled StandardRequest {}",
                                            static_cast<int>(std_request));
                            }
                        }
                        session.submit_ret_submit(
                                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(seqnum, {})
                                );
                    }
                    else {
                        data_type result{};
                        switch (std_request) {
                            case StandardRequest::GetConfiguration: {
                                auto ret = request_get_configuration();
                                vector_append_to_net(result, ret);
                                break;
                            }
                            case StandardRequest::GetDescriptor: {
                                result = request_get_descriptor(setup_packet.value >> 8, setup_packet.value & 0x00FF,
                                                                setup_packet.length);
                                if (setup_packet.length < result.size()) {
                                    result.resize(setup_packet.length);
                                }
                                break;
                            }
                            case StandardRequest::GetStatus: {
                                auto status = request_get_status();
                                vector_append_to_net(result, status);
                                break;
                            }
                            default: {
                                SPDLOG_WARN("Device Unhandled StandardRequest {}", static_cast<int>(std_request));
                            }
                        }
                        session.submit_ret_submit(
                                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(seqnum, result)
                                );

                    }
                    break;
                }
                case RequestRecipient::Interface: {
                    auto intf_idx = setup_packet.index;
                    auto handler = handle_device.interfaces[intf_idx].handler;
                    if (handler) {
                        StandardRequest std_request = static_cast<StandardRequest>(setup_packet.
                            calc_standard_request_type());
                        if (setup_packet.is_out()) {
                            switch (std_request) {
                                case StandardRequest::ClearFeature: {
                                    handler->request_clear_feature(setup_packet.value);
                                    break;
                                }
                                case StandardRequest::SetFeature: {
                                    handler->request_set_feature(setup_packet.value);
                                    break;
                                }
                                case StandardRequest::SetInterface: {
                                    handler->request_set_interface(setup_packet.value);
                                    break;
                                }
                                default: {
                                    SPDLOG_WARN("Interface Unhandled StandardRequest {}",
                                                static_cast<int>(std_request));
                                }
                            }
                            session.submit_ret_submit(
                                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(seqnum, {})
                                    );
                        }
                        else {
                            data_type result{};
                            switch (std_request) {
                                case StandardRequest::GetInterface: {
                                    auto ret = handler->request_get_interface();
                                    vector_append_to_net(result, ret);
                                    break;
                                }
                                case StandardRequest::GetStatus: {
                                    auto ret = handler->request_get_status();
                                    vector_append_to_net(result, ret);
                                    break;;
                                }
                                default: {
                                    SPDLOG_WARN("Interface Unhandled StandardRequest {}",
                                                static_cast<int>(std_request));
                                }
                            }
                            session.submit_ret_submit(
                                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(seqnum, result)
                                    );
                        }

                    }
                    else {
                        SPDLOG_ERROR("接口未注册handler，无法处理发够接口的信息");
                        ec = make_error_code(ErrorType::INVALID_ARG);
                    }
                    break;
                }
                case RequestRecipient::Endpoint: {
                    auto find_ret = handle_device.find_ep(setup_packet.index);
                    if (find_ret) {
                        auto &ep = find_ret->first;
                        auto &intf = find_ret->second;
                        if (intf) {
                            auto handler = intf->handler;
                            if (handler) {
                                StandardRequest std_request = static_cast<StandardRequest>(setup_packet.
                                    calc_standard_request_type());
                                if (setup_packet.is_out()) {
                                    switch (std_request) {
                                        case StandardRequest::ClearFeature: {
                                            handler->request_endpoint_clear_feature(
                                                    setup_packet.value, setup_packet.index);
                                            break;
                                        }
                                        case StandardRequest::SetFeature: {
                                            handler->request_endpoint_set_feature(
                                                    setup_packet.value, setup_packet.index);
                                            break;
                                        }
                                        default: {
                                            SPDLOG_WARN("Endpoint {:04x} Unhandled StandardRequest {}",
                                                        setup_packet.index,
                                                        static_cast<int>(std_request));
                                        }
                                    }
                                    session.submit_ret_submit(
                                            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
                                                    seqnum,
                                                    {}
                                                    )
                                            );
                                }
                                else {
                                    data_type result{};
                                    switch (std_request) {
                                        case StandardRequest::GetStatus: {
                                            auto status = handler->request_endpoint_get_status(setup_packet.index);
                                            vector_append_to_net(result, status);
                                            break;
                                        }
                                        case StandardRequest::SynchFrame: {
                                            handler->request_endpoint_sync_frame(setup_packet.index);
                                            break;
                                        }
                                        default: {
                                            SPDLOG_WARN("Endpoint {:04x} Unhandled StandardRequest {}",
                                                        setup_packet.index,
                                                        static_cast<int>(std_request));
                                        }
                                    }
                                    session.submit_ret_submit(
                                            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
                                                    seqnum,
                                                    result)
                                            );
                                }
                            }
                            else {
                                SPDLOG_ERROR("端点{:04x}所在的接口没注册对应handler", setup_packet.value);
                            }
                        }
                        else {
                            SPDLOG_ERROR("端点{:04x}没有对应的接口", setup_packet.value);
                        }
                    }
                    break;
                }
                case RequestRecipient::Other: {
                    SPDLOG_WARN("未实现去其他地方的包");
                    ec = make_error_code(ErrorType::UNIMPLEMENTED);
                    break;
                }
                default: {
                    SPDLOG_WARN("未知去往目标");
                    ec = make_error_code(ErrorType::UNIMPLEMENTED);
                }
            }
        }
        else {
            switch (recipient) {
                case RequestRecipient::Device: {
                    handle_non_standard_control_urb(session, seqnum, ep, transfer_flags, transfer_buffer_length,
                                                    setup_packet,
                                                    out_data, ec);
                    break;
                }
                case RequestRecipient::Interface: {
                    auto intf_idx = setup_packet.index;
                    auto handler = handle_device.interfaces[intf_idx].handler;
                    if (handler) {
                        handler->handle_non_standard_control_urb(session, seqnum, ep, transfer_flags,
                                                                 transfer_buffer_length,
                                                                 setup_packet,
                                                                 out_data, ec);
                    }
                    else {
                        SPDLOG_ERROR("接口未注册handler，无法处理发够接口的信息");
                        ec = make_error_code(ErrorType::INVALID_ARG);
                    }
                    break;
                }
                case RequestRecipient::Endpoint: {
                    auto intf_idx = setup_packet.index;
                    auto handler = handle_device.interfaces[intf_idx].handler;
                    if (handler) {
                        handler->handle_non_standard_control_urb_to_endpoint(session, seqnum, ep, transfer_flags,
                                                                             transfer_buffer_length,
                                                                             setup_packet,
                                                                             out_data, ec);
                    }
                    else {
                        SPDLOG_ERROR("接口未注册handler，无法处理发往接口的信息");
                        ec = make_error_code(ErrorType::INVALID_ARG);
                    }
                    break;
                }
                case RequestRecipient::Other: {
                    SPDLOG_WARN("未实现去其他地方的包");
                    ec = make_error_code(ErrorType::UNIMPLEMENTED);
                    break;
                }
                default: {
                    SPDLOG_WARN("未知去往目标");
                    ec = make_error_code(ErrorType::UNIMPLEMENTED);
                }
            }

        }
    }
    else {
        session.submit_ret_unlink(
                UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(
                        std::get<1>(unlink_ret),
                        static_cast<std::uint32_t>(StatuType::OK)
                        )
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
        }
    }
    else {
        session.submit_ret_unlink(
                UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(
                        std::get<1>(unlink_ret),
                        static_cast<std::uint32_t>(StatuType::OK)
                        )
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
        }
    }
    else {
        session.submit_ret_unlink(
                UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(
                        std::get<1>(unlink_ret),
                        static_cast<std::uint32_t>(StatuType::OK)
                        )
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
        }
    }
    else {
        session.submit_ret_unlink(
                UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(
                        std::get<1>(unlink_ret),
                        static_cast<std::uint32_t>(StatuType::OK)
                        )
                );
    }
}

std::uint8_t VirtualDeviceHandler::request_get_configuration() {
    return handle_device.configuration_value;
}

data_type VirtualDeviceHandler::request_get_descriptor(std::uint8_t type, std::uint8_t language_id,
                                                       std::uint16_t descriptor_length) {
    auto desc_type = static_cast<DescriptorType>(type);
    switch (desc_type) {
        case DescriptorType::Device: {
            return get_device_descriptor(language_id, descriptor_length);
            break;
        }
        case DescriptorType::Configuration: {
            return get_configuration_descriptor(language_id, descriptor_length);
            break;
        }
        case DescriptorType::DeviceQualifier: {
            return get_device_qualifier_descriptor(language_id, descriptor_length);
            break;
        }
        case DescriptorType::BOS: {
            return get_bos_descriptor(language_id, descriptor_length);
            break;
        }
        case DescriptorType::String: {
            return get_string_descriptor(language_id, descriptor_length);
            break;
        }
        case DescriptorType::OtherSpeedConfiguration: {
            return get_other_speed_descriptor(language_id, descriptor_length);
            break;
        }
        default: {
            SPDLOG_WARN("请求非标准描述符 {:08b}，暂未实现", type);
            return {};
        }
    }
}

std::uint8_t VirtualDeviceHandler::request_get_interface(std::uint16_t intf) {
    return handle_device.interfaces[intf].handler->request_get_interface();
}

void VirtualDeviceHandler::request_set_interface(std::uint16_t alternate_setting, std::uint16_t intf) {
    handle_device.interfaces[intf].handler->request_set_interface(alternate_setting);
}

data_type VirtualDeviceHandler::get_device_descriptor(std::uint16_t language_id, std::uint16_t descriptor_length
        ) {
    std::shared_lock lock(data_mutex);
    data_type desc = {
            0x12, // bLength
            static_cast<std::uint8_t>(DescriptorType::Device), // bDescriptorType: Device
            usb_version.minor,
            usb_version.major, // bcdUSB: USB 2.0
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

data_type VirtualDeviceHandler::get_bos_descriptor(std::uint16_t language_id, std::uint16_t descriptor_length) {
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
        std::uint16_t language_id, std::uint16_t descriptor_length) {
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
    for (auto i = 0; i < handle_device.interfaces.size(); i++) {
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

data_type VirtualDeviceHandler::get_string_descriptor(std::uint8_t language_id, std::uint16_t descriptor_length
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
        return {};
    }
}

data_type VirtualDeviceHandler::get_device_qualifier_descriptor(std::uint8_t language_id,
                                                                std::uint16_t descriptor_length) {
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
