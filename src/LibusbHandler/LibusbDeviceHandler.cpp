#include "LibusbHandler/LibusbDeviceHandler.h"


#include "Session.h"
#include "protocol.h"
#include "SetupPacket.h"
#include "constant.h"
#include "Endpoint.h"

// 普通模式构造函数
usbipdcpp::LibusbDeviceHandler::LibusbDeviceHandler(UsbDevice &handle_device, libusb_device *native_device) :
    DeviceHandlerBase(handle_device), native_device_(native_device) {
    // 设备尚未打开，将在 on_new_connection 时打开
}

// Android 模式构造函数
usbipdcpp::LibusbDeviceHandler::LibusbDeviceHandler(UsbDevice &handle_device, intptr_t fd) :
    DeviceHandlerBase(handle_device), wrapped_fd_(fd) {
    // fd 将在 on_new_connection 时通过 libusb_wrap_sys_device 包装
}

usbipdcpp::LibusbDeviceHandler::~LibusbDeviceHandler() {
    if (native_device_) {
        libusb_unref_device(native_device_);
        native_device_ = nullptr;
    }
}

void usbipdcpp::LibusbDeviceHandler::on_new_connection(Session &current_session, error_code &ec) {
    DeviceHandlerBase::on_new_connection(current_session, ec);

    if (native_device_) {
        // 普通模式：需要打开设备
        if (!open_and_claim_device()) {
            SPDLOG_ERROR("打开设备失败");
            ec = make_error_code(ErrorType::NO_DEVICE);
            return;
        }
    } else if (wrapped_fd_ >= 0) {
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

    //为下次连接做准备，清空自身状态
    callback_args_pool_.clear();

    // 断开连接时释放接口并关闭设备
    if (interfaces_claimed_) {
        release_and_close_device();
    }

    DeviceHandlerBase::on_disconnection(ec);
}

void usbipdcpp::LibusbDeviceHandler::handle_unlink_seqnum(std::uint32_t seqnum) {
    if (device_removed)[[unlikely]]
            return;

    auto info = transfer_tracker_.get(seqnum);
    if (info.has_value()) {
        int err = libusb_cancel_transfer(info->transfer);
        if (err)[[unlikely]] {
            SPDLOG_ERROR("libusb_cancel_transfer failed: {}", libusb_strerror(err));
        }
    }
    else {
        // transfer 已经完成（不在 transfer_tracker_ 中），需要立即发送 status=0 的 ret_unlink
        //
        // 时序说明（参考 usbipd-libusb stub_tx_loop 注释）：
        // 如果 transfer 完成早于收到 CMD_UNLINK，transfer_callback 已经发送了 ret_submit，
        // 此时 handle_unlink_seqnum 在 transfer_tracker_ 中找不到目标。
        // 这种情况下需要立即发送 status=0 的 ret_unlink。
        // 客户端会先收到 ret_submit，再收到 ret_unlink，vhci 驱动会理解这是 unlink 太晚了，
        // 忽略 ret_unlink，使用 ret_submit 的结果。这种"两个都发"的行为是正确的。
        auto unlink_seqnum_opt = session->get_unlink_seqnum(seqnum);
        if (std::get<0>(unlink_seqnum_opt))[[likely]] {
            auto cmd_unlink_seqnum = std::get<1>(unlink_seqnum_opt);
            SPDLOG_DEBUG("transfer {} 已完成，立即发送 ret_unlink {}", seqnum, cmd_unlink_seqnum);
            session->submit_ret_unlink_and_then_remove_seqnum_unlink(
                    UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(cmd_unlink_seqnum, 0),
                    seqnum
                    );
        }
    }
}

void usbipdcpp::LibusbDeviceHandler::handle_control_urb(
        std::uint32_t seqnum,
        const UsbEndpoint &ep,
        std::uint32_t transfer_flags,
        std::uint32_t transfer_buffer_length,
        const SetupPacket &setup_packet, data_type &&transfer_data,
        [[maybe_unused]] std::error_code &ec) {
    // SPDLOG_TRACE("transfer_buffer_length:{},req.size():{}", transfer_buffer_length, req.size());

    if (device_removed)[[unlikely]] {
        ec = make_error_code(ErrorType::NO_DEVICE);
        return;
    }

    // spdlog::debug("控制传输");

    auto tweak_ret = tweak_special_requests(setup_packet);
    if (tweak_ret < 0)[[likely]] {
        // 不需要 tweak，提交 transfer
        SPDLOG_DEBUG("控制传输 {}，ep addr: {:02x}", ep.direction() == UsbEndpoint::Direction::Out?"Out":"In", ep.address);

        auto transfer = libusb_alloc_transfer(0);
        if (!transfer)[[unlikely]] {
            SPDLOG_ERROR("libusb_alloc_transfer 失败");
            session->submit_ret_submit(
                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum));
            return;
        }


        data_type transfer_buffer(LIBUSB_CONTROL_SETUP_SIZE + transfer_buffer_length, 0);
        if (setup_packet.is_out()) {
            memcpy(transfer_buffer.data() + LIBUSB_CONTROL_SETUP_SIZE, transfer_data.data(), transfer_data.size());
        }
        libusb_fill_control_setup(transfer_buffer.data(), setup_packet.request_type, setup_packet.request,
                                  setup_packet.value,
                                  setup_packet.index, setup_packet.length);

        auto *callback_args = callback_args_pool_.alloc();
        if (!callback_args) [[unlikely]] {
            callback_args = new libusb_callback_args{};
        }
        callback_args->handler = this;
        callback_args->seqnum = seqnum;
        callback_args->is_out = setup_packet.is_out();
        callback_args->transfer_buffer = std::move(transfer_buffer);

        libusb_fill_control_transfer(transfer, native_handle, callback_args->transfer_buffer.data(),
                                     LibusbDeviceHandler::transfer_callback,
                                     callback_args,
                                     timeout_milliseconds);
        // libusb控制传输长度需要为setup包和数据包结合的长度
        transfer->length = LIBUSB_CONTROL_SETUP_SIZE + transfer_buffer_length;
        //将usbio transder flag转换成libusb的flags
        transfer->flags = get_libusb_transfer_flags(transfer_flags);
        masking_bogus_flags(setup_packet.is_out(), transfer);

        transfer_tracker_.register_transfer(seqnum, transfer, ep.address);

        LATENCY_TRACK(session->latency_tracker,seqnum,"LibusbDeviceHandler::handle_control_urb libusb_submit_transfer");
        auto err = libusb_submit_transfer(transfer);

        if (err < 0)[[unlikely]] {
            SPDLOG_ERROR("控制传输给设备失败：{}", libusb_strerror(err));
            transfer_tracker_.remove(seqnum);
            callback_args->transfer_buffer.clear();
            callback_args->transfer_buffer.shrink_to_fit();
            if (!callback_args_pool_.free(callback_args)) {
                delete callback_args;
            }
            libusb_free_transfer(transfer);
            session->submit_ret_submit(
                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum));
            if (err == LIBUSB_ERROR_NO_DEVICE)[[unlikely]] {
                device_removed = true;
                ec = make_error_code(ErrorType::NO_DEVICE);
            }
        }
        return;
    }
    // tweak 成功或失败，都不提交 transfer，发送成功响应
    // 与 usbipd-libusb 行为一致：特殊命令无论成功失败都返回成功
    session->submit_ret_submit(
            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_ok_without_data(seqnum));
}

void usbipdcpp::LibusbDeviceHandler::handle_bulk_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                          UsbInterface &interface,
                                                          std::uint32_t transfer_flags,
                                                          std::uint32_t transfer_buffer_length,
                                                          data_type &&transfer_data,
                                                          [[maybe_unused]] std::error_code &ec) {
    if (device_removed)[[unlikely]] {
        ec = make_error_code(ErrorType::NO_DEVICE);
        return;
    }
    bool is_out = !ep.is_in();

    LATENCY_TRACK(session->latency_tracker,seqnum,"LibusbDeviceHandler::handle_bulk_transfer进来");

    SPDLOG_DEBUG("块传输 {}，ep addr: {:02x}", is_out?"Out":"In", ep.address);
    auto transfer = libusb_alloc_transfer(0);
    data_type transfer_buffer(std::move(transfer_data));

    auto *callback_args = callback_args_pool_.alloc();
    if (!callback_args) [[unlikely]] {
        callback_args = new libusb_callback_args{};
    }
    callback_args->handler = this;
    callback_args->seqnum = seqnum;
    callback_args->is_out = is_out;
    callback_args->transfer_buffer = std::move(transfer_buffer);

    libusb_fill_bulk_transfer(transfer, native_handle, ep.address, callback_args->transfer_buffer.data(),
                              transfer_buffer_length,
                              LibusbDeviceHandler::transfer_callback,
                              callback_args,
                              timeout_milliseconds);
    transfer->flags = get_libusb_transfer_flags(transfer_flags);
    masking_bogus_flags(is_out, transfer);

    transfer_tracker_.register_transfer(seqnum, transfer, ep.address);

    LATENCY_TRACK(session->latency_tracker,seqnum,"LibusbDeviceHandler::handle_bulk_transfer libusb_submit_transfer");
    auto err = libusb_submit_transfer(transfer);
    if (err < 0)[[unlikely]] {
        SPDLOG_ERROR("块传输失败，{}", libusb_strerror(err));
        transfer_tracker_.remove(seqnum);
        callback_args->transfer_buffer.clear();
        callback_args->transfer_buffer.shrink_to_fit();
        if (!callback_args_pool_.free(callback_args)) {
            delete callback_args;
        }
        libusb_free_transfer(transfer);
        if (err == LIBUSB_ERROR_NO_DEVICE)[[unlikely]] {
            device_removed = true;
            ec = make_error_code(ErrorType::NO_DEVICE);
        }
        session->submit_ret_submit(
                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum));
    }
}

void usbipdcpp::LibusbDeviceHandler::handle_interrupt_transfer(std::uint32_t seqnum,
                                                               const UsbEndpoint &ep,
                                                               UsbInterface &interface,
                                                               std::uint32_t transfer_flags,
                                                               std::uint32_t transfer_buffer_length,
                                                               data_type &&transfer_data,
                                                               [[maybe_unused]] std::error_code &ec) {
    if (device_removed)[[unlikely]] {
        ec = make_error_code(ErrorType::NO_DEVICE);
        return;
    }
    bool is_out = !ep.is_in();

    SPDLOG_DEBUG("中断传输 {}，ep addr: {:02x}", ep.direction() == UsbEndpoint::Direction::Out?"Out":"In", ep.address);
    auto transfer = libusb_alloc_transfer(0);
    data_type transfer_buffer(std::move(transfer_data));
    if (is_out) {
        // SPDLOG_DEBUG("transfer_buffer_length:{}, out_data.size():{}", transfer_buffer_length, out_data.size());
        assert(transfer_buffer_length==transfer_data.size());
    }

    auto *callback_args = callback_args_pool_.alloc();
    if (!callback_args) [[unlikely]] {
        callback_args = new libusb_callback_args{};
    }
    callback_args->handler = this;
    callback_args->seqnum = seqnum;
    callback_args->is_out = is_out;
    callback_args->transfer_buffer = std::move(transfer_buffer);

    libusb_fill_bulk_transfer(transfer, native_handle, ep.address, callback_args->transfer_buffer.data(),
                              transfer_buffer_length,
                              LibusbDeviceHandler::transfer_callback,
                              callback_args,
                              timeout_milliseconds);
    transfer->flags = get_libusb_transfer_flags(transfer_flags);
    masking_bogus_flags(is_out, transfer);

    transfer_tracker_.register_transfer(seqnum, transfer, ep.address);

    auto err = libusb_submit_transfer(transfer);
    if (err < 0)[[unlikely]] {
        SPDLOG_ERROR("中断传输失败，{}", libusb_strerror(err));
        transfer_tracker_.remove(seqnum);
        callback_args->transfer_buffer.clear();
        callback_args->transfer_buffer.shrink_to_fit();
        if (!callback_args_pool_.free(callback_args)) {
            delete callback_args;
        }
        libusb_free_transfer(transfer);
        if (err == LIBUSB_ERROR_NO_DEVICE)[[unlikely]] {
            device_removed = true;
            ec = make_error_code(ErrorType::NO_DEVICE);
        }
        session->submit_ret_submit(
                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum));
    }
}


void usbipdcpp::LibusbDeviceHandler::handle_isochronous_transfer(
        std::uint32_t seqnum,
        const UsbEndpoint &ep,
        UsbInterface &interface,
        std::uint32_t transfer_flags,
        std::uint32_t transfer_buffer_length,
        data_type &&transfer_data,
        const std::vector<UsbIpIsoPacketDescriptor> &
        iso_packet_descriptors,
        [[maybe_unused]] std::error_code &ec) {
    if (device_removed)[[unlikely]] {
        ec = make_error_code(ErrorType::NO_DEVICE);
        return;
    }

    bool is_out = !ep.is_in();
    SPDLOG_DEBUG("同步传输 {}，ep addr: {:02x}", ep.direction() == UsbEndpoint::Direction::Out?"Out":"In", ep.address);

    auto num_iso_packets = static_cast<int>(iso_packet_descriptors.size());
    auto transfer = libusb_alloc_transfer(num_iso_packets);
    data_type transfer_buffer(std::move(transfer_data));

    auto *callback_args = callback_args_pool_.alloc();
    if (!callback_args) [[unlikely]] {
        callback_args = new libusb_callback_args{};
    }
    callback_args->handler = this;
    callback_args->seqnum = seqnum;
    callback_args->is_out = is_out;
    callback_args->transfer_buffer = std::move(transfer_buffer);

    libusb_fill_iso_transfer(
            transfer, native_handle, ep.address, callback_args->transfer_buffer.data(), transfer_buffer_length,
            num_iso_packets,
            LibusbDeviceHandler::transfer_callback, callback_args, timeout_milliseconds);

    for (std::size_t i = 0; i < iso_packet_descriptors.size(); i++) {
        auto &libusb_iso_desc_i = transfer->iso_packet_desc[i];
        /* ignore iso->offset; */
        libusb_iso_desc_i.status = error2trxstat(iso_packet_descriptors[i].status);
        libusb_iso_desc_i.actual_length = iso_packet_descriptors[i].actual_length;
        libusb_iso_desc_i.length = iso_packet_descriptors[i].length;
    }

    transfer->flags = get_libusb_transfer_flags(transfer_flags);
    masking_bogus_flags(is_out, transfer);

    transfer_tracker_.register_transfer(seqnum, transfer, ep.address);

    auto err = libusb_submit_transfer(transfer);
    if (err < 0)[[unlikely]] {
        SPDLOG_ERROR("同步传输失败，{}", libusb_strerror(err));
        transfer_tracker_.remove(seqnum);
        callback_args->transfer_buffer.clear();
        callback_args->transfer_buffer.shrink_to_fit();
        if (!callback_args_pool_.free(callback_args)) {
            delete callback_args;
        }
        libusb_free_transfer(transfer);
        if (err == LIBUSB_ERROR_NO_DEVICE)[[unlikely]] {
            device_removed = true;
            ec = make_error_code(ErrorType::NO_DEVICE);
        }
        session->submit_ret_submit(
                UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum));
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
    if (setup_packet.is_clear_halt_cmd()) {
        return tweak_clear_halt_cmd(setup_packet);
    }
    else if (setup_packet.is_set_interface_cmd()) {
        return tweak_set_interface_cmd(setup_packet);
    }
    else if (setup_packet.is_set_configuration_cmd()) {
        return tweak_set_configuration_cmd(setup_packet);
    }
    else if (setup_packet.is_reset_device_cmd()) {
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

    LATENCY_TRACK(callback_arg.handler->session->latency_tracker,callback_arg.seqnum,"LibusbDeviceHandler::transfer_callback调用");

    //调了回调则当前包并未在发送，因此只要调了回调就先将其删了
    callback_arg.handler->transfer_tracker_.remove(callback_arg.seqnum);

    // 如果断连了，检查是否所有传输都完成了
    if (callback_arg.handler->client_disconnection)[[unlikely]] {
        if (callback_arg.handler->transfer_tracker_.concurrent_count() == 0) {
            std::lock_guard lock(callback_arg.handler->transfer_complete_mutex_);
            callback_arg.handler->transfer_complete_cv_.notify_one();
        }
        // 清理并返回
        callback_arg.transfer_buffer.clear();
        callback_arg.transfer_buffer.shrink_to_fit();
        if (!callback_arg.handler->callback_args_pool_.free(&callback_arg)) {
            delete &callback_arg;
        }
        libusb_free_transfer(trx);
        return;
    }

    // std::error_code ec;
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


    auto unlink_found = callback_arg.handler->session->get_unlink_seqnum(callback_arg.seqnum);
    if (!std::get<0>(unlink_found))[[likely]] {
        //发送ret_submit
        auto ret = UsbIpResponse::UsbIpRetSubmit::create_ret_submit(
                callback_arg.seqnum,
                trxstat2error(trx->status),
                0,
                trx->num_iso_packets,
                std::move(callback_arg.transfer_buffer),
                {}
                );

        if (!callback_arg.is_out) {
            ret.actual_length = trx->actual_length;

            if (trx->type == LIBUSB_TRANSFER_TYPE_CONTROL)[[unlikely]] {
                // 控制传输：数据从偏移8开始
                ret.send_config.data_offset = LIBUSB_CONTROL_SETUP_SIZE;
            }
            else if (trx->type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) {
                std::vector<UsbIpIsoPacketDescriptor> iso_packet_descriptors{};
                // ISO传输：零拷贝，使用 scatter-gather 发送
                // iso_packet_descriptor 用于 USBIP 协议：
                // - offset: 紧凑排列后的偏移（协议要求）
                // - length: 原始长度（用于 to_socket_co 计算原始 buffer 偏移）
                // - actual_length: 实际数据长度
                iso_packet_descriptors.resize(trx->num_iso_packets);

                //重新计算长度，一般来说不用，但写了更保险
                size_t iso_actual_length = 0;
                for (int i = 0; i < trx->num_iso_packets; i++) {
                    auto &iso_packet = trx->iso_packet_desc[i];
                    iso_actual_length += iso_packet.actual_length;
                }

                // 计算紧凑的iso包描述
                size_t received_data_offset = 0;
                size_t trx_buffer_offset = 0;
                for (int i = 0; i < trx->num_iso_packets; i++) {
                    auto &iso_packet = trx->iso_packet_desc[i];
                    iso_packet_descriptors[i].offset = received_data_offset;
                    iso_packet_descriptors[i].length = iso_packet.actual_length;
                    iso_packet_descriptors[i].actual_length = iso_packet.actual_length;
                    iso_packet_descriptors[i].status = trxstat2error(iso_packet.status);

                    //记录在内存中的长度以便发送时遍历每一小节
                    iso_packet_descriptors[i].length_in_transfer_buffer_only_for_send = iso_packet.length;

                    received_data_offset += iso_packet.actual_length;
                    trx_buffer_offset += iso_packet.length;
                }
                ret.iso_packet_descriptor = std::move(iso_packet_descriptors);
                ret.actual_length = static_cast<std::uint32_t>(iso_actual_length);
                ret.send_config.data_offset = 0;
            }
            else[[likely]] {
                // Bulk/Interrupt传输：零拷贝，直接使用移动过来的buffer
                assert(trx->actual_length == ret.transfer_buffer.size());
                ret.send_config.data_offset = 0;
            }
        }
        else {
            // OUT传输没有返回数据
            ret.actual_length = 0;
        }

        SPDLOG_DEBUG("libusb传输actual_length为{}个字节", trx->actual_length);

        LATENCY_TRACK(callback_arg.handler->session->latency_tracker,callback_arg.seqnum,"LibusbDeviceHandler::transfer_callback submit_ret_submit");
        callback_arg.handler->session->submit_ret_submit(std::move(ret));
    }
    else {
        auto cmd_unlink_seqnum = std::get<1>(unlink_found);
        LATENCY_TRACK_END_MSG(callback_arg.handler->session->latency_tracker,cmd_unlink_seqnum,"cmd_unlink_seqnum被unlink");
        //发送ret_unlink
        callback_arg.handler->session->submit_ret_unlink_and_then_remove_seqnum_unlink(
                UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(
                        cmd_unlink_seqnum,
                        trxstat2error(trx->status)
                        ),
                callback_arg.seqnum
                );
    }

    //清理数据，归还对象池
    callback_arg.transfer_buffer.clear();
    callback_arg.transfer_buffer.shrink_to_fit();
    if (!callback_arg.handler->callback_args_pool_.free(&callback_arg)) {
        delete &callback_arg;
    }
    libusb_free_transfer(trx);
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
