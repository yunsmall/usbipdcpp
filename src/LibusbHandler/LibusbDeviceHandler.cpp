#include "LibusbHandler/LibusbDeviceHandler.h"


#include "Endpoint.h"
#include "LibusbHandler/LibusbTransferOperator.h"
#include "Session.h"
#include "SetupPacket.h"
#include "constant.h"
#include "protocol.h"

using namespace usbipdcpp;

// 普通模式构造函数
usbipdcpp::LibusbDeviceHandler::LibusbDeviceHandler(UsbDevice &handle_device, libusb_device *native_device) :
    AbstDeviceHandler(handle_device, std::make_unique<LibusbTransferOperator>()), native_device_(native_device) {
    // 设备尚未打开，将在 on_new_connection 时打开
}

// Android 模式构造函数
usbipdcpp::LibusbDeviceHandler::LibusbDeviceHandler(UsbDevice &handle_device, intptr_t fd) :
    AbstDeviceHandler(handle_device, std::make_unique<LibusbTransferOperator>()), wrapped_fd_(fd) {
    // fd 将在 on_new_connection 时通过 libusb_wrap_sys_device 包装
}

usbipdcpp::LibusbDeviceHandler::~LibusbDeviceHandler() {
    if (native_device_) {
        libusb_unref_device(native_device_);
        native_device_ = nullptr;
    }
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

    // 标记客户端连接
    client_disconnection = false;
}

void usbipdcpp::LibusbDeviceHandler::on_disconnection(error_code &ec) {
    client_disconnection = true;
    // 不检查 device_removed，因为 libusb 会在设备拔出时正确触发回调（LIBUSB_TRANSFER_NO_DEVICE）

    // 取消所有传输
    {
        std::shared_lock lock(transfers_mutex_);
        for (auto &[seqnum, cb]: transfers_) {
            auto err = libusb_cancel_transfer(static_cast<libusb_transfer *>(cb->transfer.get()));
            if (err) {
                SPDLOG_ERROR("libusb_cancel_transfer failed on seqnum {}: {}", cb->seqnum, libusb_strerror(err));
            }
        }
    }

    // 等待所有传输完成
    {
        std::unique_lock lock(transfer_complete_mutex_);
        transfer_complete_cv_.wait(lock, [this]() { return pending_count_.load(std::memory_order_acquire) == 0; });
    }

    // 为下次连接做准备，重置对象池状态
    callback_args_pool_.reset();

    // 断开连接时释放接口并关闭设备
    if (interfaces_claimed_) {
        release_and_close_device();
    }

    AbstDeviceHandler::on_disconnection(ec);
}

void usbipdcpp::LibusbDeviceHandler::receive_urb(UsbIpCommand::UsbIpCmdSubmit cmd, UsbEndpoint ep,
                                                 std::optional<UsbInterface> interface, usbipdcpp::error_code &ec) {

    if (device_removed) [[unlikely]] {
        ec = make_error_code(ErrorType::NO_DEVICE);
        return;
    }

    auto seqnum = cmd.header.seqnum;
    auto transfer_flags = cmd.transfer_flags;
    auto transfer_buffer_length = cmd.transfer_buffer_length;
    const auto &setup_packet = cmd.setup;

    // 根据端点类型分发
    if (ep.attributes == static_cast<std::uint8_t>(EndpointAttributes::Control)) [[unlikely]] {
        // 控制传输
        auto tweak_ret = tweak_special_requests(setup_packet);
        if (tweak_ret < 0) [[likely]] {
            // 不需要 tweak，提交 transfer
            SPDLOG_DEBUG("控制传输 {}，ep addr: {:02x}", ep.direction() == UsbEndpoint::Direction::Out ? "Out" : "In",
                         ep.address);

            auto *trx = static_cast<libusb_transfer *>(cmd.transfer.get());

            // 填充 setup 包到 buffer 开头
            libusb_fill_control_setup(trx->buffer, setup_packet.request_type, setup_packet.request, setup_packet.value,
                                      setup_packet.index, setup_packet.length);

            auto *callback_args = callback_args_pool_.alloc();
            if (!callback_args) [[unlikely]] {
                callback_args = new libusb_callback_args{};
            }
            callback_args->handler = this;
            callback_args->seqnum = seqnum;
            callback_args->is_out = setup_packet.is_out();
            callback_args->transfer = std::move(cmd.transfer); // 转移所有权

            libusb_fill_control_transfer(trx, native_handle, trx->buffer, LibusbDeviceHandler::transfer_callback,
                                         callback_args, timeout_milliseconds);
            trx->flags = get_libusb_transfer_flags(transfer_flags);
            masking_bogus_flags(setup_packet.is_out(), trx);

            {
                std::lock_guard lock(transfers_mutex_);
                transfers_.emplace(seqnum, callback_args);
                pending_count_.fetch_add(1, std::memory_order_release);
            }

            LATENCY_TRACK(session->latency_tracker, seqnum, "LibusbDeviceHandler::receive_urb libusb_submit_transfer");
            auto err = libusb_submit_transfer(trx);

            if (err < 0) [[unlikely]] {
                SPDLOG_ERROR("控制传输给设备失败：{}", libusb_strerror(err));
                {
                    std::lock_guard lock(transfers_mutex_);
                    if (transfers_.erase(seqnum)) {
                        pending_count_.fetch_sub(1, std::memory_order_release);
                    }
                }
                callback_args->transfer.reset();
                if (!callback_args_pool_.free(callback_args)) {
                    delete callback_args;
                }
                session->submit_ret_submit(
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                if (err == LIBUSB_ERROR_NO_DEVICE || err == LIBUSB_ERROR_IO) [[unlikely]] {
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
    else if (interface.has_value()) [[likely]] {
        bool is_out = !ep.is_in();

        auto *trx = static_cast<libusb_transfer *>(cmd.transfer.get());

        auto *callback_args = callback_args_pool_.alloc();
        if (!callback_args) [[unlikely]] {
            callback_args = new libusb_callback_args{};
        }
        callback_args->handler = this;
        callback_args->seqnum = seqnum;
        callback_args->is_out = is_out;
        callback_args->transfer = std::move(cmd.transfer); // 转移所有权

        if (ep.attributes == static_cast<std::uint8_t>(EndpointAttributes::Bulk)) [[likely]] {
            LATENCY_TRACK(session->latency_tracker, seqnum, "LibusbDeviceHandler::receive_urb bulk");

            libusb_fill_bulk_transfer(trx, native_handle, ep.address, trx->buffer, transfer_buffer_length,
                                      LibusbDeviceHandler::transfer_callback, callback_args, timeout_milliseconds);
        }
        else if (ep.attributes == static_cast<std::uint8_t>(EndpointAttributes::Interrupt)) {
            SPDLOG_DEBUG("中断传输 {}，ep addr: {:02x}", ep.direction() == UsbEndpoint::Direction::Out ? "Out" : "In",
                         ep.address);

            libusb_fill_interrupt_transfer(trx, native_handle, ep.address, trx->buffer, transfer_buffer_length,
                                           LibusbDeviceHandler::transfer_callback, callback_args, timeout_milliseconds);
        }
        else if (ep.attributes == static_cast<std::uint8_t>(EndpointAttributes::Isochronous)) {
            int num_iso_packets = (cmd.number_of_packets != 0 && cmd.number_of_packets != 0xFFFFFFFF)
                                          ? static_cast<int>(cmd.number_of_packets)
                                          : 0;
            SPDLOG_DEBUG("同步传输 {}，ep addr: {:02x}", ep.direction() == UsbEndpoint::Direction::Out ? "Out" : "In",
                         ep.address);

            libusb_fill_iso_transfer(trx, native_handle, ep.address, trx->buffer, transfer_buffer_length,
                                     num_iso_packets, LibusbDeviceHandler::transfer_callback, callback_args,
                                     timeout_milliseconds);
        }
        else [[unlikely]] {
            SPDLOG_DEBUG("端口{:02x}的未知传输类型：{}", ep.address, ep.attributes);
            callback_args->transfer.reset();
            if (!callback_args_pool_.free(callback_args)) {
                delete callback_args;
            }
            session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
            return;
        }

        trx->flags = get_libusb_transfer_flags(transfer_flags);
        masking_bogus_flags(is_out, trx);

        {
            std::lock_guard lock(transfers_mutex_);
            transfers_.emplace(seqnum, callback_args);
            pending_count_.fetch_add(1, std::memory_order_release);
        }

        auto err = libusb_submit_transfer(trx);
        if (err < 0) [[unlikely]] {
            SPDLOG_ERROR("传输失败，{}", libusb_strerror(err));
            {
                std::lock_guard lock(transfers_mutex_);
                if (transfers_.erase(seqnum)) {
                    pending_count_.fetch_sub(1, std::memory_order_release);
                }
            }
            callback_args->transfer.reset();
            if (!callback_args_pool_.free(callback_args)) {
                delete callback_args;
            }
            if (err == LIBUSB_ERROR_NO_DEVICE || err == LIBUSB_ERROR_IO) [[unlikely]] {
                device_removed = true;
                ec = make_error_code(ErrorType::NO_DEVICE);
            }
            session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
        }
    }
    else [[unlikely]] {
        SPDLOG_ERROR("非控制传输却不存在目标接口");
        session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
    }
}

void usbipdcpp::LibusbDeviceHandler::handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) {
    if (device_removed) [[unlikely]]
        return;

    {
        std::shared_lock lock(transfers_mutex_);
        auto it = transfers_.find(unlink_seqnum);
        if (it != transfers_.end()) {
            auto *cb = it->second;
            // transfer 仍在 tracker 中，在锁内标记 unlinking 并 cancel。
            // 回调执行时会看到 unlinking==true，走 RET_UNLINK 分支，并负责唤醒 sender。
            cb->unlinking = true;
            cb->unlink_cmd_seqnum = cmd_seqnum;

            lock.unlock();

            int err = libusb_cancel_transfer(static_cast<libusb_transfer *>(cb->transfer.get()));
            if (err == LIBUSB_ERROR_NOT_FOUND) [[unlikely]] {
                // transfer 在 libusb 层已完成但回调尚未执行。unlinking 已置位，
                // 回调会走 unlink 分支入队 RET_UNLINK（带实际状态码）并唤醒 sender。
            }
            else if (err) [[unlikely]] {
                SPDLOG_ERROR("libusb_cancel_transfer failed: {}", libusb_strerror(err));
            }
        }
        else {
            SPDLOG_DEBUG("handle_unlink: transfer NOT found, enqueue RET_UNLINK({})", cmd_seqnum);
            // transfer 已被 transfer_callback 从 map 移除，说明回调已决定发送 RET_SUBMIT
            // 还是 RET_UNLINK 并已入队。此时主机即将收到（或已经收到）该传输的完成通知，
            // 这个 CMD_UNLINK 已经无关紧要——传输已经完成了，unlink 天然晚了。
            // 因此 RET_UNLINK(0) 不需要保证马上发出，入队即可，不唤醒 sender。
            // 等待下一次 callback 的 wakeup_sender() 顺带发出，或者 session 清理时发出。
            session->submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(cmd_seqnum, 0));
            lock.unlock();
        }
    }
}

int usbipdcpp::LibusbDeviceHandler::tweak_clear_halt_cmd(const SetupPacket &setup_packet) {
    auto target_endp = setup_packet.index;
    SPDLOG_INFO("tweak_clear_halt_cmd");

    auto err = libusb_clear_halt(native_handle, target_endp);
    if (err) [[unlikely]] {
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

    SPDLOG_INFO("set_interface: inf {} alt {}", interface, alternate);
    int err = libusb_set_interface_alt_setting(native_handle, interface, alternate);
    if (err) [[unlikely]] {
        SPDLOG_ERROR("{}: usb_set_interface error: inf {} alt {} err {}",
                     get_device_busid(libusb_get_device(native_handle)), interface, alternate, libusb_strerror(err));
    }
    else {
        SPDLOG_DEBUG("{}: usb_set_interface done: inf {} alt {}", get_device_busid(libusb_get_device(native_handle)),
                     interface, alternate);

        // 同步更新 handle_device 中该接口的端点列表
        if (interface < handle_device.interfaces.size()) {
            libusb_device *dev = libusb_get_device(native_handle);
            libusb_config_descriptor *config = nullptr;
            if (libusb_get_active_config_descriptor(dev, &config) == 0 && config) {
                if (interface < config->bNumInterfaces) {
                    auto &intf = config->interface[interface];
                    if (alternate < intf.num_altsetting) {
                        auto &intf_desc = intf.altsetting[alternate];
                        std::vector<UsbEndpoint> endpoints;
                        endpoints.reserve(intf_desc.bNumEndpoints);
                        for (int ep_i = 0; ep_i < intf_desc.bNumEndpoints; ep_i++) {
                            endpoints.emplace_back(intf_desc.endpoint[ep_i].bEndpointAddress,
                                                   intf_desc.endpoint[ep_i].bmAttributes & 0x03,
                                                   intf_desc.endpoint[ep_i].wMaxPacketSize,
                                                   intf_desc.endpoint[ep_i].bInterval);
                        }
                        handle_device.interfaces[interface].endpoints = std::move(endpoints);
                        SPDLOG_DEBUG("已更新接口 {} 的端点列表，新端点数量: {}", interface,
                                     handle_device.interfaces[interface].endpoints.size());
                    }
                }
                libusb_free_config_descriptor(config);
            }
        }
    }
    return err; // 返回 0 表示成功，正数表示错误
}

int usbipdcpp::LibusbDeviceHandler::tweak_set_configuration_cmd(const SetupPacket &setup_packet) {
    SPDLOG_INFO("tweak_set_configuration_cmd");

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

    // 不可以set_configuration，会device_busy
    //  usbipd-libusb 返回 -1，表示不处理这个命令，继续正常提交 transfer
    //  设备会收到 set_configuration 命令
    return -1;
}

int usbipdcpp::LibusbDeviceHandler::tweak_reset_device_cmd(const SetupPacket &setup_packet) {
    SPDLOG_INFO("{}: usb_queue_reset_device", get_device_busid(libusb_get_device(native_handle)));

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
    if (setup_packet.is_clear_halt_cmd()) [[unlikely]] {
        return tweak_clear_halt_cmd(setup_packet);
    }
    else if (setup_packet.is_set_interface_cmd()) [[unlikely]] {
        return tweak_set_interface_cmd(setup_packet);
    }
    else if (setup_packet.is_set_configuration_cmd()) [[unlikely]] {
        return tweak_set_configuration_cmd(setup_packet);
    }
    else if (setup_packet.is_reset_device_cmd()) [[unlikely]] {
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
    // 具体数值抄的linux的
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
        case static_cast<int>(UrbStatusType::StatusEEOVERFLOW): // EOVERFLOW
            return LIBUSB_TRANSFER_OVERFLOW;
        default:
            return LIBUSB_TRANSFER_ERROR;
    }
}

void LIBUSB_CALL usbipdcpp::LibusbDeviceHandler::transfer_callback(libusb_transfer *trx) {
    auto &callback_arg = *static_cast<libusb_callback_args *>(trx->user_data);

    // SPDLOG_WARN("callback: seqnum={} type={} num_iso={} actual_length={} is_out={}", callback_arg.seqnum,
    //             static_cast<int>(trx->type), trx->num_iso_packets, trx->actual_length, callback_arg.is_out);

    LATENCY_TRACK(callback_arg.handler->session->latency_tracker, callback_arg.seqnum,
                  "LibusbDeviceHandler::transfer_callback调用");

    // 如果断连了，直接清理并返回（不发送响应）
    // 这是在传输完成后检查，断连是特殊情况
    if (callback_arg.handler->client_disconnection) [[unlikely]] {
        auto *handler = callback_arg.handler;
        handler->transfers_mutex_.lock();
        handler->transfers_.erase(callback_arg.seqnum);
        handler->pending_count_.fetch_sub(1, std::memory_order_release);
        handler->transfers_mutex_.unlock();
        callback_arg.transfer.reset(); // 释放 libusb_transfer，避免延后到下次 alloc
        if (!handler->callback_args_pool_.free(&callback_arg)) {
            delete &callback_arg;
        }
        handler->transfer_complete_cv_.notify_one();
        return;
    }

    // status 检查
    switch (trx->status) {
        case LIBUSB_TRANSFER_COMPLETED:
            /* OK */
            break;
        case LIBUSB_TRANSFER_ERROR:
            if (!(trx->flags & LIBUSB_TRANSFER_SHORT_NOT_OK)) {
                dev_err(libusb_get_device(trx->dev_handle), "error on endpoint {}", trx->endpoint);
            }
            else {
                // Tweaking status to complete as we received data, but all
                trx->status = LIBUSB_TRANSFER_COMPLETED;
            }
            break;
        case LIBUSB_TRANSFER_CANCELLED:
            dev_info(libusb_get_device(trx->dev_handle), "unlinked by a call to usb_unlink_urb()");
            break;
        case LIBUSB_TRANSFER_STALL:
            dev_err(libusb_get_device(trx->dev_handle), "endpoint {} is stalled", trx->endpoint);
            break;
        case LIBUSB_TRANSFER_NO_DEVICE:
            dev_info(libusb_get_device(trx->dev_handle), "device removed?");
            callback_arg.handler->device_removed = true;
            break;
        default:
            dev_warn(libusb_get_device(trx->dev_handle), "urb completion with unknown status {}",
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

    // 在锁内检查 unlinking、入队响应、移除追踪——三个操作原子完成。
    // handle_unlink_seqnum 若在此之前执行，会看到 map 中有此 entry 并设置 unlinking；
    // 若在此之后，则看不到，自行入队 RET_UNLINK(0)。
    bool unlinking = false;
    std::uint32_t unlink_cmd_seqnum = 0;

    {
        auto *handler = callback_arg.handler;
        std::unique_lock lock(handler->transfers_mutex_);
        unlinking = callback_arg.unlinking;
        unlink_cmd_seqnum = callback_arg.unlink_cmd_seqnum;
        handler->transfers_.erase(callback_arg.seqnum);
        handler->pending_count_.fetch_sub(1, std::memory_order_release);

        if (unlinking) [[unlikely]] {
            // URB 被 unlink 取消，入队 RET_UNLINK（带实际传输状态码）
            callback_arg.handler->session->enqueue_ret_unlink(
                    UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(unlink_cmd_seqnum, trxstat2error(trx->status)));
            // unlink 情况：释放 transfer
            callback_arg.transfer.reset();
        }
        else {
            // 发送 ret_submit
            // OUT 传输不需要发送数据回客户端，只发送 header（无 transfer_handle）
            // IN 传输需要发送 header + 数据（有 transfer_handle）
            UsbIpResponse::UsbIpRetSubmit ret;
            if (callback_arg.is_out) {
                // OUT 传输：无数据阶段
                ret = UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(
                        callback_arg.seqnum, trxstat2error(trx->status), actual_length);
                // OUT 传输：释放 transfer
                callback_arg.transfer.reset();
            }
            else {
                // IN 传输：有数据，转移所有权
                ret = UsbIpResponse::UsbIpRetSubmit::create_ret_submit(callback_arg.seqnum, trxstat2error(trx->status),
                                                                       actual_length,
                                                                       0, // start_frame
                                                                       trx->num_iso_packets,
                                                                       std::move(callback_arg.transfer) // 转移所有权
                );
            }
            callback_arg.handler->session->enqueue_ret_submit(std::move(ret));
        }
    }

    SPDLOG_DEBUG("libusb传输actual_length为{}个字节", actual_length);

    LATENCY_TRACK(callback_arg.handler->session->latency_tracker, callback_arg.seqnum,
                  "LibusbDeviceHandler::transfer_callback submit_ret_submit");

    // 入队完成，唤醒 sender 线程统一发送
    callback_arg.handler->session->wakeup_sender();

    // 释放 libusb_transfer 后归还 callback_arg（值字段由 Reset 在 alloc 时清零）
    auto *handler = callback_arg.handler;
    callback_arg.transfer.reset();
    if (!handler->callback_args_pool_.free(&callback_arg)) {
        delete &callback_arg;
    }
    // 无条件通知，覆盖正常路径与 on_disconnection 的竞态。
    // CV 谓词 pending_count_==0 确保只有最后一个 callback 真正唤醒 wait。
    handler->transfer_complete_cv_.notify_one();
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

    // 尝试 detach kernel driver 后再声明接口
    for (int intf_i = 0; intf_i < num_interfaces; intf_i++) {
        int detach_err = libusb_detach_kernel_driver(native_handle, intf_i);
        if (detach_err && detach_err != LIBUSB_ERROR_NOT_FOUND) {
            SPDLOG_WARN("Android 模式下 detach kernel driver 接口 {} 失败: {}", intf_i, libusb_strerror(detach_err));
        }
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
