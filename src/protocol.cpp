#include "protocol.h"

#include <filesystem>

#include <asio.hpp>
#include <variant>
#include <spdlog/spdlog.h>
#include "utils/SmallVector.h"


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

void UsbIpHeaderBasic::from_socket(asio::ip::tcp::socket &sock) {
    unsigned_integral_read_from_socket(sock, seqnum, devid, direction, ep);
}

array_data_type<calculate_total_size_with_array<
    decltype(UsbIpIsoPacketDescriptor::offset),
    decltype(UsbIpIsoPacketDescriptor::length),
    decltype(UsbIpIsoPacketDescriptor::actual_length),
    decltype(UsbIpIsoPacketDescriptor::status)
>()> usbipdcpp::UsbIpIsoPacketDescriptor::to_bytes() const {
    return to_network_array(offset, length, actual_length, status);
}

void usbipdcpp::UsbIpIsoPacketDescriptor::from_socket(asio::ip::tcp::socket &sock) {
    unsigned_integral_read_from_socket(sock, offset, length, actual_length, status);
}

std::vector<std::uint8_t> usbipdcpp::UsbIpResponse::OpRepDevlist::to_bytes() const {
    std::vector<std::uint8_t> result = to_network_data(USBIP_VERSION, OP_REP_DEVLIST, status, device_count);
    for (auto &device: devices) {
        auto bytes = device.to_bytes_with_interfaces();
        result.insert(result.end(), bytes.begin(), bytes.end());
    }
    return result;
}

void UsbIpResponse::OpRepDevlist::to_socket(asio::ip::tcp::socket &sock, error_code &ec) const {
    asio::write(sock, asio::buffer(to_network_data(USBIP_VERSION, OP_REP_DEVLIST, status, device_count)), ec);
    for (auto &device: devices) {
        asio::write(sock, asio::buffer(device.to_bytes_with_interfaces()), ec);
    }
}

void UsbIpResponse::OpRepDevlist::from_socket(asio::ip::tcp::socket &sock) {
    return;
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

void UsbIpResponse::OpRepImport::to_socket(asio::ip::tcp::socket &sock, error_code &ec) const {
    std::array<asio::const_buffer, 2> buffers;
    auto data1 = to_network_array(USBIP_VERSION, OP_REP_IMPORT, status);
    buffers[0] = asio::buffer(data1);
    if (status == 0) {
        if (device) {
            auto data2 = device->to_bytes();
            buffers[1] = asio::buffer(data2);
            asio::write(sock, buffers, ec);
        }
        else {
            asio::write(sock, buffers[0], ec);
        }
    }
    else {
        asio::write(sock, buffers[0], ec);
    }
}

void UsbIpResponse::OpRepImport::from_socket(asio::ip::tcp::socket &sock) {
    return;
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

void UsbIpResponse::UsbIpRetSubmit::to_socket(asio::ip::tcp::socket &sock, error_code &ec) const {
    assert(header.command==USBIP_RET_SUBMIT);

    auto data1 = array_add_padding<8>(to_network_array(header.to_bytes(), status, actual_length, start_frame,
                                                       number_of_packets, error_count));
    // spdlog::debug("RET_SUBMIT seqnum:{} status:{} actual_length:{} buffer_size:{}",
    //              header.seqnum, status, actual_length, transfer_buffer.size());
    if (!transfer_buffer.empty() && actual_length > 0)[[likely]] {
        // 验证长度一致性：actual_length 不应超过有效缓冲区大小
        assert(actual_length <= transfer_buffer.size() - send_config.data_offset);
        if (iso_packet_descriptor.empty())[[likely]] {
            // 非等时传输：header + data
            std::array<asio::const_buffer, 2> buffers;
            buffers[0] = asio::buffer(data1);
            buffers[1] = asio::buffer(transfer_buffer.data() + send_config.data_offset, actual_length);
            asio::write(sock, buffers, ec);
        }
        else {
            // 等时传输：header + scatter-gather data + scatter-gather iso descriptors
            // 零动态分配（64 个 iso packet 以内）

            // 存储每个 iso descriptor 的字节（每个 16 字节）
            SmallVector<decltype(UsbIpIsoPacketDescriptor{}.to_bytes()), 64> iso_desc_bytes;
            for (const auto &iso: iso_packet_descriptor) {
                iso_desc_bytes.push_back(iso.to_bytes());
            }

            // 组织 scatter-gather buffer
            SmallVector<asio::const_buffer, 130> buffers; // 1 header + 64 data + 64 iso_desc
            buffers.push_back(asio::buffer(data1));

            // scatter-gather 发送每个 iso packet 数据
            size_t original_offset = 0;
            for (const auto &iso: iso_packet_descriptor) {
                // 验证偏移和长度不越界
                assert(original_offset + iso.actual_length <= transfer_buffer.size());
                buffers.push_back(asio::buffer(
                        transfer_buffer.data() + original_offset,
                        iso.actual_length
                        ));
                original_offset += iso.length_in_transfer_buffer_only_for_send;
            }

            // scatter-gather 发送每个 iso descriptor
            for (const auto &bytes: iso_desc_bytes) {
                buffers.push_back(asio::buffer(bytes));
            }

            asio::write(sock, buffers, ec);
        }
    }
    else {
        asio::write(sock, asio::buffer(data1), ec);
    }
}

void UsbIpResponse::UsbIpRetSubmit::from_socket(asio::ip::tcp::socket &sock) {
    return;
}


usbipdcpp::UsbIpResponse::UsbIpRetSubmit usbipdcpp::UsbIpResponse::UsbIpRetSubmit::create_ret_submit(
        std::uint32_t seqnum, std::uint32_t status, std::uint32_t actual_length,
        std::uint32_t start_frame, std::uint32_t number_of_packets,
        std::vector<std::uint8_t> &&transfer_buffer,
        std::vector<UsbIpIsoPacketDescriptor> &&iso_packet_descriptor) {
    auto ret = UsbIpRetSubmit{
            .header = UsbIpHeaderBasic::get_server_header(
                    USBIP_RET_SUBMIT,
                    seqnum
                    ),
            .status = status,
            .actual_length = actual_length,
            .start_frame = start_frame,
            .number_of_packets = number_of_packets,
            .error_count = 0,
            .transfer_buffer = std::move(transfer_buffer),
            .iso_packet_descriptor = std::move(iso_packet_descriptor)
    };
    return ret;
}

usbipdcpp::UsbIpResponse::UsbIpRetSubmit usbipdcpp::UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(
        std::uint32_t seqnum, std::uint32_t actual_length) {
    auto ret = UsbIpRetSubmit{
            .header = UsbIpHeaderBasic::get_server_header(
                    USBIP_RET_SUBMIT,
                    seqnum
                    ),
            .status = static_cast<std::uint32_t>(UrbStatusType::StatusOK),
            .actual_length = actual_length,
            .start_frame = 0,
            .number_of_packets = 0,
            .error_count = 0,
            .transfer_buffer = {},
            .iso_packet_descriptor = {}
    };
    return ret;
}

usbipdcpp::UsbIpResponse::UsbIpRetSubmit usbipdcpp::UsbIpResponse::UsbIpRetSubmit::
create_ret_submit_with_status_and_no_data(std::uint32_t seqnum, std::uint32_t status, std::uint32_t actual_length) {
    auto ret = UsbIpRetSubmit{
            .header = UsbIpHeaderBasic::get_server_header(
                    USBIP_RET_SUBMIT,
                    seqnum
                    ),
            .status = status,
            .actual_length = actual_length,
            .start_frame = 0,
            .number_of_packets = 0,
            .error_count = 0,
            .transfer_buffer = {},
            .iso_packet_descriptor = {}
    };
    return ret;
}

usbipdcpp::UsbIpResponse::UsbIpRetSubmit usbipdcpp::UsbIpResponse::UsbIpRetSubmit::
create_ret_submit_with_status_and_no_iso(std::uint32_t seqnum, std::uint32_t status, std::uint32_t actual_length,
                                         const data_type &transfer_buffer) {
    auto ret = UsbIpRetSubmit{
            .header = UsbIpHeaderBasic::get_server_header(
                    USBIP_RET_SUBMIT,
                    seqnum
                    ),
            .status = status,
            .actual_length = actual_length,
            .start_frame = 0,
            .number_of_packets = 0,
            .error_count = 0,
            .transfer_buffer = transfer_buffer,
            .iso_packet_descriptor = {}
    };
    return ret;
}

usbipdcpp::UsbIpResponse::UsbIpRetSubmit usbipdcpp::UsbIpResponse::UsbIpRetSubmit::
create_ret_submit_with_status_and_no_iso(std::uint32_t seqnum, std::uint32_t status, std::uint32_t actual_length,
                                         data_type &&transfer_buffer) {
    auto ret = UsbIpRetSubmit{
            .header = UsbIpHeaderBasic::get_server_header(
                    USBIP_RET_SUBMIT,
                    seqnum
                    ),
            .status = status,
            .actual_length = actual_length,
            .start_frame = 0,
            .number_of_packets = 0,
            .error_count = 0,
            .transfer_buffer = std::move(transfer_buffer),
            .iso_packet_descriptor = {}
    };
    return ret;
}

usbipdcpp::UsbIpResponse::UsbIpRetSubmit usbipdcpp::UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_no_iso(
        std::uint32_t seqnum, std::uint32_t actual_length,
        const data_type &transfer_buffer) {
    return create_ret_submit_with_status_and_no_iso(seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE),
                                                    actual_length, transfer_buffer);
}

usbipdcpp::UsbIpResponse::UsbIpRetSubmit usbipdcpp::UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_no_iso(
        std::uint32_t seqnum, std::uint32_t actual_length,
        data_type &&transfer_buffer) {
    return create_ret_submit_with_status_and_no_iso(seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE),
                                                    actual_length, std::move(transfer_buffer));
}

usbipdcpp::UsbIpResponse::UsbIpRetSubmit usbipdcpp::UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(
        std::uint32_t seqnum, std::uint32_t actual_length) {
    auto ret = UsbIpRetSubmit{
            .header = UsbIpHeaderBasic::get_server_header(
                    USBIP_RET_SUBMIT,
                    seqnum
                    ),
            .status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE),
            .actual_length = actual_length,
            .start_frame = 0,
            .number_of_packets = 0,
            .error_count = 0,
            .transfer_buffer = {},
            .iso_packet_descriptor = {}
    };
    return ret;
}

usbipdcpp::UsbIpResponse::UsbIpRetSubmit usbipdcpp::UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
        std::uint32_t seqnum, std::uint32_t actual_length, const data_type &transfer_buffer) {
    return create_ret_submit_with_status_and_no_iso(seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK),
                                                    actual_length, transfer_buffer);
}

usbipdcpp::UsbIpResponse::UsbIpRetSubmit usbipdcpp::UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
        std::uint32_t seqnum, std::uint32_t actual_length, data_type &&transfer_buffer) {
    return create_ret_submit_with_status_and_no_iso(seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK),
                                                    actual_length, std::move(transfer_buffer));
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

void UsbIpResponse::UsbIpRetUnlink::to_socket(asio::ip::tcp::socket &sock, error_code &ec) const {
    asio::write(sock, asio::buffer(to_bytes()), ec);
}

void usbipdcpp::UsbIpResponse::UsbIpRetUnlink::from_socket(asio::ip::tcp::socket &sock) {
    return;
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
            .status = static_cast<std::uint32_t>(UrbStatusType::StatusOK)
    };
}

array_data_type<calculate_total_size_with_array<
    decltype(USBIP_VERSION), decltype(OP_REQ_DEVLIST), decltype(UsbIpCommand::OpReqDevlist::status)
>()> usbipdcpp::UsbIpCommand::OpReqDevlist::to_bytes() const {
    return to_network_array(USBIP_VERSION, OP_REQ_DEVLIST, status);
}

void UsbIpCommand::OpReqDevlist::from_socket(asio::ip::tcp::socket &sock) {
    status = read_u32(sock);
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

void UsbIpCommand::OpReqImport::to_socket(asio::ip::tcp::socket &sock, error_code &ec) const {
    asio::write(sock, asio::buffer(to_bytes()), ec);
}

void usbipdcpp::UsbIpCommand::OpReqImport::from_socket(asio::ip::tcp::socket &sock) {
    data_read_from_socket(sock, status, busid);
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

void UsbIpCommand::UsbIpCmdSubmit::to_socket(asio::ip::tcp::socket &sock, error_code &ec) const {
    assert(header.direction!=UsbIpDirection::Out||transfer_buffer_length==data.size());
    std::array<asio::const_buffer, 2> buffers;
    buffers[0] = asio::buffer(
            to_network_array(
                    header.to_bytes(),
                    transfer_flags,
                    transfer_buffer_length,
                    start_frame,
                    number_of_packets,
                    interval,
                    setup.to_bytes()
                    )
            );
    buffers[1] = asio::buffer(data);
    asio::write(sock, buffers, ec);
    // iso传输
    if (!iso_packet_descriptor.empty()) {
        auto iso_bytes = serializable_array_range_to_network_data(iso_packet_descriptor);
        asio::write(sock, asio::buffer(iso_bytes), ec);
    }
}

void UsbIpCommand::UsbIpCmdSubmit::from_socket(asio::ip::tcp::socket &sock) {
    // 使用 scatter-gather 一次性读取固定部分
    // header 字段(16字节) + transfer参数(20字节) + setup(8字节) = 44字节
    decltype(SetupPacket{}.to_bytes()) setup_buffer;
    unsigned_integral_and_array_read_from_socket(
        sock,
        header.seqnum, header.devid, header.direction, header.ep,
        transfer_flags, transfer_buffer_length, start_frame, number_of_packets, interval,
        setup_buffer
    );
    //设置命令类型
    header.command = USBIP_CMD_SUBMIT;

    // 解析 setup packet（小端序）
    setup = SetupPacket::parse(setup_buffer);

    // 检查缓冲区大小，防止恶意大内存分配
    if (transfer_buffer_length > USBIPDCPP_MAX_TRANSFER_BUFFER_SIZE)[[unlikely]] {
        throw std::system_error(std::make_error_code(std::errc::no_buffer_space), "transfer_buffer_length too large");
    }
    data.resize(transfer_buffer_length);

    if (header.direction == UsbIpDirection::In)[[likely]] {
        // IN 传输不需要从 socket 读取数据
    }
    else {
        asio::read(sock, asio::buffer(data));
    }

    if (number_of_packets != 0 && number_of_packets != 0xFFFFFFFF)[[unlikely]] {
        // 等时传输较少见
        iso_packet_descriptor.resize(number_of_packets);
        for (auto &iso_packet: iso_packet_descriptor) {
            iso_packet.from_socket(sock);
        }
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

void UsbIpCommand::UsbIpCmdUnlink::to_socket(asio::ip::tcp::socket &sock, error_code &ec) const {
    asio::write(sock, asio::buffer(to_bytes()), ec);
}

void UsbIpCommand::UsbIpCmdUnlink::from_socket(asio::ip::tcp::socket &sock) {
    // 使用 scatter-gather 一次性读取 header 字段 + unlink_seqnum + padding
    unsigned_integral_and_array_read_from_socket<24>(
        sock,
        header.seqnum, header.devid, header.direction, header.ep,
        unlink_seqnum
    );
    //设置命令类型
    header.command = USBIP_CMD_UNLINK;
}

usbipdcpp::UsbIpCommand::OpCmdVariant usbipdcpp::UsbIpCommand::get_op_from_socket(
        asio::ip::tcp::socket &sock, usbipdcpp::error_code &ec) {
    try {
        auto version = read_u16(sock);
        if (version != 0 && version != USBIP_VERSION) {
            ec = make_error_code(ErrorType::UNKNOWN_VERSION);
            return OpCmdVariant{};
        }
        auto op = read_u16(sock);
        SPDLOG_DEBUG("收到op: 0x{:04x}", op);

        switch (op) {
            case OP_REQ_DEVLIST: {
                auto req = OpReqDevlist{};
                req.from_socket(sock);
                return req;
                break;
            }
            case OP_REQ_IMPORT: {
                auto req = OpReqImport{};
                req.from_socket(sock);
                return req;
                break;
            }
            default: {
                ec = make_error_code(ErrorType::UNKNOWN_CMD);
                return UsbIpCommand::OpCmdVariant{};
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
    return UsbIpCommand::OpCmdVariant{};
}

usbipdcpp::UsbIpCommand::CmdVariant usbipdcpp::UsbIpCommand::get_cmd_from_socket(
        asio::ip::tcp::socket &sock, usbipdcpp::error_code &ec) {

    try {
        auto command = read_u32(sock);
        SPDLOG_DEBUG("收到command: 0x{:04x}", command);

        switch (command) {
            case USBIP_CMD_SUBMIT: {
                auto cmd = UsbIpCmdSubmit{};
                cmd.from_socket(sock);
                return cmd;
                break;
            }
            case USBIP_CMD_UNLINK: {
                auto cmd = UsbIpCmdUnlink{};
                cmd.from_socket(sock);
                return cmd;
                break;
            }
            default: {
                ec = make_error_code(ErrorType::UNKNOWN_CMD);
                return UsbIpCommand::CmdVariant{};
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
    return UsbIpCommand::CmdVariant{};

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
