#include "protocol.h"

#include <filesystem>

#include <asio.hpp>
#include <variant>
#include <spdlog/spdlog.h>
#include "utils/SmallVector.h"
#include "DeviceHandler/DeviceHandler.h"


using namespace usbipdcpp;

// ========== TransferHandle 实现 ==========

TransferHandle::TransferHandle(void* handle, AbstDeviceHandler* handler)
    : handle_(handle), handler_(handler) {}

TransferHandle::TransferHandle(TransferHandle&& other) noexcept
    : handle_(other.handle_), handler_(other.handler_) {
    other.handle_ = nullptr;
    other.handler_ = nullptr;
}

TransferHandle& TransferHandle::operator=(TransferHandle&& other) noexcept {
    if (this != &other) {
        reset();
        handle_ = other.handle_;
        handler_ = other.handler_;
        other.handle_ = nullptr;
        other.handler_ = nullptr;
    }
    return *this;
}

TransferHandle::~TransferHandle() {
    reset();
}

void TransferHandle::reset() {
    if (handle_ && handler_) {
        handler_->free_transfer_handle(handle_);
    }
    handle_ = nullptr;
    handler_ = nullptr;
}

void* TransferHandle::release() {
    void* tmp = handle_;
    handle_ = nullptr;
    handler_ = nullptr;
    return tmp;
}

// ========== 其他协议实现 ==========

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

    // 从 transfer 获取数据
    if (transfer && actual_length > 0) {
        auto* handler = transfer.handler();
        void* raw_handle = transfer.get();
        void* buffer = handler->get_transfer_buffer(raw_handle);
        auto offset = handler->get_read_data_offset(raw_handle);
        auto byte_ptr = static_cast<const std::uint8_t*>(buffer) + offset;
        total_result.insert(total_result.end(), byte_ptr, byte_ptr + actual_length);

        // iso 描述符
        int num_iso = (number_of_packets != 0 && number_of_packets != 0xFFFFFFFF) ? static_cast<int>(number_of_packets) : 0;
        for (int i = 0; i < num_iso; i++) {
            auto iso_desc = handler->get_iso_descriptor(raw_handle, i);
            auto iso_byte = iso_desc.to_bytes();
            vector_append_to_net(total_result, iso_byte);
        }
    }

    return total_result;
}

void UsbIpResponse::UsbIpRetSubmit::to_socket(asio::ip::tcp::socket &sock, error_code &ec) const {
    assert(header.command==USBIP_RET_SUBMIT);

    auto data1 = array_add_padding<8>(to_network_array(header.to_bytes(), status, actual_length, start_frame,
                                                       number_of_packets, error_count));

    // 从 transfer 获取数据
    if (transfer && actual_length > 0)[[likely]] {
        auto* handler = transfer.handler();
        void* raw_handle = transfer.get();
        void* buffer = handler->get_transfer_buffer(raw_handle);
        auto offset = handler->get_read_data_offset(raw_handle);

        int num_iso = (number_of_packets != 0 && number_of_packets != 0xFFFFFFFF) ? static_cast<int>(number_of_packets) : 0;

        if (num_iso == 0)[[likely]] {
            // 非等时传输：header + data
            std::array<asio::const_buffer, 2> buffers;
            buffers[0] = asio::buffer(data1);
            buffers[1] = asio::buffer(static_cast<const char*>(buffer) + offset, actual_length);
            asio::write(sock, buffers, ec);
        }
        else {
            // 等时传输：header + scatter-gather data + scatter-gather iso descriptors
            SmallVector<asio::const_buffer, 130> buffers; // 1 header + 64 data + 64 iso_desc
            buffers.push_back(asio::buffer(data1));

            // scatter-gather 发送每个 iso packet 数据
            size_t current_offset = 0;
            for (int i = 0; i < num_iso; i++) {
                auto iso = handler->get_iso_descriptor(raw_handle, i);
                buffers.push_back(asio::buffer(
                        static_cast<const char*>(buffer) + current_offset,
                        iso.actual_length
                        ));
                current_offset += iso.length_in_transfer_buffer_only_for_send;
            }

            // scatter-gather 发送每个 iso descriptor
            for (int i = 0; i < num_iso; i++) {
                auto iso_desc = handler->get_iso_descriptor(raw_handle, i);
                buffers.push_back(asio::buffer(iso_desc.to_bytes()));
            }

            asio::write(sock, buffers, ec);
        }
        // transfer 会在 UsbIpRetSubmit 析构时自动释放
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
        TransferHandle transfer) {
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
            .transfer = std::move(transfer)
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
            .error_count = 0
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
            .error_count = 0
    };
    return ret;
}

usbipdcpp::UsbIpResponse::UsbIpRetSubmit usbipdcpp::UsbIpResponse::UsbIpRetSubmit::
create_ret_submit_with_status_and_no_iso(std::uint32_t seqnum, std::uint32_t status, std::uint32_t actual_length,
                                         TransferHandle transfer) {
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
            .transfer = std::move(transfer)
    };
    return ret;
}

usbipdcpp::UsbIpResponse::UsbIpRetSubmit usbipdcpp::UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_no_iso(
        std::uint32_t seqnum,
        std::uint32_t actual_length,
        TransferHandle transfer) {
    return create_ret_submit_with_status_and_no_iso(seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE),
                                                    actual_length, std::move(transfer));
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
            .error_count = 0
    };
    return ret;
}

usbipdcpp::UsbIpResponse::UsbIpRetSubmit usbipdcpp::UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
        std::uint32_t seqnum,
        std::uint32_t actual_length,
        TransferHandle transfer) {
    return create_ret_submit_with_status_and_no_iso(seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK),
                                                    actual_length, std::move(transfer));
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
        decltype(OP_REQ_IMPORT),
        decltype(UsbIpCommand::OpReqImport::status),
        decltype(UsbIpCommand::OpReqImport::busid)
    >()
> usbipdcpp::UsbIpCommand::OpReqImport::to_bytes() const {
    return to_network_array(USBIP_VERSION, OP_REQ_IMPORT, status, busid);
}

void UsbIpCommand::OpReqImport::to_socket(asio::ip::tcp::socket &sock, error_code &ec) const {
    asio::write(sock, asio::buffer(to_bytes()), ec);
}

void usbipdcpp::UsbIpCommand::OpReqImport::from_socket(asio::ip::tcp::socket &sock) {
    data_read_from_socket(sock, status, busid);
    assert(status == 0);
}

std::vector<std::uint8_t> usbipdcpp::UsbIpCommand::UsbIpCmdSubmit::to_bytes() const {
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
                            )
                    );
    // 从 transfer 获取数据
    if (transfer) {
        auto* handler = transfer.handler();
        void* raw_handle = transfer.get();
        void* buffer = handler->get_transfer_buffer(raw_handle);
        auto write_offset = handler->get_write_data_offset(header);
        auto byte_ptr = static_cast<const std::uint8_t*>(buffer) + write_offset;
        total_result.insert(total_result.end(), byte_ptr, byte_ptr + transfer_buffer_length);

        // iso 描述符
        int num_iso = (number_of_packets != 0 && number_of_packets != 0xFFFFFFFF) ? static_cast<int>(number_of_packets) : 0;
        for (int i = 0; i < num_iso; i++) {
            auto iso_desc = handler->get_iso_descriptor(raw_handle, i);
            auto iso_byte = iso_desc.to_bytes();
            vector_append_to_net(total_result, iso_byte);
        }
    }
    return total_result;
}

void UsbIpCommand::UsbIpCmdSubmit::to_socket(asio::ip::tcp::socket &sock, error_code &ec) const {
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
    // 从 transfer 获取数据
    if (transfer) {
        auto* handler = transfer.handler();
        void* buffer = handler->get_transfer_buffer(transfer.get());
        auto write_offset = handler->get_write_data_offset(header);
        buffers[1] = asio::buffer(static_cast<const char*>(buffer) + write_offset, transfer_buffer_length);
        asio::write(sock, buffers, ec);
    }
    else {
        asio::write(sock, buffers[0], ec);
    }
    // iso传输
    if (number_of_packets != 0 && number_of_packets != 0xFFFFFFFF && transfer) {
        auto* handler = transfer.handler();
        void* raw_handle = transfer.get();
        int num_iso = static_cast<int>(number_of_packets);
        for (int i = 0; i < num_iso; i++) {
            auto iso_desc = handler->get_iso_descriptor(raw_handle, i);
            auto iso_byte = iso_desc.to_bytes();
            asio::write(sock, asio::buffer(iso_byte), ec);
        }
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

    // 使用 handler 创建 transfer_handle
    int num_iso = (number_of_packets != 0 && number_of_packets != 0xFFFFFFFF) ? static_cast<int>(number_of_packets) : 0;
    auto* raw_handle = transfer.handler()->alloc_transfer_handle(transfer_buffer_length, num_iso, header, setup);
    transfer.set_handle(raw_handle);

    // 获取 buffer 头指针和写入偏移量
    void* buffer = transfer.handler()->get_transfer_buffer(raw_handle);
    std::size_t write_offset = transfer.handler()->get_write_data_offset(header);

    if (header.direction == UsbIpDirection::In)[[likely]] {
        // IN 传输不需要从 socket 读取数据
    }
    else {
        // OUT 传输：读入到 buffer + write_offset 位置（跳过 setup 空间）
        asio::read(sock, asio::buffer(
            static_cast<std::uint8_t*>(buffer) + write_offset,
            transfer_buffer_length
        ));
    }

    // 读取等时包描述符
    if (number_of_packets != 0 && number_of_packets != 0xFFFFFFFF)[[unlikely]] {
        for (int i = 0; i < num_iso; i++) {
            UsbIpIsoPacketDescriptor iso_desc{};
            iso_desc.from_socket(sock);
            transfer.handler()->set_iso_descriptor(raw_handle, i, iso_desc);
        }
    }
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
        asio::ip::tcp::socket &sock, AbstDeviceHandler* handler, usbipdcpp::error_code &ec) {

    try {
        auto command = read_u32(sock);
        SPDLOG_DEBUG("收到command: 0x{:04x}", command);

        switch (command) {
            case USBIP_CMD_SUBMIT: {
                auto cmd = UsbIpCmdSubmit{};
                cmd.transfer.set_handler(handler);
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
