#include "usbipdcpp/protocol.h"

#include <algorithm>
#include <filesystem>

#include <asio.hpp>
#include <spdlog/spdlog.h>
#include <variant>
#include "usbipdcpp/DeviceHandler/DeviceHandler.h"
#include "usbipdcpp/utils/SmallVector.h"


using namespace usbipdcpp;

// ========== TransferHandle 实现 ==========

TransferHandle::TransferHandle(void *handle, TransferOperator *op) : handle_(handle), op_(op) {
}

TransferHandle::TransferHandle(TransferHandle &&other) noexcept : handle_(other.handle_), op_(other.op_) {
    other.handle_ = nullptr;
    other.op_ = nullptr;
}

TransferHandle &TransferHandle::operator=(TransferHandle &&other) noexcept {
    if (this != &other) {
        reset();
        handle_ = other.handle_;
        op_ = other.op_;
        other.handle_ = nullptr;
        other.op_ = nullptr;
    }
    return *this;
}

TransferHandle::~TransferHandle() {
    reset();
}

void TransferHandle::reset() {
    if (handle_ && op_) {
        op_->free_transfer_handle(handle_);
    }
    handle_ = nullptr;
    op_ = nullptr;
}

void *TransferHandle::release() {
    void *tmp = handle_;
    handle_ = nullptr;
    op_ = nullptr;
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
        default:;
            return "Unknown Error";
    }
}


std::error_code usbipdcpp::make_error_code(ErrorType e) {
    return {static_cast<int>(e), g_error_category};
}

array_data_type<usbipdcpp::calculate_total_size_with_array<
        decltype(UsbIpHeaderBasic::command), decltype(UsbIpHeaderBasic::seqnum), decltype(UsbIpHeaderBasic::devid),
        decltype(UsbIpHeaderBasic::direction), decltype(UsbIpHeaderBasic::ep)>()>
UsbIpHeaderBasic::to_bytes() const {
    return to_network_array(command, seqnum, devid, direction, ep);
}

void UsbIpHeaderBasic::from_socket(asio::ip::tcp::socket &sock) {
    unsigned_integral_read_from_socket(sock, seqnum, devid, direction, ep);
}

array_data_type<calculate_total_size_with_array<
        decltype(UsbIpIsoPacketDescriptor::offset), decltype(UsbIpIsoPacketDescriptor::length),
        decltype(UsbIpIsoPacketDescriptor::actual_length), decltype(UsbIpIsoPacketDescriptor::status)>()>
usbipdcpp::UsbIpIsoPacketDescriptor::to_bytes() const {
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
    // 与 to_socket 对称：status + device_count + 每设备（312 字节固定头部 +
    // 接口计数个 4 字节接口体，接口体数量在设备头部末尾）。version/command
    // 两个字段由调用方在读命令码时消费，不在此读取
    data_read_from_socket(sock, status, device_count);
    devices.clear();
    // device_count 来自网络不可信：恶意服务端可发巨大值，reserve 直接抛
    // bad_alloc 崩溃（防御风格同 CmdSubmit::from_socket 的大小校验）
    devices.reserve(std::min<std::uint32_t>(device_count, 256));
    for (std::uint32_t i = 0; i < device_count; i++) {
        UsbDevice device;
        device.from_socket(sock);
        // 接口体紧跟在设备头部之后（to_bytes_with_interfaces 的格式），
        // 数量由头部末尾的接口计数给出
        for (auto &intf: device.interfaces) {
            intf.from_socket(sock);
        }
        devices.push_back(std::move(device));
    }
}


usbipdcpp::UsbIpResponse::OpRepDevlist
usbipdcpp::UsbIpResponse::OpRepDevlist::create_from_devices(const std::vector<std::shared_ptr<UsbDevice>> &devices) {
    std::vector<UsbDevice> ret_devices;
    ret_devices.reserve(devices.size());
    for (auto &device: devices) {
        ret_devices.emplace_back(*device);
    }
    return {.status = 0, .device_count = static_cast<uint32_t>(ret_devices.size()), .devices = std::move(ret_devices)};
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
    // 与 to_socket 对称：status 后（成功时）跟设备描述符（312 字节固定头部，
    // 不含接口体，服务端 import 响应只发 to_bytes()）。version/command
    // 由调用方在读命令码时消费
    data_read_from_socket(sock, status);
    if (status == 0) {
        device = std::make_shared<UsbDevice>();
        device->from_socket(sock);
    }
}

usbipdcpp::UsbIpResponse::OpRepImport
usbipdcpp::UsbIpResponse::OpRepImport::create_on_failure_with_status(std::uint32_t status) {
    return {.status = status, .device = {}};
}

usbipdcpp::UsbIpResponse::OpRepImport usbipdcpp::UsbIpResponse::OpRepImport::create_on_failure() {
    return create_on_failure_with_status(static_cast<std::uint32_t>(OperationStatuType::NA));
}

usbipdcpp::UsbIpResponse::OpRepImport
usbipdcpp::UsbIpResponse::OpRepImport::create_on_success(std::shared_ptr<UsbDevice> device) {
    return {.status = static_cast<std::uint32_t>(OperationStatuType::OK), .device = std::move(device)};
}


void UsbIpResponse::UsbIpRetSubmit::to_socket(asio::ip::tcp::socket &sock, error_code &ec) const {
    assert(header.command == USBIP_RET_SUBMIT);

    auto data1 = array_add_padding<8>(
            to_network_array(header.to_bytes(), status, actual_length, start_frame, number_of_packets, error_count));

    // 从 transfer 获取数据。入口条件不能只看 actual_length：ISO 传输即使
    // actual_length 为 0（如全包失败/0 字节）也必须由 operator 发送 iso
    // 描述符（vhci 按 number_of_packets 读取，不发会错位）；数据发送
    // length 按 transfer_is_in 计算：IN 传 actual_length（数据回发），OUT
    // 恒为 0（OUT 应答无数据回发，见 stub_tx.c 的 usb_pipein 条件；vhci
    // 对 OUT 也不读数据，传非 0 会把数据误读成 iso 描述符导致校验失败）。
    // 非 ISO 传输 actual_length 为 0 时走 else 分支只发 header——不能把
    // 0 字节的非 ISO 传输放进 op 路径（如存储后端的零拷贝 send_direct 在
    // Windows 上 TransmitFile(0) 会发送整个文件）
    if (transfer && (actual_length > 0 || number_of_packets > 0)) {
        auto *op = transfer.get_operator();
        void *raw_handle = transfer.get();

        asio::write(sock, asio::buffer(data1), ec);
        if (!ec)
            op->send_transfer_data(raw_handle, sock, op->transfer_is_in(raw_handle) ? actual_length : 0, ec);
    }
    else {
        asio::write(sock, asio::buffer(data1), ec);
    }
}

void UsbIpResponse::UsbIpRetSubmit::from_socket(asio::ip::tcp::socket &sock) {
    // 与 to_socket 对称：header 与传输参数共 40 字节，尾随 8 字节 padding，
    // 凑齐内核 usbip_header 的 48 字节（vhci_rx_pdu 一次读 sizeof(usbip_header)，
    // ret_submit 在 union 中占 20 字节，剩余 8 字节本项目以零填充）。
    // command 由本函数设置，不读
    unsigned_integral_and_array_read_from_socket<8>(sock, header.seqnum, header.devid, header.direction, header.ep,
                                                    status, actual_length, start_frame, number_of_packets,
                                                    error_count);
    header.command = USBIP_RET_SUBMIT;

    // 数据阶段不在此读取：RET_SUBMIT 的头部不含方向信息（服务端回包时清零），
    // 无法判断数据阶段是否存在——actual_length 对 OUT 传输只是已接收字节数，
    // 内核 stub 对 OUT 响应不回发数据（stub_tx.c 只在 usb_pipein 时发 buffer）。
    // 内核 vhci 用 seqnum 匹配自己发出的 CMD_SUBMIT，按 URB 方向决定是否读数据
    // （usbip_recv_xbuff 对 OUT 直接跳过）。需要数据阶段的调用方应基于自己的
    // 传输方向，通过 transfer 的 recv_transfer_data 显式读取
}


usbipdcpp::UsbIpResponse::UsbIpRetSubmit
usbipdcpp::UsbIpResponse::UsbIpRetSubmit::create_ret_submit(std::uint32_t seqnum, std::uint32_t status,
                                                            std::uint32_t actual_length, std::uint32_t start_frame,
                                                            std::uint32_t number_of_packets, TransferHandle transfer) {
    auto ret = UsbIpRetSubmit{.header = UsbIpHeaderBasic::get_server_header(USBIP_RET_SUBMIT, seqnum),
                              .status = status,
                              .actual_length = actual_length,
                              .start_frame = start_frame,
                              .number_of_packets = number_of_packets,
                              .error_count = 0,
                              .transfer = std::move(transfer)};
    return ret;
}

usbipdcpp::UsbIpResponse::UsbIpRetSubmit
usbipdcpp::UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(std::uint32_t seqnum,
                                                                            std::uint32_t actual_length) {
    auto ret = UsbIpRetSubmit{.header = UsbIpHeaderBasic::get_server_header(USBIP_RET_SUBMIT, seqnum),
                              .status = static_cast<std::uint32_t>(UrbStatusType::StatusOK),
                              .actual_length = actual_length,
                              .start_frame = 0,
                              .number_of_packets = 0,
                              .error_count = 0};
    return ret;
}

usbipdcpp::UsbIpResponse::UsbIpRetSubmit
usbipdcpp::UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(std::uint32_t seqnum,
                                                                                    std::uint32_t status,
                                                                                    std::uint32_t actual_length) {
    auto ret = UsbIpRetSubmit{.header = UsbIpHeaderBasic::get_server_header(USBIP_RET_SUBMIT, seqnum),
                              .status = status,
                              .actual_length = actual_length,
                              .start_frame = 0,
                              .number_of_packets = 0,
                              .error_count = 0};
    return ret;
}

usbipdcpp::UsbIpResponse::UsbIpRetSubmit
usbipdcpp::UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_iso(std::uint32_t seqnum,
                                                                                   std::uint32_t status,
                                                                                   std::uint32_t actual_length,
                                                                                   TransferHandle transfer) {
    auto ret = UsbIpRetSubmit{.header = UsbIpHeaderBasic::get_server_header(USBIP_RET_SUBMIT, seqnum),
                              .status = status,
                              .actual_length = actual_length,
                              .start_frame = 0,
                              .number_of_packets = 0,
                              .error_count = 0,
                              .transfer = std::move(transfer)};
    return ret;
}

usbipdcpp::UsbIpResponse::UsbIpRetSubmit usbipdcpp::UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_no_iso(
        std::uint32_t seqnum, std::uint32_t actual_length, TransferHandle transfer) {
    return create_ret_submit_with_status_and_no_iso(seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE),
                                                    actual_length, std::move(transfer));
}

usbipdcpp::UsbIpResponse::UsbIpRetSubmit
usbipdcpp::UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(std::uint32_t seqnum,
                                                                               std::uint32_t actual_length) {
    auto ret = UsbIpRetSubmit{.header = UsbIpHeaderBasic::get_server_header(USBIP_RET_SUBMIT, seqnum),
                              .status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE),
                              .actual_length = actual_length,
                              .start_frame = 0,
                              .number_of_packets = 0,
                              .error_count = 0};
    return ret;
}

usbipdcpp::UsbIpResponse::UsbIpRetSubmit usbipdcpp::UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_with_no_iso(
        std::uint32_t seqnum, std::uint32_t actual_length, TransferHandle transfer) {
    return create_ret_submit_with_status_and_no_iso(seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusOK),
                                                    actual_length, std::move(transfer));
}

array_data_type<calculate_total_size_with_array<decltype(UsbIpHeaderBasic{}.to_bytes()),
                                                decltype(UsbIpResponse::UsbIpRetUnlink::status)>() +
                24>
UsbIpResponse::UsbIpRetUnlink::to_bytes() const {
    assert(header.command == USBIP_RET_UNLINK);
    auto result = to_network_array(header.to_bytes(), status);
    return array_add_padding<24>(result);
}

void UsbIpResponse::UsbIpRetUnlink::to_socket(asio::ip::tcp::socket &sock, error_code &ec) const {
    SPDLOG_DEBUG("RET_UNLINK to_socket: seqnum={} status={}", header.seqnum, status);
    asio::write(sock, asio::buffer(to_bytes()), ec);
}

void usbipdcpp::UsbIpResponse::UsbIpRetUnlink::from_socket(asio::ip::tcp::socket &sock) {
    // 与 to_bytes 对称：header 20 字节 + status 4 字节 + 24 字节 padding，
    // 凑齐内核 usbip_header 的 48 字节（ret_unlink 在 union 中只占 4 字节）。
    // command 由本函数设置，不读
    unsigned_integral_and_array_read_from_socket<24>(sock, header.seqnum, header.devid, header.direction, header.ep,
                                                     status);
    header.command = USBIP_RET_UNLINK;
}

usbipdcpp::UsbIpResponse::UsbIpRetUnlink
usbipdcpp::UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(std::uint32_t seqnum, std::uint32_t status) {
    return {.header = UsbIpHeaderBasic::get_server_header(USBIP_RET_UNLINK, seqnum), .status = status};
}

usbipdcpp::UsbIpResponse::UsbIpRetUnlink
usbipdcpp::UsbIpResponse::UsbIpRetUnlink::create_ret_unlink_success(std::uint32_t seqnum) {
    return {.header = UsbIpHeaderBasic::get_server_header(USBIP_RET_UNLINK, seqnum),
            .status = static_cast<std::uint32_t>(UrbStatusType::StatusOK)};
}

array_data_type<calculate_total_size_with_array<decltype(USBIP_VERSION), decltype(OP_REQ_DEVLIST),
                                                decltype(UsbIpCommand::OpReqDevlist::status)>()>
usbipdcpp::UsbIpCommand::OpReqDevlist::to_bytes() const {
    return to_network_array(USBIP_VERSION, OP_REQ_DEVLIST, status);
}

void UsbIpCommand::OpReqDevlist::from_socket(asio::ip::tcp::socket &sock) {
    // status 是客户端请求里的保留字段（协议规定为 0），服务端读后忽略：
    // 不 assert status == 0——恶意客户端可发任意值，debug 构建下 assert 失败
    // 会让服务端崩溃（可被远程触发的崩溃点）
    status = read_u32(sock);
}

array_data_type<calculate_total_size_with_array<decltype(USBIP_VERSION), decltype(OP_REQ_IMPORT),
                                                decltype(UsbIpCommand::OpReqImport::status),
                                                decltype(UsbIpCommand::OpReqImport::busid)>()>
usbipdcpp::UsbIpCommand::OpReqImport::to_bytes() const {
    return to_network_array(USBIP_VERSION, OP_REQ_IMPORT, status, busid);
}

void UsbIpCommand::OpReqImport::to_socket(asio::ip::tcp::socket &sock, error_code &ec) const {
    asio::write(sock, asio::buffer(to_bytes()), ec);
}

void usbipdcpp::UsbIpCommand::OpReqImport::from_socket(asio::ip::tcp::socket &sock) {
    data_read_from_socket(sock, status, busid);
    // 同 OpReqDevlist：不 assert status == 0——status 是请求里的保留字段，
    // 恶意客户端可发任意值，debug 构建下 assert 失败会让服务端崩溃
}

void UsbIpCommand::UsbIpCmdSubmit::to_socket(asio::ip::tcp::socket &sock, error_code &ec) const {
    asio::write(sock,
                asio::buffer(to_network_array(header.to_bytes(), transfer_flags, transfer_buffer_length, start_frame,
                                              number_of_packets, interval, setup.to_bytes())),
                ec);
    if (ec)
        return;
    if (transfer) {
        auto *op = transfer.get_operator();
        void *raw_handle = transfer.get();
        // 数据阶段只在 OUT 方向携带（与 from_socket 的方向判断对称）：
        // IN 恒传 0，防止 IN 的 transfer 缓冲内容被误发
        op->send_transfer_data(raw_handle, sock, op->transfer_is_in(raw_handle) ? 0 : transfer_buffer_length, ec);
    }
}

void UsbIpCommand::UsbIpCmdSubmit::from_socket(asio::ip::tcp::socket &sock) {
    // 使用 scatter-gather 一次性读取固定部分
    // header 字段(16字节) + transfer参数(20字节) + setup(8字节) = 44字节
    decltype(SetupPacket{}.to_bytes()) setup_buffer;
    unsigned_integral_and_array_read_from_socket(sock, header.seqnum, header.devid, header.direction, header.ep,
                                                 transfer_flags, transfer_buffer_length, start_frame, number_of_packets,
                                                 interval, setup_buffer);
    // 设置命令类型
    header.command = USBIP_CMD_SUBMIT;

    // 校验端点号：header.ep 是 32 位，转 uint8_t 会截断，恶意客户端可发
    // 溢出值（如 0x100）截断后映射到错误端点。协议中 ep 是端点号且不带
    // 方向位（方向由 direction 字段给出，见 protocol.h 中 UsbIpHeaderBasic::ep
    // 的注释），合法范围 0-0x7F，超范围按协议错误拒绝
    if (header.ep > 0x7F) [[unlikely]] {
        throw std::system_error(std::make_error_code(std::errc::protocol_error), "invalid endpoint number");
    }

    // 解析 setup packet（小端序）
    setup = SetupPacket::parse(setup_buffer);

    // 检查缓冲区大小，防止恶意大内存分配
    if (transfer_buffer_length > USBIPDCPP_MAX_TRANSFER_BUFFER_SIZE) [[unlikely]] {
        throw std::system_error(std::make_error_code(std::errc::no_buffer_space), "transfer_buffer_length too large");
    }

    // 检查等时包数量，防止恶意巨大分配（如 0x7FFFFFFF 导致 libusb_alloc_transfer
    // 分配失败返回 null）。0xFFFFFFFF 是协议中非等时传输的占位值，必须放行
    if (number_of_packets != 0xFFFFFFFF && number_of_packets > USBIPDCPP_MAX_ISO_PACKETS) [[unlikely]] {
        throw std::system_error(std::make_error_code(std::errc::no_buffer_space), "number_of_packets too large");
    }

    // 从路由 op 拿到对应端点的 leaf op，用它创建 transfer_handle
    // header.ep 是 USB/IP 线格式（不带方向位），需还原真实端点地址再查表
    int num_iso = (number_of_packets != 0 && number_of_packets != 0xFFFFFFFF) ? static_cast<int>(number_of_packets) : 0;
    auto *routing_op = transfer.get_operator();
    std::uint8_t real_ep = static_cast<std::uint8_t>(header.ep);
    // direction 不显式校验（非 0/1 值按 Out 处理）：与内核 usbip 模块和
    // usbipd-libusb 一致（都只判断 == USBIP_DIR_IN），非法值还原出的
    // 端点地址不会命中任何真实端点，后续 find_ep 失败自然拒绝，无内存
    // 安全影响
    if (header.direction == UsbIpDirection::In)
        real_ep |= 0x80;
    auto *leaf_op = routing_op->get_operator_for_ep(real_ep);
    auto *raw_handle = leaf_op->alloc_transfer_handle(transfer_buffer_length, num_iso, header, setup);
    if (!raw_handle) [[unlikely]] {
        // 分配失败（内存耗尽等），抛 std::system_error 由 get_cmd_from_socket 捕获后
        // 优雅断开本会话，而不是空指针解引用崩溃整个进程
        throw std::system_error(std::make_error_code(std::errc::no_buffer_space), "alloc_transfer_handle failed");
    }
    // 将 handle 绑定到 leaf op，后续 I/O 操作直接走 leaf op，无需 map 查找
    transfer.set_handle(raw_handle, leaf_op);

    // 数据传输统一由 recv_transfer_data 处理（数据 + iso 描述符）
    // IN 方向 client 不发送数据，长度传 0
    std::error_code ec;
    leaf_op->recv_transfer_data(raw_handle, sock, header.direction == UsbIpDirection::In ? 0 : transfer_buffer_length,
                                ec);
    if (ec)
        throw std::system_error(ec);
}

array_data_type<calculate_total_size_with_array<decltype(UsbIpHeaderBasic{}.to_bytes()),
                                                decltype(UsbIpCommand::UsbIpCmdUnlink::unlink_seqnum)>() +
                24>
usbipdcpp::UsbIpCommand::UsbIpCmdUnlink::to_bytes() const {
    return array_add_padding<24>(to_network_array(header.to_bytes(), unlink_seqnum));
}

void UsbIpCommand::UsbIpCmdUnlink::to_socket(asio::ip::tcp::socket &sock, error_code &ec) const {
    asio::write(sock, asio::buffer(to_bytes()), ec);
}

void UsbIpCommand::UsbIpCmdUnlink::from_socket(asio::ip::tcp::socket &sock) {
    // 使用 scatter-gather 一次性读取 header 字段 + unlink_seqnum + padding
    unsigned_integral_and_array_read_from_socket<24>(sock, header.seqnum, header.devid, header.direction, header.ep,
                                                     unlink_seqnum);
    // 设置命令类型
    header.command = USBIP_CMD_UNLINK;
}

usbipdcpp::UsbIpCommand::OpCmdVariant usbipdcpp::UsbIpCommand::get_op_from_socket(asio::ip::tcp::socket &sock,
                                                                                  usbipdcpp::error_code &ec) {
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

usbipdcpp::UsbIpCommand::CmdVariant usbipdcpp::UsbIpCommand::get_cmd_from_socket(asio::ip::tcp::socket &sock,
                                                                                 AbstDeviceHandler *handler,
                                                                                 usbipdcpp::error_code &ec) {

    try {
        auto command = read_u32(sock);
        SPDLOG_DEBUG("收到command: 0x{:04x}", command);

        switch (command) {
            case USBIP_CMD_SUBMIT: {
                auto cmd = UsbIpCmdSubmit{};
                // 提前设置operator防止空指针
                cmd.transfer.set_operator(handler->get_transfer_operator());
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
        // 注意：独立 asio 中 asio::system_error 就是 std::system_error 的 typedef，
        // 因此 from_socket 里抛的纯 std::system_error（如 transfer_buffer_length 超限、
        // OUT 数据阶段读失败）也会被这里捕获，不会逃出线程函数导致 std::terminate
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
