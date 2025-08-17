#include "protocol.h"

#include <filesystem>

#include <asio.hpp>
#include <variant>
#include <spdlog/spdlog.h>


using namespace usbipdcpp;

const usbipdcpp::TransferErrorCategory g_error_category;

const char *usbipdcpp::TransferErrorCategory::name() const noexcept {
    return "UsbIp Error Category";
}

std::string usbipdcpp::TransferErrorCategory::message(int _Errval) const {
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
        case ErrorType::PROTOCOL_ERROR: {
            return "Protocol Error";
        }
        case ErrorType::NO_DEVICE: {
            return "No Device";
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
        case ErrorType::UNIMPLEMENTED: {
            return "Unimplemented";
        }
        case ErrorType::TRANSFER_ERROR: {
            return "Transfer Error";
        }
        default: ;
            return "Unknown Error";
    }
}


std::error_code usbipdcpp::make_error_code(ErrorType e) {
    return {static_cast<int>(e), g_error_category};
}

array_data_type<usbipdcpp::calculate_total_size_with_array<
    decltype(UsbIpHeaderBasic::command),
    decltype(UsbIpHeaderBasic::seqnum),
    decltype(UsbIpHeaderBasic::devid),
    decltype(UsbIpHeaderBasic::direction),
    decltype(UsbIpHeaderBasic::ep)
>()> UsbIpHeaderBasic::to_bytes() const {
    return to_network_array(command, seqnum, devid, direction, ep);
}

asio::awaitable<void> usbipdcpp::UsbIpHeaderBasic::from_socket(asio::ip::tcp::socket &sock) {
    co_await unsigned_integral_read_from_socket(sock, seqnum, devid, direction, ep);
}

array_data_type<calculate_total_size_with_array<
    decltype(UsbIpIsoPacketDescriptor::offset),
    decltype(UsbIpIsoPacketDescriptor::length),
    decltype(UsbIpIsoPacketDescriptor::actual_length),
    decltype(UsbIpIsoPacketDescriptor::status)
>()> usbipdcpp::UsbIpIsoPacketDescriptor::to_bytes() const {
    return to_network_array(offset, length, actual_length, status);
}

asio::awaitable<void> usbipdcpp::UsbIpIsoPacketDescriptor::from_socket(asio::ip::tcp::socket &sock) {
    co_await unsigned_integral_read_from_socket(sock, offset, length, actual_length, status);
}

std::vector<std::uint8_t> usbipdcpp::UsbIpResponse::OpRepDevlist::to_bytes() const {
    std::vector<std::uint8_t> result = to_network_data(USBIP_VERSION, OP_REP_DEVLIST, status, device_count);
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

asio::awaitable<void> usbipdcpp::UsbIpResponse::OpRepDevlist::from_socket(asio::ip::tcp::socket &sock) {
    // status = read_u32(sock);
    // device_count = read_u32(sock);
    // devices = read_u32(sock);
    co_return;
}


usbipdcpp::UsbIpResponse::OpRepDevlist usbipdcpp::UsbIpResponse::OpRepDevlist::
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

std::vector<std::uint8_t> usbipdcpp::UsbIpResponse::OpRepImport::to_bytes() const {
    std::vector<std::uint8_t> result = to_network_data(USBIP_VERSION, OP_REP_IMPORT, status);
    if (status == 0) {
        if (device) {
            vector_append_to_net(result, device->to_bytes());
        }
    }
    return result;
}

asio::awaitable<void> usbipdcpp::UsbIpResponse::OpRepImport::from_socket(asio::ip::tcp::socket &sock) {
    co_return;
}

usbipdcpp::UsbIpResponse::OpRepImport usbipdcpp::UsbIpResponse::OpRepImport::create_on_failure_with_status(
        std::uint32_t status) {
    return {
            .status = status,
            .device = {}
    };
}

usbipdcpp::UsbIpResponse::OpRepImport usbipdcpp::UsbIpResponse::OpRepImport::create_on_failure() {
    return create_on_failure_with_status(static_cast<std::uint32_t>(OperationStatuType::NA));
}

usbipdcpp::UsbIpResponse::OpRepImport usbipdcpp::UsbIpResponse::OpRepImport::create_on_success(
        std::shared_ptr<UsbDevice> device) {
    return {
            .status = static_cast<std::uint32_t>(OperationStatuType::OK),
            .device = std::move(device)
    };
}


data_type usbipdcpp::UsbIpResponse::UsbIpRetSubmit::to_bytes() const {
    assert(header.command==USBIP_RET_SUBMIT);
    //检查个软，服务端方向恒为0，导致这里直接报错
    // SPDLOG_TRACE(
    //         "UsbIpResponse::UsbIpRetSubmit::to_bytes() header.direction==UsbIpDirection::In {},actual_length {},transfer_buffer.size() {}",
    //         header.direction==UsbIpDirection::In, actual_length, transfer_buffer.size());
    // assert(header.direction==UsbIpDirection::In?(actual_length==transfer_buffer.size()):(actual_length==0));
    assert(actual_length==transfer_buffer.size());

    auto result1 = header.to_bytes();
    auto result2 = to_network_array(status, actual_length, start_frame, number_of_packets, error_count);
    auto total_result = to_network_data(result1, result2);

    //padding
    total_result.resize(total_result.size() + 8, 0);
    // if (header.direction == UsbIpDirection::In) {
    //     result.insert(result.end(), transfer_buffer.begin(), transfer_buffer.end());
    // }
    if (!transfer_buffer.empty()) {
        vector_append_to_net(total_result, transfer_buffer);
    }
    for (auto &iso_desc: iso_packet_descriptor) {
        auto iso_byte = iso_desc.to_bytes();
        vector_append_to_net(total_result, iso_byte);
    }

    return total_result;
}

asio::awaitable<void> usbipdcpp::UsbIpResponse::UsbIpRetSubmit::from_socket(asio::ip::tcp::socket &sock) {
    co_return;
}

usbipdcpp::UsbIpResponse::UsbIpRetSubmit usbipdcpp::UsbIpResponse::UsbIpRetSubmit::usbip_ret_submit_fail_with_status(
        std::uint32_t seqnum, std::uint32_t status) {
    return UsbIpRetSubmit{
            .header = UsbIpHeaderBasic::get_server_header(
                    USBIP_RET_SUBMIT,
                    seqnum
                    ),
            .status = status,
            .actual_length = 0,
            .start_frame = 0,
            .number_of_packets = 0,
            .error_count = 0,
            .transfer_buffer = {},
            .iso_packet_descriptor = {}
    };
}

usbipdcpp::UsbIpResponse::UsbIpRetSubmit usbipdcpp::UsbIpResponse::UsbIpRetSubmit::create_ret_submit(
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

usbipdcpp::UsbIpResponse::UsbIpRetSubmit usbipdcpp::UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(
        std::uint32_t seqnum) {
    auto ret = UsbIpRetSubmit{
            .header = UsbIpHeaderBasic::get_server_header(
                    USBIP_RET_SUBMIT,
                    seqnum
                    ),
            .status = static_cast<std::uint32_t>(UrbStatusType::StatusOK),
            .actual_length = 0,
            .start_frame = 0,
            .number_of_packets = 0,
            .error_count = 0,
            .transfer_buffer = {},
            .iso_packet_descriptor = {}
    };
    return ret;
}

usbipdcpp::UsbIpResponse::UsbIpRetSubmit usbipdcpp::UsbIpResponse::UsbIpRetSubmit::
create_ret_submit_with_status_and_no_data(std::uint32_t seqnum, std::uint32_t status) {
    auto ret = UsbIpRetSubmit{
            .header = UsbIpHeaderBasic::get_server_header(
                    USBIP_RET_SUBMIT,
                    seqnum
                    ),
            .status = status,
            .actual_length = 0,
            .start_frame = 0,
            .number_of_packets = 0,
            .error_count = 0,
            .transfer_buffer = {},
            .iso_packet_descriptor = {}
    };
    return ret;
}

usbipdcpp::UsbIpResponse::UsbIpRetSubmit usbipdcpp::UsbIpResponse::UsbIpRetSubmit::
create_ret_submit_with_status_and_no_iso(std::uint32_t seqnum, std::uint32_t status, const data_type &transfer_buffer) {
    auto ret = UsbIpRetSubmit{
            .header = UsbIpHeaderBasic::get_server_header(
                    USBIP_RET_SUBMIT,
                    seqnum
                    ),
            .status = status,
            .actual_length = static_cast<std::uint32_t>(transfer_buffer.size()),
            .start_frame = 0,
            .number_of_packets = 0,
            .error_count = 0,
            .transfer_buffer = transfer_buffer,
            .iso_packet_descriptor = {}
    };
    return ret;
}

usbipdcpp::UsbIpResponse::UsbIpRetSubmit usbipdcpp::UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_no_iso(
        std::uint32_t seqnum,
        const data_type &transfer_buffer) {
    return create_ret_submit_with_status_and_no_iso(seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE),
                                                    transfer_buffer);
}

usbipdcpp::UsbIpResponse::UsbIpRetSubmit usbipdcpp::UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(
        std::uint32_t seqnum) {
    auto ret = UsbIpRetSubmit{
            .header = UsbIpHeaderBasic::get_server_header(
                    USBIP_RET_SUBMIT,
                    seqnum
                    ),
            .status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE),
            .actual_length = 0,
            .start_frame = 0,
            .number_of_packets = 0,
            .error_count = 0,
            .transfer_buffer = {},
            .iso_packet_descriptor = {}
    };
    return ret;
}

usbipdcpp::UsbIpResponse::UsbIpRetSubmit usbipdcpp::UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
        std::uint32_t seqnum, const data_type &transfer_buffer) {
    return create_ret_submit_with_status_and_no_iso(seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK),
                                                    transfer_buffer);
}

array_data_type<
    calculate_total_size_with_array<
        decltype(UsbIpHeaderBasic{}.to_bytes()), decltype(UsbIpResponse::UsbIpRetUnlink::status)
    >() + 24
> UsbIpResponse::UsbIpRetUnlink::to_bytes() const {
    assert(header.command==USBIP_RET_UNLINK);
    auto result = to_network_array(header.to_bytes(), status);
    return array_add_padding<24>(result);
}

asio::awaitable<void> usbipdcpp::UsbIpResponse::UsbIpRetUnlink::from_socket(asio::ip::tcp::socket &sock) {
    co_return;
}

usbipdcpp::UsbIpResponse::UsbIpRetUnlink usbipdcpp::UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(
        std::uint32_t seqnum, std::uint32_t status) {
    return {
            .header = UsbIpHeaderBasic::get_server_header(USBIP_RET_UNLINK, seqnum),
            .status = status
    };
}

usbipdcpp::UsbIpResponse::UsbIpRetUnlink usbipdcpp::UsbIpResponse::UsbIpRetUnlink::create_ret_unlink_success(
        std::uint32_t seqnum) {
    return {
            .header = UsbIpHeaderBasic::get_server_header(USBIP_RET_UNLINK, seqnum),
            .status = static_cast<std::uint32_t>(UrbStatusType::StatusECONNRESET)
    };
}

array_data_type<calculate_total_size_with_array<
    decltype(USBIP_VERSION), decltype(OP_REQ_DEVLIST), decltype(UsbIpCommand::OpReqDevlist::status)
>()> usbipdcpp::UsbIpCommand::OpReqDevlist::to_bytes() const {
    return to_network_array(USBIP_VERSION, OP_REQ_DEVLIST, status);
}

asio::awaitable<void> usbipdcpp::UsbIpCommand::OpReqDevlist::from_socket(asio::ip::tcp::socket &sock) {
    status = co_await read_u32(sock);
    assert(status == 0);
}

array_data_type<
    calculate_total_size_with_array<
        decltype(USBIP_VERSION),
        decltype(OP_REQ_DEVLIST),
        decltype(UsbIpCommand::OpReqImport::status),
        decltype(UsbIpCommand::OpReqImport::busid)
    >()
> usbipdcpp::UsbIpCommand::OpReqImport::to_bytes() const {
    return to_network_array(USBIP_VERSION, OP_REQ_DEVLIST, status, busid);
}

asio::awaitable<void> usbipdcpp::UsbIpCommand::OpReqImport::from_socket(asio::ip::tcp::socket &sock) {
    co_await data_read_from_socket(sock, status, busid);
    assert(status == 0);
}

std::vector<std::uint8_t> usbipdcpp::UsbIpCommand::UsbIpCmdSubmit::to_bytes() const {
    assert(header.direction!=UsbIpDirection::Out||transfer_buffer_length==data.size());
    auto total_result =
            to_network_data(
                    to_network_array(
                            header.to_bytes(),
                            transfer_flags,
                            transfer_buffer_length,
                            start_frame,
                            number_of_packets,
                            interval,
                            setup.to_bytes()
                            ),
                    data
                    );
    for (auto &iso_des: iso_packet_descriptor) {
        auto iso_byte = iso_des.to_bytes();
        vector_append_to_net(total_result, iso_byte);
    }
    return total_result;
}

asio::awaitable<void> usbipdcpp::UsbIpCommand::UsbIpCmdSubmit::from_socket(asio::ip::tcp::socket &sock) {
    co_await header.from_socket(sock);
    //设置命令类型
    header.command = USBIP_CMD_SUBMIT;

    co_await unsigned_integral_read_from_socket(sock, transfer_flags, transfer_buffer_length, start_frame,
                                                number_of_packets,
                                                interval);
    co_await setup.from_socket(sock);

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

bool usbipdcpp::UsbIpCommand::UsbIpCmdSubmit::operator==(const UsbIpCmdSubmit &other) const {
    bool non_data_equal = header == other.header &&
                          transfer_flags == other.transfer_flags &&
                          start_frame == other.start_frame &&
                          number_of_packets == other.number_of_packets &&
                          interval == other.interval &&
                          setup == other.setup;
    //非数据部分是否相等
    if (!non_data_equal) {
        return false;
    }

    //重新判断iso长度是否相等
    if (iso_packet_descriptor.size() != other.iso_packet_descriptor.size()) {
        return false;
    }
    //如果iso包长度为空，代表为非iso数据，则直接判断数据是否相等
    if (iso_packet_descriptor.empty()) {
        return transfer_buffer_length == other.transfer_buffer_length &&
               data == other.data;
    }
    //iso描述符不为空则为iso包，则要判断每一个iso包描述符中的数据是否相等，允许data字段不相等
    for (std::size_t i = 0; i < iso_packet_descriptor.size(); i++) {
        //包描述符不相等则肯定不相等
        if (iso_packet_descriptor[i] != other.iso_packet_descriptor[i]) {
            return false;
        }
        //判断每个包描述的数据是否相等
        if (std::memcmp(data.data() + iso_packet_descriptor[i].offset,
                        other.data.data() + other.iso_packet_descriptor[i].offset,
                        iso_packet_descriptor[i].actual_length) != 0) {
            return false;
        }
    }
    return true;
}

array_data_type<
    calculate_total_size_with_array<
        decltype(UsbIpHeaderBasic{}.to_bytes()),
        decltype(UsbIpCommand::UsbIpCmdUnlink::unlink_seqnum)
    >() + 24
> usbipdcpp::UsbIpCommand::UsbIpCmdUnlink::to_bytes() const {
    return array_add_padding<24>(
            to_network_array(
                    header.to_bytes(),
                    unlink_seqnum
                    )
            );
}

asio::awaitable<void> usbipdcpp::UsbIpCommand::UsbIpCmdUnlink::from_socket(asio::ip::tcp::socket &sock) {
    co_await header.from_socket(sock);
    //设置命令类型
    header.command = USBIP_CMD_UNLINK;

    unlink_seqnum = co_await read_u32(sock);

    std::array<uint8_t, 24> padding{};
    co_await asio::async_read(sock, asio::buffer(padding), asio::use_awaitable);
}

asio::awaitable<usbipdcpp::UsbIpCommand::OpVariant> usbipdcpp::UsbIpCommand::get_op_from_socket(
        asio::ip::tcp::socket &sock, usbipdcpp::error_code &ec) {
    try {
        auto version = co_await read_u16(sock);
        if (version != 0 && version != USBIP_VERSION) {
            ec = make_error_code(ErrorType::UNKNOWN_VERSION);
            co_return OpVariant{};
        }
        auto op = co_await read_u16(sock);
        SPDLOG_DEBUG("收到op: 0x{:04x}", op);

        switch (op) {
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
            default: {
                ec = make_error_code(ErrorType::UNKNOWN_CMD);
                co_return UsbIpCommand::OpVariant{};
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
    co_return UsbIpCommand::OpVariant{};
}

asio::awaitable<usbipdcpp::UsbIpCommand::CmdVariant> usbipdcpp::UsbIpCommand::get_cmd_from_socket(
        asio::ip::tcp::socket &sock, usbipdcpp::error_code &ec) {
    try {
        auto command = co_await read_u32(sock);
        SPDLOG_DEBUG("收到command: 0x{:04x}", command);

        switch (command) {
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

std::vector<std::uint8_t> usbipdcpp::UsbIpCommand::to_bytes(const AllCmdVariant &cmd) {
    return std::visit([](auto &&package) {
        auto bytes = package.to_bytes();
        using Type = decltype(bytes);
        if constexpr (std::is_same_v<Type, data_type>) {
            return bytes;
        }
        else {
            data_type result;
            vector_append_to_net(result, bytes);
            return result;
        }
    }, cmd);
}
