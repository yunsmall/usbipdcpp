#include "device.h"

#include <ranges>

#include "DeviceHandler/DeviceHandler.h"

using namespace usbipdcpp;

std::vector<std::uint8_t> usbipdcpp::UsbDevice::to_bytes_with_interfaces() const {
    auto result = to_network_data(to_bytes_without_interfaces());
    for (auto &interface: interfaces) {
        auto bytes = interface.to_bytes();
        result.insert(result.end(), bytes.begin(), bytes.end());
    }
    return result;
}

array_data_type<UsbDevice::bytes_without_interfaces_num> usbipdcpp::UsbDevice::to_bytes_without_interfaces() const {
    array_data_type<256> path_buffer = {0};
    auto path_str = this->path.string();
    std::memcpy(
            path_buffer.data(),
            path_str.c_str(),
            std::min(path_str.size(), std::size(path_buffer) - 1)
            );

    array_data_type<32> busid_buffer = {0};;
    std::memcpy(
            busid_buffer.data(),
            this->busid.c_str(),
            std::min(this->busid.size(), std::size(busid_buffer) - 1)
            );

    return to_network_array(
            path_buffer,
            busid_buffer,
            bus_num,
            dev_num,
            speed,
            vendor_id,
            product_id,
            static_cast<std::uint16_t>(device_bcd),
            device_class,
            device_subclass,
            device_protocol,
            configuration_value,
            num_configurations,
            static_cast<std::uint8_t>(interfaces.size())
            );
}

array_data_type<UsbDevice::bytes_without_interfaces_num> usbipdcpp::UsbDevice::to_bytes() const {
    return to_bytes_without_interfaces();
}

asio::awaitable<void> usbipdcpp::UsbDevice::from_socket_co(asio::ip::tcp::socket &sock) {
    co_return;
}

void UsbDevice::from_socket(asio::ip::tcp::socket &sock) {
    return;
}

std::optional<std::pair<usbipdcpp::UsbEndpoint, std::optional<usbipdcpp::UsbInterface>>> usbipdcpp::UsbDevice::
find_ep(std::uint8_t ep) {
    if (ep == ep0_in.address) {
        // SPDLOG_INFO("找到端口0{}", ep);
        return std::make_pair(ep0_in, std::nullopt);
    }
    else if (ep == ep0_out.address) {
        return std::make_pair(ep0_out, std::nullopt);
    }
    else {
        for (auto &intf: interfaces) {
            for (auto &endpoint: intf.endpoints) {
                if (endpoint.address == ep) {
                    return std::make_pair(endpoint, std::make_optional(intf));
                }
            }
        }
    }
    return std::nullopt;
}

void usbipdcpp::UsbDevice::handle_urb(
        const UsbIpCommand::UsbIpCmdSubmit &cmd,
        std::uint32_t seqnum,
        const UsbEndpoint &ep,
        std::optional<UsbInterface> &interface,
        std::uint32_t transfer_buffer_length, const SetupPacket &setup_packet,
        const std::vector<std::uint8_t> &out_data,
        const std::vector<UsbIpIsoPacketDescriptor> &iso_packet_descriptors,
        std::error_code &ec) {
    SPDLOG_TRACE("设备处理URB，将其转发到对应handler中");
    if (handler) {
        handler->dispatch_urb(cmd, seqnum, ep, interface, transfer_buffer_length, transfer_buffer_length,
                              setup_packet, out_data, iso_packet_descriptors, ec);
    }
    else {
        SPDLOG_ERROR("设备没注册handler");
    }
}

void UsbDevice::on_new_connection(Session &session, error_code &ec) {
    SPDLOG_TRACE("设备处理 on_new_connection，将其转发到对应handler中");
    if (handler) {
        handler->on_new_connection(session, ec);
    }
    else {
        SPDLOG_ERROR("设备没注册handler");
    }
}

void usbipdcpp::UsbDevice::on_disconnection(error_code &ec) {
    SPDLOG_TRACE("设备处理 on_disconnection，将其转发到对应handler中");
    if (handler) {
        handler->on_disconnection(ec);
    }
    else {
        SPDLOG_ERROR("设备没注册handler");
    }
}

void usbipdcpp::UsbDevice::handle_unlink_seqnum(std::uint32_t seqnum) {
    SPDLOG_TRACE("设备处理unlink，将其转发到对应handler中");
    if (handler) {
        handler->handle_unlink_seqnum(seqnum);
    }
    else {
        SPDLOG_ERROR("设备没注册handler");
    }
}
