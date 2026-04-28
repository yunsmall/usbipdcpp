#include "LibusbHandler/LibusbDeviceHandler.h"


#include "Session.h"
#include "protocol.h"
#include "SetupPacket.h"
#include "constant.h"
#include "Endpoint.h"

using namespace usbipdcpp;

// 普通模式构造函数
usbipdcpp::LibusbDeviceHandler::LibusbDeviceHandler(UsbDevice &handle_device, libusb_device *native_device) :
    AbstDeviceHandler(handle_device), native_device_(native_device) {
    // 设备尚未打开，将在 on_new_connection 时打开
}

// Android 模式构造函数
usbipdcpp::LibusbDeviceHandler::LibusbDeviceHandler(UsbDevice &handle_device, intptr_t fd) :
    AbstDeviceHandler(handle_device), wrapped_fd_(fd) {
    // fd 将在 on_new_connection 时通过 libusb_wrap_sys_device 包装
}

usbipdcpp::LibusbDeviceHandler::~LibusbDeviceHandler() {
    if (native_device_) {
        libusb_unref_device(native_device_);
        native_device_ = nullptr;
    }
}

// ========== transfer_handle 操作实现 ==========

void* usbipdcpp::LibusbDeviceHandler::alloc_transfer_handle(std::size_t buffer_length, int num_iso_packets, const UsbIpHeaderBasic& header, const SetupPacket& setup_packet) {
    auto* trx = libusb_alloc_transfer(num_iso_packets);
    if (!trx) [[unlikely]] {
        return nullptr;
    }

    // 控制传输需要额外的 setup 空间（8 字节）
    std::size_t write_offset = get_write_data_offset(header);
    std::size_t actual_buffer_length = buffer_length + write_offset;

    trx->buffer = static_cast<unsigned char*>(malloc(actual_buffer_length));
    if (!trx->buffer) [[unlikely]] {
        libusb_free_transfer(trx);
        return nullptr;
    }
    trx->length = static_cast<int>(actual_buffer_length);
    return trx;
}

void* usbipdcpp::LibusbDeviceHandler::get_transfer_buffer(void* transfer_handle) {
    auto* trx = static_cast<libusb_transfer*>(transfer_handle);
    return trx->buffer;
}

std::size_t usbipdcpp::LibusbDeviceHandler::get_actual_length(void* transfer_handle) {
    auto* trx = static_cast<libusb_transfer*>(transfer_handle);
    return trx->actual_length;
}

std::size_t usbipdcpp::LibusbDeviceHandler::get_read_data_offset(void* transfer_handle) {
    auto* trx = static_cast<libusb_transfer*>(transfer_handle);
    // 控制传输的数据从 setup 包之后开始
    if (trx->type == LIBUSB_TRANSFER_TYPE_CONTROL) {
        return LIBUSB_CONTROL_SETUP_SIZE;
    }
    return 0;
}

std::size_t usbipdcpp::LibusbDeviceHandler::get_write_data_offset(const UsbIpHeaderBasic& header) {
    // 控制传输 (ep == 0) 需要跳过 setup 包
    if (header.ep == 0) {
        return LIBUSB_CONTROL_SETUP_SIZE;
    }
    return 0;
}

UsbIpIsoPacketDescriptor usbipdcpp::LibusbDeviceHandler::get_iso_descriptor(void* transfer_handle, int index) {
    auto* trx = static_cast<libusb_transfer*>(transfer_handle);
    auto& iso = trx->iso_packet_desc[index];
    return UsbIpIsoPacketDescriptor{
        .offset = 0, // 需要调用方计算
        .length = iso.length,
        .actual_length = iso.actual_length,
        .status = static_cast<std::uint32_t>(trxstat2error(iso.status)),
        .length_in_transfer_buffer_only_for_send = iso.length
    };
}

void usbipdcpp::LibusbDeviceHandler::set_iso_descriptor(void* transfer_handle, int index, const UsbIpIsoPacketDescriptor& desc) {
    auto* trx = static_cast<libusb_transfer*>(transfer_handle);
    auto& iso = trx->iso_packet_desc[index];
    iso.status = error2trxstat(desc.status);
    iso.actual_length = desc.actual_length;
    iso.length = desc.length;
}

void usbipdcpp::LibusbDeviceHandler::free_transfer_handle(void* transfer_handle) {
    auto* trx = static_cast<libusb_transfer*>(transfer_handle);
    free(trx->buffer);
    libusb_free_transfer(trx);
}

void usbipdcpp::LibusbDeviceHandler::on_new_connection(Session &current_session, error_code &ec) {
    AbstDeviceHandler::on_new_connection(current_session, ec);

    if (native_device_) {
        // 普通模式：需要打开设备
        if (!open_and_claim_device()) {
            SPDLOG_ERROR("打开设备失败");
            ec = make_error_code(ErrorType::NO_DEVICE);
            return;
        }
    }
    else if (wrapped_fd_ >= 0) {
        // Android 模式：wrap fd 并声明接口
        if (!wrap_fd_and_claim_interfaces()) {
            SPDLOG_ERROR("wrap fd 失败");
            ec = make_error_code(ErrorType::NO_DEVICE);
            return;
        }
    }

    //标记客户端连接
    client_disconnection = false;
}

void usbipdcpp::LibusbDeviceHandler::on_disconnection(error_code &ec) {
    client_disconnection = true;
    // 不检查 device_removed，因为 libusb 会在设备拔出时正确触发回调（LIBUSB_TRANSFER_NO_DEVICE）

    // 取消所有传输
    auto transfers = transfer_tracker_.get_all_transfers();
    for (auto &info: transfers) {
        auto err = libusb_cancel_transfer(info.transfer);
        if (err) {
            SPDLOG_ERROR("libusb_cancel_transfer failed on seqnum {}: {}", info.seqnum, libusb_strerror(err));
        }
    }

    // 等待所有传输完成
    {
        std::unique_lock lock(transfer_complete_mutex_);
        transfer_complete_cv_.wait(lock, [this]() {
            return transfer_tracker_.concurrent_count() == 0;
        });
    }

    //为下次连接做准备，重置对象池状态
    callback_args_pool_.reset();

    // 断开连接时释放接口并关闭设备
    if (interfaces_claimed_) {
        release_and_close_device();
    }

    AbstDeviceHandler::on_disconnection(ec);
}

void usbipdcpp::LibusbDeviceHandler::receive_urb(
        UsbIpCommand::UsbIpCmdSubmit cmd,
        UsbEndpoint ep,
        std::optional<UsbInterface> interface,
        usbipdcpp::error_code &ec) {

    if (device_removed)[[unlikely]] {
        ec = make_error_code(ErrorType::NO_DEVICE);
        return;
    }

    auto seqnum = cmd.header.seqnum;
    auto transfer_flags = cmd.transfer_flags;
    auto transfer_buffer_length = cmd.transfer_buffer_length;
    const auto &setup_packet = cmd.setup;

    // 根据端点类型分发
    if (ep.attributes == static_cast<std::uint8_t>(EndpointAttributes::Control))[[unlikely]] {
        // 控制传输
        auto tweak_ret = tweak_special_requests(setup_packet);
        if (tweak_ret < 0)[[likely]] {
            // 不需要 tweak，提交 transfer
            SPDLOG_DEBUG("控制传输 {}，ep addr: {:02x}", ep.direction() == UsbEndpoint::Direction::Out?"Out":"In", ep.address);

            auto* trx = static_cast<libusb_transfer*>(cmd.transfer.get());

            // 填充 setup 包到 buffer 开头
            libusb_fill_control_setup(trx->buffer, setup_packet.request_type, setup_packet.request,
                                      setup_packet.value, setup_packet.index, setup_packet.length);

            auto *callback_args = callback_args_pool_.alloc();
            if (!callback_args) [[unlikely]] {
                callback_args = new libusb_callback_args{};
            }
            callback_args->handler = this;
            callback_args->seqnum = seqnum;
            callback_args->is_out = setup_packet.is_out();
            callback_args->transfer = std::move(cmd.transfer);  // 转移所有权

            libusb_fill_control_transfer(trx, native_handle, trx->buffer,
                                         LibusbDeviceHandler::transfer_callback,
                                         callback_args,
                                         timeout_milliseconds);
            trx->flags = get_libusb_transfer_flags(transfer_flags);
            masking_bogus_flags(setup_packet.is_out(), trx);

            transfer_tracker_.register_transfer(seqnum, trx, ep.address);

            LATENCY_TRACK(session->latency_tracker, seqnum,
                          "LibusbDeviceHandler::receive_urb libusb_submit_transfer");
            auto err = libusb_submit_transfer(trx);

            if (err < 0)[[unlikely]] {
                SPDLOG_ERROR("控制传输给设备失败：{}", libusb_strerror(err));
                transfer_tracker_.remove(seqnum);
                callback_args->transfer.reset();
                if (!callback_args_pool_.free(callback_args)) {
                    delete callback_args;
                }
                session->submit_ret_submit(
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                if (err == LIBUSB_ERROR_NO_DEVICE || err == LIBUSB_ERROR_IO)[[unlikely]] {
                    device_removed = true;
                    ec = make_error_code(ErrorType::NO_DEVICE);
                }
            }
        }
        else {
            // tweak 成功或失败，都不提交 transfer
            session->submit_ret_submit(
                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(seqnum, transfer_buffer_length));
        }
    }
    else if (interface.has_value())[[likely]] {
        bool is_out = !ep.is_in();

        auto* trx = static_cast<libusb_transfer*>(cmd.transfer.get());

        auto *callback_args = callback_args_pool_.alloc();
        if (!callback_args) [[unlikely]] {
            callback_args = new libusb_callback_args{};
        }
        callback_args->handler = this;
        callback_args->seqnum = seqnum;
        callback_args->is_out = is_out;
        callback_args->transfer = std::move(cmd.transfer);  // 转移所有权

        if (ep.attributes == static_cast<std::uint8_t>(EndpointAttributes::Bulk))[[likely]] {
            LATENCY_TRACK(session->latency_tracker, seqnum, "LibusbDeviceHandler::receive_urb bulk");

            libusb_fill_bulk_transfer(trx, native_handle, ep.address, trx->buffer,
                                      transfer_buffer_length,
                                      LibusbDeviceHandler::transfer_callback,
                                      callback_args,
                                      timeout_milliseconds);
        }
        else if (ep.attributes == static_cast<std::uint8_t>(EndpointAttributes::Interrupt)) {
            SPDLOG_DEBUG("中断传输 {}，ep addr: {:02x}", ep.direction() == UsbEndpoint::Direction::Out?"Out":"In", ep.address);

            libusb_fill_interrupt_transfer(trx, native_handle, ep.address, trx->buffer,
                                           transfer_buffer_length,
                                           LibusbDeviceHandler::transfer_callback,
                                           callback_args,
                                           timeout_milliseconds);
        }
        else if (ep.attributes == static_cast<std::uint8_t>(EndpointAttributes::Isochronous)) {
            int num_iso_packets = (cmd.number_of_packets != 0 && cmd.number_of_packets != 0xFFFFFFFF)
                                  ? static_cast<int>(cmd.number_of_packets) : 0;
            SPDLOG_DEBUG("同步传输 {}，ep addr: {:02x}", ep.direction() == UsbEndpoint::Direction::Out?"Out":"In", ep.address);

            libusb_fill_iso_transfer(
                    trx, native_handle, ep.address, trx->buffer, transfer_buffer_length,
                    num_iso_packets,
                    LibusbDeviceHandler::transfer_callback, callback_args, timeout_milliseconds);
        }
        else [[unlikely]] {
            SPDLOG_DEBUG("端口{:02x}的未知传输类型：{}", ep.address, ep.attributes);
            callback_args->transfer.reset();
            if (!callback_args_pool_.free(callback_args)) {
                delete callback_args;
            }
            session->submit_ret_submit(
                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
            return;
        }

        trx->flags = get_libusb_transfer_flags(transfer_flags);
        masking_bogus_flags(is_out, trx);

        transfer_tracker_.register_transfer(seqnum, trx, ep.address);

        auto err = libusb_submit_transfer(trx);
        if (err < 0)[[unlikely]] {
            SPDLOG_ERROR("传输失败，{}", libusb_strerror(err));
            transfer_tracker_.remove(seqnum);
            callback_args->transfer.reset();
            if (!callback_args_pool_.free(callback_args)) {
                delete callback_args;
            }
            if (err == LIBUSB_ERROR_NO_DEVICE || err == LIBUSB_ERROR_IO)[[unlikely]] {
                device_removed = true;
                ec = make_error_code(ErrorType::NO_DEVICE);
            }
            session->submit_ret_submit(
                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        }
    }
    else[[unlikely]] {
        SPDLOG_ERROR("非控制传输却不存在目标接口");
        session->submit_ret_submit(
                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
    }
}

void usbipdcpp::LibusbDeviceHandler::handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) {
    if (device_removed)[[unlikely]]
            return;

    libusb_transfer *transfer_to_cancel = nullptr;

    transfer_tracker_.with_transfer(unlink_seqnum, [&](auto *info) {
        if (info) {
            // transfer 还在进行中，在锁保护下标记为 unlinking
            info->is_unlinked = true;
            info->unlink_cmd_seqnum = cmd_seqnum;
            transfer_to_cancel = info->transfer;
        }
    });

    if (transfer_to_cancel)[[likely]] {
        int err = libusb_cancel_transfer(transfer_to_cancel);
        if (err == LIBUSB_ERROR_NOT_FOUND)[[unlikely]]{
        }
        else if (err)[[unlikely]] {
            SPDLOG_ERROR("libusb_cancel_transfer failed: {}", libusb_strerror(err));
        }
    }
    else {
        // transfer 已经完成，立即发送 ret_unlink
        SPDLOG_DEBUG("transfer {} 已完成，立即发送 ret_unlink {}", unlink_seqnum, cmd_seqnum);
        session->submit_ret_unlink(
                UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(cmd_seqnum, 0)
                );
    }
}

int usbipdcpp::LibusbDeviceHandler::tweak_clear_halt_cmd(const SetupPacket &setup_packet) {
    auto target_endp = setup_packet.index;
    SPDLOG_DEBUG("tweak_clear_halt_cmd");

    auto err = libusb_clear_halt(native_handle, target_endp);
    if (err)[[unlikely]] {
        SPDLOG_ERROR("libusb_clear_halt() error: endp {} returned {}", target_endp, libusb_strerror(err));
    }
    else {
        SPDLOG_DEBUG("libusb_clear_halt() done: endp {}", target_endp);
    }
    return err; // 返回 0 表示成功，正数表示错误
}

int usbipdcpp::LibusbDeviceHandler::tweak_set_interface_cmd(const SetupPacket &setup_packet) {
    uint16_t alternate = setup_packet.value;
    uint16_t interface = setup_packet.index;

    SPDLOG_DEBUG("set_interface: inf {} alt {}",
                 interface, alternate);
    int err = libusb_set_interface_alt_setting(native_handle, interface, alternate);
    if (err)[[unlikely]] {
        SPDLOG_ERROR(
                "{}: usb_set_interface error: inf {} alt {} err {}",
                get_device_busid(libusb_get_device(native_handle)),
                interface, alternate, libusb_strerror(err));
    }
    else {
        SPDLOG_DEBUG(
                "{}: usb_set_interface done: inf {} alt {}",
                get_device_busid(libusb_get_device(native_handle)),
                interface, alternate);
    }
    return err; // 返回 0 表示成功，正数表示错误
}

int usbipdcpp::LibusbDeviceHandler::tweak_set_configuration_cmd(const SetupPacket &setup_packet) {
    SPDLOG_DEBUG("tweak_set_configuration_cmd");

    // uint16_t config = libusb_le16_to_cpu(setup_packet.value);

    // auto err = libusb_set_configuration(native_handle, config);
    // if (err) {
    //     SPDLOG_ERROR(
    //             "{}: libusb_set_configuration error: config {} ret {}",
    //             get_device_busid(libusb_get_device(native_handle)), config, libusb_strerror(err));
    // }
    // else {
    //     SPDLOG_DEBUG(
    //             "{}: libusb_set_configuration done: config {}",
    //             get_device_busid(libusb_get_device(native_handle)), config);
    // }
    // return err;

    //不可以set_configuration，会device_busy
    // usbipd-libusb 返回 -1，表示不处理这个命令，继续正常提交 transfer
    // 设备会收到 set_configuration 命令
    return -1;
}

int usbipdcpp::LibusbDeviceHandler::tweak_reset_device_cmd(const SetupPacket &setup_packet) {
    SPDLOG_DEBUG("{}: usb_queue_reset_device",
                 get_device_busid(libusb_get_device(native_handle)));

    // 参考 usbipd-libusb：不执行 libusb_reset_device
    // reset 可能导致设备重新枚举，连接会断开
    // libusb_reset_device(native_handle);
    return 0;
}

int usbipdcpp::LibusbDeviceHandler::tweak_special_requests(const SetupPacket &setup_packet) {
    // 返回值：
    // -1: 不需要 tweak，应该提交 transfer
    //  0: tweak 成功，不需要提交 transfer
    // >0: tweak 失败（libusb 错误码），不需要提交 transfer
    // 特殊请求较少见，大多数情况返回 -1（不需要 tweak）
    if (setup_packet.is_clear_halt_cmd())[[unlikely]] {
        return tweak_clear_halt_cmd(setup_packet);
    }
    else if (setup_packet.is_set_interface_cmd())[[unlikely]] {
        return tweak_set_interface_cmd(setup_packet);
    }
    else if (setup_packet.is_set_configuration_cmd())[[unlikely]] {
        return tweak_set_configuration_cmd(setup_packet);
    }
    else if (setup_packet.is_reset_device_cmd())[[unlikely]] {
        return tweak_reset_device_cmd(setup_packet);
    }
    SPDLOG_DEBUG("不需要调整包");
    return -1; // 不需要 tweak
}

uint8_t usbipdcpp::LibusbDeviceHandler::get_libusb_transfer_flags(uint32_t in) {
    uint8_t flags = 0;

    if (in & static_cast<std::uint32_t>(TransferFlag::URB_SHORT_NOT_OK))
        flags |= LIBUSB_TRANSFER_SHORT_NOT_OK;
    if (in & static_cast<std::uint32_t>(TransferFlag::URB_ZERO_PACKET))
        flags |= LIBUSB_TRANSFER_ADD_ZERO_PACKET;

    return flags;
}

void usbipdcpp::LibusbDeviceHandler::masking_bogus_flags(bool is_out, struct libusb_transfer *trx) {
    std::uint32_t allowed = 0;
    /* enforce simple/standard policy */
    switch (trx->type) {
        case LIBUSB_TRANSFER_TYPE_BULK:
            if (is_out)
                allowed |= LIBUSB_TRANSFER_ADD_ZERO_PACKET;
        /* FALLTHROUGH */
        case LIBUSB_TRANSFER_TYPE_CONTROL:
            /*allowed |= URB_NO_FSBR; */ /* only affects UHCI */
            /* FALLTHROUGH */
        default: /* all non-iso endpoints */
            if (!is_out)
                allowed |= LIBUSB_TRANSFER_SHORT_NOT_OK;
            break;
    }
    trx->flags &= allowed;
}

int usbipdcpp::LibusbDeviceHandler::trxstat2error(enum libusb_transfer_status trxstat) {
    //具体数值抄的linux的
    switch (trxstat) {
        case LIBUSB_TRANSFER_COMPLETED:
            return static_cast<int>(UrbStatusType::StatusOK);
        case LIBUSB_TRANSFER_CANCELLED:
            return static_cast<int>(UrbStatusType::StatusECONNRESET);
        case LIBUSB_TRANSFER_ERROR:
        case LIBUSB_TRANSFER_STALL:
        case LIBUSB_TRANSFER_TIMED_OUT:
        case LIBUSB_TRANSFER_OVERFLOW:
            return static_cast<int>(UrbStatusType::StatusEPIPE);
        case LIBUSB_TRANSFER_NO_DEVICE:
            return static_cast<int>(UrbStatusType::StatusESHUTDOWN);
        default:
            return static_cast<int>(UrbStatusType::StatusENOENT);
    }
}


enum libusb_transfer_status usbipdcpp::LibusbDeviceHandler::error2trxstat(int e) {
    switch (e) {
        case static_cast<int>(UrbStatusType::StatusOK):
            return LIBUSB_TRANSFER_COMPLETED;
        case static_cast<int>(UrbStatusType::StatusENOENT):
            return LIBUSB_TRANSFER_ERROR;
        case static_cast<int>(UrbStatusType::StatusECONNRESET):
            return LIBUSB_TRANSFER_CANCELLED;
        case static_cast<int>(UrbStatusType::StatusETIMEDOUT):
            return LIBUSB_TRANSFER_TIMED_OUT;
        case static_cast<int>(UrbStatusType::StatusEPIPE):
            return LIBUSB_TRANSFER_STALL;
        case static_cast<int>(UrbStatusType::StatusESHUTDOWN):
            return LIBUSB_TRANSFER_NO_DEVICE;
        case static_cast<int>(UrbStatusType::StatusEEOVERFLOW): //EOVERFLOW
            return LIBUSB_TRANSFER_OVERFLOW;
        default:
            return LIBUSB_TRANSFER_ERROR;
    }
}

void usbipdcpp::LibusbDeviceHandler::transfer_callback(libusb_transfer *trx) {
    auto &callback_arg = *static_cast<libusb_callback_args *>(trx->user_data);

    LATENCY_TRACK(callback_arg.handler->session->latency_tracker, callback_arg.seqnum,
                  "LibusbDeviceHandler::transfer_callback调用");

    // 在锁保护下检查 is_unlinked 并从 tracker 移除
    bool is_unlinked = false;
    std::uint32_t unlink_cmd_seqnum = 0;

    callback_arg.handler->transfer_tracker_.check_and_remove(callback_arg.seqnum, [&](auto *info) {
        if (info) {
            is_unlinked = info->is_unlinked;
            unlink_cmd_seqnum = info->unlink_cmd_seqnum;
            return true; // 移除
        }
        return false;
    });

    // 如果断连了，直接清理并返回（不发送响应）
    // 这是在传输完成后检查，断连是特殊情况
    if (callback_arg.handler->client_disconnection)[[unlikely]] {
        if (callback_arg.handler->transfer_tracker_.concurrent_count() == 0) {
            std::lock_guard lock(callback_arg.handler->transfer_complete_mutex_);
            callback_arg.handler->transfer_complete_cv_.notify_one();
        }
        // TransferHandle 析构时自动释放
        callback_arg.transfer.reset();  // 提前释放，避免 pool 问题
        if (!callback_arg.handler->callback_args_pool_.free(&callback_arg)) {
            delete &callback_arg;
        }
        return;
    }

    // status 检查
    switch (trx->status) {
        case LIBUSB_TRANSFER_COMPLETED:
            /* OK */
            break;
        case LIBUSB_TRANSFER_ERROR:
            if (!(trx->flags & LIBUSB_TRANSFER_SHORT_NOT_OK)) {
                dev_err(libusb_get_device(trx->dev_handle),
                        "error on endpoint {}", trx->endpoint);
            }
            else {
                //Tweaking status to complete as we received data, but all
                trx->status = LIBUSB_TRANSFER_COMPLETED;
            }
            break;
        case LIBUSB_TRANSFER_CANCELLED:
            dev_info(libusb_get_device(trx->dev_handle),
                     "unlinked by a call to usb_unlink_urb()");
            break;
        case LIBUSB_TRANSFER_STALL:
            dev_err(libusb_get_device(trx->dev_handle),
                    "endpoint {} is stalled", trx->endpoint);
            break;
        case LIBUSB_TRANSFER_NO_DEVICE:
            dev_info(libusb_get_device(trx->dev_handle), "device removed?");
            callback_arg.handler->device_removed = true;
            break;
        default:
            dev_warn(libusb_get_device(trx->dev_handle),
                     "urb completion with unknown status {}",
                     static_cast<int>(trx->status));
            break;
    }
    SPDLOG_DEBUG("libusb传输了{}个字节", trx->actual_length);

    // 计算 ISO 传输的实际长度
    std::uint32_t actual_length = trx->actual_length;
    if (trx->type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS && !callback_arg.is_out) {
        // ISO IN 传输：需要计算所有 iso packet 的实际长度之和
        size_t iso_actual_length = 0;
        for (int i = 0; i < trx->num_iso_packets; i++) {
            iso_actual_length += trx->iso_packet_desc[i].actual_length;
        }
        actual_length = static_cast<std::uint32_t>(iso_actual_length);
    }

    if (!is_unlinked)[[likely]] {
        // 发送 ret_submit
        // OUT 传输不需要发送数据回客户端，只发送 header（无 transfer_handle）
        // IN 传输需要发送 header + 数据（有 transfer_handle）
        UsbIpResponse::UsbIpRetSubmit ret;
        if (callback_arg.is_out) {
            // OUT 传输：无数据阶段
            ret = UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(
                    callback_arg.seqnum,
                    trxstat2error(trx->status),
                    actual_length
                    );
            // OUT 传输：释放 transfer
            callback_arg.transfer.reset();
        }
        else {
            // IN 传输：有数据，转移所有权
            ret = UsbIpResponse::UsbIpRetSubmit::create_ret_submit(
                    callback_arg.seqnum,
                    trxstat2error(trx->status),
                    actual_length,
                    0,  // start_frame
                    trx->num_iso_packets,
                    std::move(callback_arg.transfer)  // 转移所有权
                    );
        }

        SPDLOG_DEBUG("libusb传输actual_length为{}个字节", actual_length);

        LATENCY_TRACK(callback_arg.handler->session->latency_tracker, callback_arg.seqnum,
                      "LibusbDeviceHandler::transfer_callback submit_ret_submit");
        callback_arg.handler->session->submit_ret_submit(std::move(ret));
    }
    else {
        // unlink 情况：发送 ret_unlink
        LATENCY_TRACK_END_MSG(callback_arg.handler->session->latency_tracker, unlink_cmd_seqnum,
                              "被unlink");

        callback_arg.handler->session->submit_ret_unlink(
                UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(
                        unlink_cmd_seqnum,
                        trxstat2error(trx->status)
                        )
                );
        // unlink 情况：释放 transfer
        callback_arg.transfer.reset();
    }

    // 释放 callback_arg
    // TransferHandle 已被转移或重置，可以安全归还到池
    if (!callback_arg.handler->callback_args_pool_.free(&callback_arg)) {
        delete &callback_arg;
    }
}

bool usbipdcpp::LibusbDeviceHandler::open_and_claim_device() {
    // 此函数仅用于普通模式
    if (!native_device_) {
        SPDLOG_ERROR("native_device_ 为空，无法打开设备");
        return false;
    }

    int err = libusb_open(native_device_, &native_handle);
    if (err) {
        SPDLOG_ERROR("无法打开设备: {}", libusb_strerror(err));
        return false;
    }

    // 获取配置描述符
    struct libusb_config_descriptor *active_config_desc = nullptr;
    err = libusb_get_active_config_descriptor(native_device_, &active_config_desc);
    if (err) {
        SPDLOG_ERROR("无法获取配置描述符: {}", libusb_strerror(err));
        libusb_close(native_handle);
        native_handle = nullptr;
        return false;
    }

    int num_interfaces = active_config_desc->bNumInterfaces;
    SPDLOG_DEBUG("设备有 {} 个接口", num_interfaces);

    // 解绑内核驱动并声明所有接口
    for (int intf_i = 0; intf_i < num_interfaces; intf_i++) {
        err = libusb_detach_kernel_driver(native_handle, intf_i);
        if (err && err != LIBUSB_ERROR_NOT_FOUND) {
            SPDLOG_WARN("无法卸载接口 {} 的内核驱动: {}", intf_i, libusb_strerror(err));
        }

        err = libusb_claim_interface(native_handle, intf_i);
        if (err) {
            SPDLOG_ERROR("无法声明接口 {}: {}", intf_i, libusb_strerror(err));
            // 回滚已声明的接口
            for (int j = 0; j < intf_i; j++) {
                libusb_release_interface(native_handle, j);
                libusb_attach_kernel_driver(native_handle, j);
            }
            libusb_free_config_descriptor(active_config_desc);
            libusb_close(native_handle);
            native_handle = nullptr;
            return false;
        }
    }

    libusb_free_config_descriptor(active_config_desc);
    interfaces_claimed_ = true;
    SPDLOG_INFO("成功打开设备并声明 {} 个接口", num_interfaces);
    return true;
}

bool usbipdcpp::LibusbDeviceHandler::wrap_fd_and_claim_interfaces() {
    // 此函数仅用于 Android 模式
    if (wrapped_fd_ < 0) {
        SPDLOG_ERROR("wrapped_fd_ 无效");
        return false;
    }

    int err = libusb_wrap_sys_device(nullptr, wrapped_fd_, &native_handle);
    if (err) {
        SPDLOG_ERROR("libusb_wrap_sys_device 失败: {}", libusb_strerror(err));
        return false;
    }

    // 注意：wrap 得到的 device 在 libusb_close 后会被销毁
    // 因此每次连接都需要重新 wrap
    libusb_device *wrapped_device = libusb_get_device(native_handle);

    // 获取配置描述符
    struct libusb_config_descriptor *active_config_desc = nullptr;
    err = libusb_get_active_config_descriptor(wrapped_device, &active_config_desc);
    if (err) {
        SPDLOG_ERROR("无法获取配置描述符: {}", libusb_strerror(err));
        libusb_close(native_handle);
        native_handle = nullptr;
        return false;
    }

    int num_interfaces = active_config_desc->bNumInterfaces;
    SPDLOG_DEBUG("设备有 {} 个接口", num_interfaces);

    // 声明所有接口（Android 模式下通常不需要 detach kernel driver）
    for (int intf_i = 0; intf_i < num_interfaces; intf_i++) {
        err = libusb_claim_interface(native_handle, intf_i);
        if (err) {
            SPDLOG_ERROR("无法声明接口 {}: {}", intf_i, libusb_strerror(err));
            // 回滚已声明的接口
            for (int j = 0; j < intf_i; j++) {
                libusb_release_interface(native_handle, j);
            }
            libusb_free_config_descriptor(active_config_desc);
            libusb_close(native_handle);
            native_handle = nullptr;
            return false;
        }
    }

    libusb_free_config_descriptor(active_config_desc);
    interfaces_claimed_ = true;
    SPDLOG_INFO("成功 wrap fd 并声明 {} 个接口", num_interfaces);
    return true;
}

void usbipdcpp::LibusbDeviceHandler::release_and_close_device() {
    if (!native_handle) {
        return;
    }

    // 获取设备以查询接口数量
    libusb_device *device = libusb_get_device(native_handle);
    struct libusb_config_descriptor *active_config_desc = nullptr;
    int num_interfaces = 0;
    if (device && libusb_get_active_config_descriptor(device, &active_config_desc) == 0) {
        num_interfaces = active_config_desc->bNumInterfaces;
        libusb_free_config_descriptor(active_config_desc);
    }

    // 释放所有接口
    for (int intf_i = 0; intf_i < num_interfaces; intf_i++) {
        int err = libusb_release_interface(native_handle, intf_i);
        if (err) {
            SPDLOG_ERROR("释放接口 {} 时出错: {}", intf_i, libusb_strerror(err));
        }

        // 重新绑定内核驱动
        err = libusb_attach_kernel_driver(native_handle, intf_i);
        if (err && err != LIBUSB_ERROR_NOT_FOUND && err != LIBUSB_ERROR_NOT_SUPPORTED) {
            SPDLOG_WARN("重新绑定内核驱动失败 (接口 {}): {}", intf_i, libusb_strerror(err));
        }
    }

    interfaces_claimed_ = false;

    // 关闭 handle
    // 普通模式和 Android 模式都需要调用 libusb_close
    libusb_close(native_handle);
    native_handle = nullptr;

    SPDLOG_INFO("已释放设备接口");
}
