#include "protocol.h"

#include <filesystem>

#include <asio.hpp>
#include <variant>
#include <spdlog/spdlog.h>


const usbipcpp::TransferErrorCategory g_error_category;

const char *usbipcpp::TransferErrorCategory::name() const noexcept {
    return "UsbIp Error Category";
}

std::string usbipcpp::TransferErrorCategory::message(int _Errval) const {
    auto e = static_cast<ErrorType>(_Errval);
    switch (e) {
        case ErrorType::OK: {
            return "OK";
        }
        case ErrorType::UNKNOWN_VERSION: {
            return "Unknown UsbIp Version";
        }
        case ErrorType::UNKNOWN_CMD: {
            return "Unknown Command";
        }
        case ErrorType::SOCKET_EOF: {
            return "Connection closed by peer";
        }
        case ErrorType::SOCKET_ERR: {
            return "Connection err";
        }
        case ErrorType::INTERNAL_ERROR: {
            return "Internal Error";
        }
        case ErrorType::INVALID_ARG: {
            return "Invalid Argument";
        }
        case ErrorType::UNIMPLEMENTED:{
            return "Unimplemented";
        }
    default: ;
            return "Unknown Error";
    }
}


inline std::error_code usbipcpp::make_error_code(ErrorType e) {
    return {static_cast<int>(e), g_error_category};
}

std::vector<std::uint8_t> usbipcpp::UsbIpHeaderBasic::to_bytes() const {
    std::vector<std::uint8_t> result;
    vector_append_to_net(result, command, seqnum, devid, direction, ep);
    // vector_append_to_net(result, command);
    // vector_append_to_net(result, seqnum);
    // vector_append_to_net(result, devid);
    // vector_append_to_net(result, direction);
    // vector_append_to_net(result, ep);
    return result;
}

asio::awaitable<void> usbipcpp::UsbIpHeaderBasic::from_socket(asio::ip::tcp::socket &sock) {
    co_await read_from_socket(sock, seqnum, devid, direction, ep);
}

std::vector<std::uint8_t> usbipcpp::UsbIpIsoPacketDescriptor::to_bytes() const {
    // std::vector<std::uint8_t> result;
    // result.reserve(sizeof(UsbIpIsoPacketDescriptor));
    // vector_append_to_net(result, offset);
    // vector_append_to_net(result, length);
    // vector_append_to_net(result, actual_length);
    // vector_append_to_net(result, status);
    return to_data(offset, length, actual_length, status);
    // return result;
}

asio::awaitable<void> usbipcpp::UsbIpIsoPacketDescriptor::from_socket(asio::ip::tcp::socket &sock) {
    co_await read_from_socket(sock, offset, length, actual_length, status);
}

std::vector<std::uint8_t> usbipcpp::UsbIpResponse::OpRepDevlist::to_bytes() const {
    std::vector<std::uint8_t> result = to_data(USBIP_VERSION, OP_REP_DEVLIST, status, device_count);
    // vector_append_to_net(result, USBIP_VERSION);
    // vector_append_to_net(result, OP_REP_DEVLIST);
    // vector_append_to_net(result, status);
    // vector_append_to_net(result, device_count);
    for (auto &device: devices) {
        auto bytes = device.to_bytes_with_interfaces();
        result.insert(result.end(), bytes.begin(), bytes.end());
    }
    return result;
}

asio::awaitable<void> usbipcpp::UsbIpResponse::OpRepDevlist::from_socket(asio::ip::tcp::socket &sock) {
    // status = read_u32(sock);
    // device_count = read_u32(sock);
    // devices = read_u32(sock);
    co_return;
}


usbipcpp::UsbIpResponse::OpRepDevlist usbipcpp::UsbIpResponse::OpRepDevlist::
create_from_devices(const std::vector<std::shared_ptr<UsbDevice>> &devices) {
    std::vector<UsbDevice> ret_devices;
    ret_devices.reserve(devices.size());
    for (auto &device: devices) {
        ret_devices.emplace_back(*device);
    }
    return {
            .status = 0,
            .device_count = static_cast<uint32_t>(ret_devices.size()),
            .devices = std::move(ret_devices)
    };
}

std::vector<std::uint8_t> usbipcpp::UsbIpResponse::OpRepImport::to_bytes() const {
    std::vector<std::uint8_t> result = to_data(USBIP_VERSION, OP_REP_IMPORT, status);
    // vector_append_to_net(result, USBIP_VERSION);
    // vector_append_to_net(result, OP_REP_IMPORT);
    // vector_append_to_net(result, status);
    if (status == 0) {
        if (device) {
            auto bytes = device->to_bytes();
            result.insert(result.end(), bytes.begin(), bytes.end());
        }
    }
    return result;
}

asio::awaitable<void> usbipcpp::UsbIpResponse::OpRepImport::from_socket(asio::ip::tcp::socket &sock) {
    co_return;
}

usbipcpp::UsbIpResponse::OpRepImport usbipcpp::UsbIpResponse::OpRepImport::create_on_failure() {
    return {
            .status = static_cast<std::uint32_t>(StatuType::NA),
    };
}

usbipcpp::UsbIpResponse::OpRepImport usbipcpp::UsbIpResponse::OpRepImport::create_on_success(
        std::shared_ptr<UsbDevice> device) {
    return {
            .status = static_cast<std::uint32_t>(StatuType::OK),
            .device = std::move(device)
    };
}


std::vector<std::uint8_t> usbipcpp::UsbIpResponse::UsbIpRetSubmit::to_bytes() const {
    assert(header.command==USBIP_RET_SUBMIT);
    //检查个软，服务端方向恒为0，导致这里直接报错
    // SPDLOG_TRACE(
    //         "UsbIpResponse::UsbIpRetSubmit::to_bytes() header.direction==UsbIpDirection::In {},actual_length {},transfer_buffer.size() {}",
    //         header.direction==UsbIpDirection::In, actual_length, transfer_buffer.size());
    // assert(header.direction==UsbIpDirection::In?(actual_length==transfer_buffer.size()):(actual_length==0));
    assert(actual_length==transfer_buffer.size());
    auto result = header.to_bytes();
    auto result2 = to_data(status, actual_length, start_frame, number_of_packets, error_count);
    result.insert(result.end(), result2.begin(), result2.end());
    // vector_append_to_net(result, status);
    // vector_append_to_net(result, actual_length);
    // vector_append_to_net(result, start_frame);
    // vector_append_to_net(result, number_of_packets);
    // vector_append_to_net(result, error_count);
    result.resize(result.size() + 8, 0);
    // if (header.direction == UsbIpDirection::In) {
    //     result.insert(result.end(), transfer_buffer.begin(), transfer_buffer.end());
    // }
    if (!transfer_buffer.empty()) {
        result.insert(result.end(), transfer_buffer.begin(), transfer_buffer.end());
    }
    for (auto &iso_desc: iso_packet_descriptor) {
        auto iso_byte = iso_desc.to_bytes();
        result.insert(result.end(), iso_byte.begin(), iso_byte.end());
    }

    return result;
}

asio::awaitable<void> usbipcpp::UsbIpResponse::UsbIpRetSubmit::from_socket(asio::ip::tcp::socket &sock) {
    co_return;
}

usbipcpp::UsbIpResponse::UsbIpRetSubmit usbipcpp::UsbIpResponse::UsbIpRetSubmit::usbip_ret_submit_fail(
        std::uint32_t seqnum) {
    //具体怎么填查看usbip C语言源码
    return UsbIpRetSubmit{
            .header = UsbIpHeaderBasic::get_server_header(
                    USBIP_RET_SUBMIT,
                    seqnum
                    ),
            .status = static_cast<std::uint32_t>(StatuType::NA),
            .actual_length = 0,
            .start_frame = 0,
            .number_of_packets = 0,
            .error_count = 0,
            .transfer_buffer = {},
            .iso_packet_descriptor = {}
    };
}

usbipcpp::UsbIpResponse::UsbIpRetSubmit usbipcpp::UsbIpResponse::UsbIpRetSubmit::usbip_ret_submit_fail_with_status(
        std::uint32_t seqnum, std::uint32_t status) {
    auto ret = usbip_ret_submit_fail(seqnum);
    ret.status = status;
    return ret;
}

usbipcpp::UsbIpResponse::UsbIpRetSubmit usbipcpp::UsbIpResponse::UsbIpRetSubmit::create_ret_submit(
        std::uint32_t seqnum, std::uint32_t status, std::uint32_t start_frame,
        std::uint32_t number_of_packets, const std::vector<std::uint8_t> &transfer_buffer,
        const std::vector<UsbIpIsoPacketDescriptor> &
        iso_packet_descriptor) {
    auto ret = UsbIpRetSubmit{
            .header = UsbIpHeaderBasic::get_server_header(
                    USBIP_RET_SUBMIT,
                    seqnum
                    ),
            .status = status,
            .actual_length = static_cast<std::uint32_t>(transfer_buffer.size()),
            .start_frame = start_frame,
            .number_of_packets = number_of_packets,
            .error_count = 0,
            .transfer_buffer = transfer_buffer,
            .iso_packet_descriptor = iso_packet_descriptor
    };
    return ret;
}

usbipcpp::UsbIpResponse::UsbIpRetSubmit usbipcpp::UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(
        std::uint32_t seqnum) {
    auto ret = UsbIpRetSubmit{
            .header = UsbIpHeaderBasic::get_server_header(
                    USBIP_RET_SUBMIT,
                    seqnum
                    ),
            .status = static_cast<std::uint32_t>(StatuType::OK),
            .actual_length = 0,
            .start_frame = 0,
            .number_of_packets = 0,
            .error_count = 0,
            .transfer_buffer = {},
            .iso_packet_descriptor = {}
    };
    return ret;
}

usbipcpp::UsbIpResponse::UsbIpRetSubmit usbipcpp::UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
        std::uint32_t seqnum, const data_type &transfer_buffer) {
    auto ret = UsbIpRetSubmit{
            .header = UsbIpHeaderBasic::get_server_header(
                    USBIP_RET_SUBMIT,
                    seqnum
                    ),
            .status = static_cast<std::uint32_t>(StatuType::OK),
            .actual_length = static_cast<std::uint32_t>(transfer_buffer.size()),
            .start_frame = 0,
            .number_of_packets = 0,
            .error_count = 0,
            .transfer_buffer = transfer_buffer,
            .iso_packet_descriptor = {}
    };
    return ret;
}

std::vector<std::uint8_t> usbipcpp::UsbIpResponse::UsbIpRetUnlink::to_bytes() const {
    assert(header.command==USBIP_RET_UNLINK);
    auto result = header.to_bytes();
    vector_append_to_net(result, status);
    result.resize(result.size() + 24, 0);
    return result;
}

asio::awaitable<void> usbipcpp::UsbIpResponse::UsbIpRetUnlink::from_socket(asio::ip::tcp::socket &sock) {
    co_return;
}

usbipcpp::UsbIpResponse::UsbIpRetUnlink usbipcpp::UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(
        std::uint32_t seqnum, std::uint32_t status) {
    return {
            .header = UsbIpHeaderBasic::get_server_header(USBIP_RET_UNLINK, seqnum),
            .status = status
    };
}

usbipcpp::UsbIpResponse::UsbIpRetUnlink usbipcpp::UsbIpResponse::UsbIpRetUnlink::usbip_ret_unlink_success(
        const UsbIpHeaderBasic &header) {
    return {
            .header = header,
            .status = static_cast<std::uint32_t>(StatuType::OK)
    };
}

usbipcpp::UsbIpResponse::UsbIpRetUnlink usbipcpp::UsbIpResponse::UsbIpRetUnlink::usbip_ret_unlink_failed_with_status(
        const UsbIpHeaderBasic &header, std::uint32_t status) {
    return {
            .header = header,
            .status = status
    };
}

std::vector<std::uint8_t> usbipcpp::UsbIpCommand::OpReqDevlist::to_bytes() const {
    std::vector<std::uint8_t> result = to_data(USBIP_VERSION, OP_REQ_DEVLIST, status);
    // vector_append_to_net(result, USBIP_VERSION);
    // vector_append_to_net(result, OP_REQ_DEVLIST);
    // vector_append_to_net(result, status);
    return result;
}

asio::awaitable<void> usbipcpp::UsbIpCommand::OpReqDevlist::from_socket(asio::ip::tcp::socket &sock) {
    status = co_await read_u32(sock);
    assert(status == 0);
}

std::vector<std::uint8_t> usbipcpp::UsbIpCommand::OpReqImport::to_bytes() const {
    std::vector<std::uint8_t> result = to_data(USBIP_VERSION, OP_REQ_DEVLIST, status);
    // vector_append_to_net(result, USBIP_VERSION);
    // vector_append_to_net(result, OP_REQ_DEVLIST);
    // vector_append_to_net(result, status);
    result.insert(result.end(), busid.begin(), busid.end());
    return result;
}

asio::awaitable<void> usbipcpp::UsbIpCommand::OpReqImport::from_socket(asio::ip::tcp::socket &sock) {
    status = co_await read_u32(sock);
    assert(status == 0);
    co_await asio::async_read(sock, asio::buffer(busid), asio::use_awaitable);
}

std::vector<std::uint8_t> usbipcpp::UsbIpCommand::UsbIpCmdSubmit::to_bytes() const {
    assert(header.direction!=UsbIpDirection::Out||transfer_buffer_length==data.size());
    auto result = header.to_bytes();
    auto result2 = to_data(transfer_flags, transfer_buffer_length, start_frame, number_of_packets, interval);
    result.insert(result.end(), result2.begin(), result2.end());
    // vector_append_to_net(result, transfer_flags);
    // vector_append_to_net(result, transfer_buffer_length);
    // vector_append_to_net(result, start_frame);
    // vector_append_to_net(result, number_of_packets);
    // vector_append_to_net(result, interval);
    result.insert(result.end(), setup.begin(), setup.end());
    result.insert(result.end(), data.begin(), data.end());
    for (auto &iso_des: iso_packet_descriptor) {
        auto iso_byte = iso_des.to_bytes();
        result.insert(result.end(), iso_byte.begin(), iso_byte.end());
    }
    return result;
}

asio::awaitable<void> usbipcpp::UsbIpCommand::UsbIpCmdSubmit::from_socket(asio::ip::tcp::socket &sock) {
    co_await header.from_socket(sock);
    //设置命令类型
    header.command = USBIP_CMD_SUBMIT;

    co_await read_from_socket(sock, transfer_flags, transfer_buffer_length, start_frame, number_of_packets, interval);
    co_await asio::async_read(sock, asio::buffer(setup), asio::use_awaitable);

    if (header.direction == UsbIpDirection::In) {
        data.clear();
    }
    else {
        data.resize(transfer_buffer_length);
        co_await asio::async_read(sock, asio::buffer(data), asio::use_awaitable);
    }

    if (number_of_packets != 0 && number_of_packets != 0xFFFFFFFF) {
        iso_packet_descriptor.resize(number_of_packets);
        for (auto &iso_packet: iso_packet_descriptor) {
            co_await iso_packet.from_socket(sock);
        }
    }
    else {
        iso_packet_descriptor.clear();
    }
}

std::vector<std::uint8_t> usbipcpp::UsbIpCommand::UsbIpCmdUnlink::to_bytes() const {
    auto result = header.to_bytes();
    vector_append_to_net(result, unlink_seqnum);
    result.resize(result.size() + 24, 0);
    return result;
}

asio::awaitable<void> usbipcpp::UsbIpCommand::UsbIpCmdUnlink::from_socket(asio::ip::tcp::socket &sock) {
    co_await header.from_socket(sock);
    //设置命令类型
    header.command = USBIP_CMD_UNLINK;

    unlink_seqnum = co_await read_u32(sock);

    std::array<uint8_t, 24> padding{};
    co_await asio::async_read(sock, asio::buffer(padding), asio::use_awaitable);
}


asio::awaitable<usbipcpp::UsbIpCommand::CmdVariant> usbipcpp::UsbIpCommand::get_cmd_from_socket(
        asio::ip::tcp::socket &sock, usbipcpp::error_code &ec) {
    try {
        auto version = co_await read_u16(sock);
        if (version != 0 && version != USBIP_VERSION) {
            ec = make_error_code(ErrorType::UNKNOWN_VERSION);
            co_return UsbIpCommand::CmdVariant{};
        }
        auto op_command = co_await read_u16(sock);
        SPDLOG_DEBUG("收到command: 0x{:04x}", op_command);

        switch (op_command) {
            case OP_REQ_DEVLIST: {
                auto req = OpReqDevlist{};
                co_await req.from_socket(sock);
                co_return req;
                break;
            }
            case OP_REQ_IMPORT: {
                auto req = OpReqImport{};
                co_await req.from_socket(sock);
                co_return req;
                break;
            }
            case USBIP_CMD_SUBMIT: {
                auto cmd = UsbIpCmdSubmit{};
                co_await cmd.from_socket(sock);
                co_return cmd;
                break;
            }
            case USBIP_CMD_UNLINK: {
                auto cmd = UsbIpCmdUnlink{};
                co_await cmd.from_socket(sock);
                co_return cmd;
                break;
            }
            default: {
                ec = make_error_code(ErrorType::UNKNOWN_CMD);
                co_return UsbIpCommand::CmdVariant{};
            }
        }
    } catch (const asio::system_error &e) {
        SPDLOG_DEBUG("asio错误：{}", e.what());
        if (e.code() == asio::error::eof) {
            ec = make_error_code(ErrorType::SOCKET_EOF);
        }
        else {
            ec = make_error_code(ErrorType::SOCKET_ERR);
        }
    }
    co_return UsbIpCommand::CmdVariant{};
}

std::vector<std::uint8_t> usbipcpp::UsbIpCommand::to_bytes(const CmdVariant &cmd) {
    return std::visit([](auto &&package) {
        return package.to_bytes();
    }, cmd);
}
