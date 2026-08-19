#include "usbipdcpp/LibusbHandler/LibusbDeviceHandler.h"

#include <chrono>
#include <spdlog/spdlog.h>

#include "usbipdcpp/Endpoint.h"
#include "usbipdcpp/LibusbHandler/LibusbTransferOperator.h"
#include "usbipdcpp/Session.h"
#include "usbipdcpp/SetupPacket.h"
#include "usbipdcpp/constant.h"
#include "usbipdcpp/protocol.h"

using namespace usbipdcpp;

namespace {
// 回调中取设备 busid 用于日志：libusb 契约保证回调执行期间 trx->dev_handle
// 必然有效（提交时 handle 持引用计数，直到 libusb_close 才释放，而 close
// 在 on_disconnection 等 pending_count_ 归零、全部回调完成之后），
// libusb_get_device 也恒返回 handle->dev 非空。判空仅为防御 libusb 版本
// 行为差异（如某些后端在 NO_DEVICE 时提前清理 handle 内部状态）
std::string get_trx_device_busid(libusb_transfer *trx) {
    if (auto *dev = libusb_get_device(trx->dev_handle)) {
        return get_device_busid(dev);
    }
    return "unknown";
}
} // namespace

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
            // 基类的 on_new_connection 已注册本 Session，失败后必须撤销注册：
            // 残留的 session 指针指向即将析构的 Session。设备随后回 available，
            // 下次导入时 try_moving_device_to_using 先把设备移入 using，在
            // on_new_connection 重新注册之前的窗口内若设备拔出，handle_device_left
            // 走 using 分支调 trigger_session_stop，会通过残留指针访问已析构的
            // Session（use-after-free）。remove_session 使指针为 null 安全跳过
            remove_session();
            ec = make_error_code(ErrorType::NO_DEVICE);
            return;
        }
    }
    else if (wrapped_fd_ >= 0) {
        // Android 模式：wrap fd 并声明接口
        if (!wrap_fd_and_claim_interfaces()) {
            SPDLOG_ERROR("wrap fd 失败");
            remove_session();
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
        // 持共享锁调用 libusb_cancel_transfer 不会死锁：libusb 的传输回调
        // 只在 handle_events 线程执行，cancel 只异步提交取消请求、不调用
        // 任何回调（usbfs 后端为 URB_CANCEL ioctl）。回调线程最多短暂阻塞
        // 等待 unique_lock，cancel 返回释放共享锁后即继续
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
        // libusb 契约保证已提交的传输最终必触发回调（取消的传输同样以
        // CANCELLED 完成，见 libusb_cancel_transfer 文档），等待必然结束。
        // 超时仅作为 libusb 内部错误的诊断手段（与 Server::stop 的
        // reap_cv 超时模式一致）；不能超时后放弃等待直接继续——回调仍可能
        // 在事件线程执行并访问本 handler，handler 提前析构是 use-after-free
        std::unique_lock lock(transfer_complete_mutex_);
        if (!transfer_complete_cv_.wait_for(lock, std::chrono::seconds(10),
                                            [this]() { return pending_count_.load(std::memory_order_acquire) == 0; })) {
            SPDLOG_ERROR("on_disconnection 等待传输完成超时（剩余 {} 个），继续等待", pending_count_.load());
            transfer_complete_cv_.wait(lock, [this]() { return pending_count_.load(std::memory_order_acquire) == 0; });
        }
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

            // 校验 wLength 不超过 transfer_buffer_length：libusb_fill_control_transfer
            // 会按 wLength 设置传输长度（8 + wLength），若 wLength 大于按
            // transfer_buffer_length 分配的缓冲区，libusb 提交后读写会越界
            // （堆溢出）。恶意客户端可构造 wLength=0xFFFF 触发，必须拒绝。
            // 不要求严格相等：内核 usbcore（usb_submit_urb）虽然对
            // wLength != transfer_buffer_length 返回 -EBADR，但那是客户端
            // 主机侧约束（Linux/Android vhci 的 URB 必然相等）；Windows
            // vhci（usbip-win2）无此约束，可能发送 wLength <
            // transfer_buffer_length 的包。此时数据阶段按
            // transfer_buffer_length 读取（协议固定字段），多余的字节
            // 读掉即丢弃、后续协议流不错位，无内存风险
            if (setup_packet.length > transfer_buffer_length) [[unlikely]] {
                SPDLOG_ERROR("控制传输 wLength({}) 大于 transfer_buffer_length({})，拒绝传输",
                             setup_packet.length, transfer_buffer_length);
                // cmd 析构时 transfer 自动释放（TransferHandle RAII）
                session->submit_ret_submit(
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                return;
            }

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

            bool duplicate_seqnum = false;
            {
                std::lock_guard lock(transfers_mutex_);
                // 拒绝重复 seqnum：transfers_ 以 seqnum 为 key，两个在途
                // 传输共用同一 key 时，回调/unlink/submit 失败路径的
                // erase 会错删先提交传输的记录，响应与取消语义全乱
                // （RET_SUBMIT 与 RET_UNLINK 可能乱序）。协议要求 seqnum
                // 单调递增（内核 vhci 原子递增），重复属客户端违规。
                // 本次传输不提交给设备，按 EPIPE 回复
                if (transfers_.contains(seqnum)) [[unlikely]] {
                    duplicate_seqnum = true;
                }
                else {
                    transfers_.emplace(seqnum, callback_args);
                    pending_count_.fetch_add(1, std::memory_order_release);
                }
            }
            if (duplicate_seqnum) [[unlikely]] {
                callback_args->transfer.reset();
                if (!callback_args_pool_.free(callback_args)) {
                    delete callback_args;
                }
                session->submit_ret_submit(
                        UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                return;
            }

            LATENCY_TRACK(session->latency_tracker, seqnum, "LibusbDeviceHandler::receive_urb libusb_submit_transfer");
            auto err = libusb_submit_transfer(trx);

            if (err < 0) [[unlikely]] {
                SPDLOG_ERROR("控制传输给设备失败：{}", libusb_strerror(err));
                bool unlinked = false;
                std::uint32_t unlink_cmd_seqnum = 0;
                {
                    std::lock_guard lock(transfers_mutex_);
                    if (transfers_.erase(seqnum)) {
                        // 只递减不通知：receive_urb 与 on_disconnection 同在
                        // receiver 线程，本路径的递减必然先于等待的谓词检查
                        // 执行（谓词直接满足，等待者不会入睡），无需 notify
                        // 也不会丢失唤醒。同时不能在此唤醒——notify 后等待者
                        // （若未来跨线程调用 on_disconnection）会清理对象池并
                        // 可能析构 handler，而本函数之后还要访问
                        // callback_args/session
                        pending_count_.fetch_sub(1, std::memory_order_release);
                    }
                    // 锁内读取取消标记：handle_unlink_seqnum 可能在 submit 完成前
                    // 已置 unlinking（此时 cancel 返回 NOT_FOUND，因为传输尚未
                    // 提交）。若此处只发 RET_SUBMIT 而丢弃取消标记，客户端会
                    // 一直等 RET_UNLINK 而挂起。置位则改发 RET_UNLINK
                    unlinked = callback_args->unlinking;
                    unlink_cmd_seqnum = callback_args->unlink_cmd_seqnum;
                }
                callback_args->transfer.reset();
                if (!callback_args_pool_.free(callback_args)) {
                    delete callback_args;
                }
                if (unlinked) [[unlikely]] {
                    session->submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(
                            unlink_cmd_seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE)));
                }
                else {
                    session->submit_ret_submit(
                            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
                }
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

        bool duplicate_seqnum = false;
        {
            std::lock_guard lock(transfers_mutex_);
            // 拒绝重复 seqnum（同控制传输分支的注释：transfers_ 以
            // seqnum 为 key，重复会让两个在途传输共用一个 entry，
            // erase/取消语义错乱）。按 EPIPE 回复，不提交给设备
            if (transfers_.contains(seqnum)) [[unlikely]] {
                duplicate_seqnum = true;
            }
            else {
                transfers_.emplace(seqnum, callback_args);
                pending_count_.fetch_add(1, std::memory_order_release);
            }
        }
        if (duplicate_seqnum) [[unlikely]] {
            callback_args->transfer.reset();
            if (!callback_args_pool_.free(callback_args)) {
                delete callback_args;
            }
            session->submit_ret_submit(
                    UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
            return;
        }

        auto err = libusb_submit_transfer(trx);
        if (err < 0) [[unlikely]] {
            SPDLOG_ERROR("传输失败，{}", libusb_strerror(err));
            bool unlinked = false;
            std::uint32_t unlink_cmd_seqnum = 0;
            {
                std::lock_guard lock(transfers_mutex_);
                if (transfers_.erase(seqnum)) {
                    // 只递减不通知（同控制路径：与 on_disconnection 同线程，
                    // 递减先于等待的谓词检查，谓词直接满足，无需 notify）
                    pending_count_.fetch_sub(1, std::memory_order_release);
                }
                // 同控制传输路径：submit 前已被 unlink 标记时改发 RET_UNLINK，
                // 否则客户端等不到 RET_UNLINK 而挂起
                unlinked = callback_args->unlinking;
                unlink_cmd_seqnum = callback_args->unlink_cmd_seqnum;
            }
            callback_args->transfer.reset();
            if (!callback_args_pool_.free(callback_args)) {
                delete callback_args;
            }
            if (unlinked) [[unlikely]] {
                session->submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(
                        unlink_cmd_seqnum, static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE)));
            }
            else {
                session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
            }
            if (err == LIBUSB_ERROR_NO_DEVICE || err == LIBUSB_ERROR_IO) [[unlikely]] {
                device_removed = true;
                ec = make_error_code(ErrorType::NO_DEVICE);
            }
        }
    }
    else [[unlikely]] {
        SPDLOG_ERROR("非控制传输却不存在目标接口");
        session->submit_ret_submit(UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(seqnum, 0));
    }
}

void usbipdcpp::LibusbDeviceHandler::handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) {
    if (device_removed) [[unlikely]]
        // 设备已物理拔出，连接即将断开，vhci 端会强制结束所有 URB。
        // 有意不回复 RET_UNLINK：缺失对即将断开的连接无实际影响
        return;

    {
        // 在共享锁下写 cb->unlinking / cb->unlink_cmd_seqnum 是安全的（非数据竞争）：
        // 1) 本函数只由 Session::receiver 单线程调用（同一 Session 只有一个收包
        //    循环，receiver 退出后的 on_disconnection 也在这个线程，不会并发）
        // 2) 唯一读这两个字段的地方——transfer_callback（libusb 事件线程）和
        //    receive_urb 的 submit 失败路径——都持排他锁，与这里的写者互斥，
        //    不存在"共享锁下读 unlinking"的路径
        // 3) 能与本函数并发持共享锁的 on_disconnection 只读 cb->transfer（get()
        //    只读）并调用 libusb_cancel_transfer（libusb 承诺线程安全），不触碰
        //    unlinking 字段；C++ 内存模型只对同一内存位置上的冲突访问定义数据
        //    竞争，对同一对象不同成员的并发访问不构成竞争
        std::shared_lock lock(transfers_mutex_);
        auto it = transfers_.find(unlink_seqnum);
        if (it != transfers_.end()) {
            auto *cb = it->second;
            // transfer 仍在 tracker 中，在锁内标记 unlinking 并 cancel。
            // 回调执行时会看到 unlinking==true，走 RET_UNLINK 分支，并负责唤醒 sender。
            cb->unlinking = true;
            cb->unlink_cmd_seqnum = cmd_seqnum;

            // 不在此处释放共享锁：libusb_cancel_transfer 仅设置标志位不调用回调，
            // 在锁内调用可防止 transfer_callback 拿到排他锁后释放 cb，导致下面 cb 悬空。
            int err = libusb_cancel_transfer(static_cast<libusb_transfer *>(cb->transfer.get()));
            if (err == LIBUSB_ERROR_NOT_FOUND) [[unlikely]] {
                // transfer 在 libusb 层已完成但回调尚未执行。unlinking 已置位，
                // 回调会走 unlink 分支入队 RET_UNLINK（带实际状态码）并唤醒 sender。
            }
            else if (err) [[unlikely]] {
                // libusb 契约：已提交的传输最终必触发回调（cancel 成功或
                // NOT_FOUND=已完成待派发，回调都以取消/完成状态触发），
                // 回调因 unlinking 已置位会入队 RET_UNLINK，此处无需立即
                // 回复。与参考项目 usbipd-libusb 的 stub_recv_cmd_unlink
                // 一致（只打日志）；若在错误分支立即入队 RET_UNLINK 而
                // 回调后来也触发，会发两个 RET_UNLINK（协议违规）
                SPDLOG_ERROR("libusb_cancel_transfer failed: {}", libusb_strerror(err));
            }
        }
        else {
            SPDLOG_DEBUG("handle_unlink: transfer NOT found, enqueue RET_UNLINK({})", cmd_seqnum);
            // transfer 已被 transfer_callback 从 map 移除，说明回调已决定发送 RET_SUBMIT
            // 还是 RET_UNLINK 并已入队。此时主机即将收到（或已经收到）该传输的完成通知，
            // 这个 CMD_UNLINK 已经无关紧要——传输已经完成了，unlink 天然晚了。
            // 用 submit_ret_unlink（入队+唤醒）立即发出：回调的唤醒可能已经发生过
            // （sender 消费完队列重新睡眠后本响应才入队），若依赖"下一次唤醒顺带
            // 发出"，空闲连接上的 RET_UNLINK 会滞留到连接关闭，客户端可能超时等待
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
    // 返回 libusb 错误码（0=成功，负=错误）。
    // caller 中 tweak_ret < 0 视为"未处理，提交 transfer 让设备自行处理"，
    // tweak_ret == 0 视为"已处理，无需提交 transfer"。
    // 因此 tweak 失败时会 fall through 到正常 transfer 提交，作为降级策略。
    return err;
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

        // 切换 current_altsetting（端点数据已在 bind 时预填）。
        // SET_INTERFACE 的 wIndex 是 bInterfaceNumber（USB 规范），不能当数组
        // 下标用：跳号设备（接口号不连续，如 0 和 2）下标与接口号不一致，
        // 按索引会切错接口。按 interface_number 匹配（bind 时从配置描述符
        // bInterfaceNumber 填充，见 LibusbServer.cpp）
        for (auto &dev_intf: handle_device.interfaces) {
            if (dev_intf.interface_number != interface) {
                continue;
            }
            if (alternate < dev_intf.endpoints.size()) {
                dev_intf.current_altsetting = static_cast<std::uint8_t>(alternate);
                SPDLOG_DEBUG("已切换接口 {} 到 alt {}，端点数量: {}", interface, alternate,
                             dev_intf.current_endpoints().size());
            }
            break;
        }
    }
    // 返回 libusb 错误码（0=成功，负=错误）。
    // caller 中 tweak_ret < 0 视为"未处理，提交 transfer 让设备自行处理"，
    // tweak_ret == 0 视为"已处理，无需提交 transfer"。
    // 因此 tweak 失败时会 fall through 到正常 transfer 提交，作为降级策略。
    return err;
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
    // 负数: 未处理（不需要 tweak 或 tweak 执行失败），caller 正常提交 transfer
    //  0: tweak 已成功处理，无需提交 transfer
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
    // 只转换 libusb 有对应物的标志：URB_ISO_ASAP 是 libusb ISO 传输的固有
    // 语义（Linux usbfs 强制 ASAP，libusb 无对应 flag 可设），其余 URB 标志
    // （NO_TRANSFER_DMA_MAP 等）与 libusb 传输无关，不需要转换
    uint8_t flags = 0;

    if (in & static_cast<std::uint32_t>(TransferFlag::URB_SHORT_NOT_OK))
        flags |= LIBUSB_TRANSFER_SHORT_NOT_OK;
    if (in & static_cast<std::uint32_t>(TransferFlag::URB_ZERO_PACKET))
        flags |= LIBUSB_TRANSFER_ADD_ZERO_PACKET;

    return flags;
}

void usbipdcpp::LibusbDeviceHandler::masking_bogus_flags(bool is_out, struct libusb_transfer *trx) {
    // 结构与 is_out 判定（控制传输按方向位或 wLength==0 视为 OUT，即无数据
    // 阶段）均与参考项目 usbipd-libusb 的 masking_bogus_flags 一致
    // （driver-libusb/stub_rx.c，两者同为 usbip 服务器实现）
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
        case LIBUSB_TRANSFER_TYPE_ISOCHRONOUS:
            // ISO 包短包（包实际长度小于分配槽位）是正常语义，不能设
            // SHORT_NOT_OK（否则依赖短包状态的 ISO 设备可能被误报
            // ERROR）。case 必须放在 default 之后：若放在 CONTROL 之后，
            // BULK/CONTROL 的 fallthrough 会在这里 break，永远到不了
            // default，Bulk IN / Control IN 的 SHORT_NOT_OK 会被清除
            // （曾引入此回归）。注：usbipd-libusb 的 masking_bogus_flags
            // 没有此 case、ISO 会掉进 default 被设上 SHORT_NOT_OK，但
            // Linux usbfs 的 URB_SHORT_NOT_OK 对 ISO 传输无实际效果
            // （ISO 完成状态由 iso_frame_desc 表达，不走 short 检查），
            // 此处显式排除以符合注释 "all non-iso endpoints" 的意图
            break;
    }
    trx->flags &= allowed;
}

int usbipdcpp::LibusbDeviceHandler::trxstat2error(enum libusb_transfer_status trxstat) {
    // 具体数值抄的linux的
    // TIMED_OUT 也映射 EPIPE 而非 ETIMEDOUT：与参考项目 usbipd-libusb 的
    // trxstat2error 一致（stub_common.c 中 ERROR/STALL/TIMED_OUT/OVERFLOW
    // 统一返回 -EPIPE）。且本后端 timeout_milliseconds 为 0（无限等待），
    // libusb 实际不会返回 TIMED_OUT，该分支不可达
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
        handler->transfers_mutex_.unlock();
        callback_arg.transfer.reset(); // 释放 libusb_transfer，避免延后到下次 alloc
        if (!handler->callback_args_pool_.free(&callback_arg)) {
            delete &callback_arg;
        }
        // 锁内递减并通知（依据见 decrement_pending_and_notify 注释）
        handler->decrement_pending_and_notify();
        return;
    }

    // status 检查
    switch (trx->status) {
        case LIBUSB_TRANSFER_COMPLETED:
            /* OK */
            break;
        case LIBUSB_TRANSFER_ERROR:
            // SHORT_NOT_OK 未设置：调用者接受短包，SHORT_NOT_OK 报告 ERROR 是正常的；
            //   实际数据已收到，视为完成以正确转发 actual_length。
            // SHORT_NOT_OK 已设置：调用者明确要求完整长度，这是真正的传输错误。
            if (!(trx->flags & LIBUSB_TRANSFER_SHORT_NOT_OK)) {
                trx->status = LIBUSB_TRANSFER_COMPLETED;
            }
            else {
                SPDLOG_ERROR("dev {}: error on endpoint {}", get_trx_device_busid(trx),
                             trx->endpoint);
            }
            break;
        case LIBUSB_TRANSFER_CANCELLED:
            SPDLOG_INFO("dev {}: unlinked by a call to usb_unlink_urb()",
                        get_trx_device_busid(trx));
            break;
        case LIBUSB_TRANSFER_STALL:
            SPDLOG_ERROR("dev {}: endpoint {} is stalled",
                         get_trx_device_busid(trx), trx->endpoint);
            break;
        case LIBUSB_TRANSFER_NO_DEVICE:
            SPDLOG_INFO("dev {}: device removed?", get_trx_device_busid(trx));
            callback_arg.handler->device_removed = true;
            break;
        default:
            SPDLOG_WARN("dev {}: urb completion with unknown status {}",
                        get_trx_device_busid(trx),
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

    // 统计 ISO 传输中失败的包数（协议 RET_SUBMIT 的 error_count 字段；
    // 内核 stub 由 USB 核心统计后填充，此处统计非 COMPLETED 的包等价）
    std::uint32_t error_count = 0;
    if (trx->type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) [[unlikely]] {
        for (int i = 0; i < trx->num_iso_packets; i++) {
            if (trx->iso_packet_desc[i].status != LIBUSB_TRANSFER_COMPLETED) {
                error_count++;
            }
        }
    }

    // 在锁内检查 unlinking、入队响应、移除追踪——三个操作原子完成。
    // handle_unlink_seqnum 若在此之前执行，会看到 map 中有此 entry 并设置 unlinking；
    // 若在此之后，则看不到，自行入队 RET_UNLINK(0)。
    bool unlinking = false;
    std::uint32_t unlink_cmd_seqnum = 0;

    {
        auto *handler = callback_arg.handler;
        std::unique_lock lock(handler->transfers_mutex_);
        // 约定：erase、检查 unlinking、入队响应必须在同一 unique_lock 内完成。
        // handle_unlink_seqnum 的 else 分支依赖「find 不到 ⇒ RET_SUBMIT 已入队」，
        // 若把入队挪出锁外，RET_SUBMIT 与 RET_UNLINK 的发送顺序将无法保证
        unlinking = callback_arg.unlinking;
        unlink_cmd_seqnum = callback_arg.unlink_cmd_seqnum;
        handler->transfers_.erase(callback_arg.seqnum);

        if (unlinking) [[unlikely]] {
            // URB 被 unlink 取消，入队 RET_UNLINK（带实际传输状态码）
            callback_arg.handler->session->enqueue_ret_unlink(
                    UsbIpResponse::UsbIpRetUnlink::create_ret_unlink(unlink_cmd_seqnum, trxstat2error(trx->status)));
            // unlink 情况：释放 transfer
            callback_arg.transfer.reset();
        }
        else {
            // 发送 ret_submit
            // OUT 传输不需要发送数据回客户端，只发送 header（无数据阶段）
            // IN 传输需要发送 header + 数据
            // ISO 传输（不分方向）协议要求返回 iso 描述符数组：内核 stub 对
            // ISO 传输总是发送 number_of_packets 个描述符（stub_tx.c 的
            // iso_packet_descriptor 分支不判断方向），vhci 侧按 header 的
            // number_of_packets 读取（usbip_recv_iso，np==0 时跳过不报错）。
            // 若不带描述符返回，客户端 URB 的 number_of_packets 被覆盖为 0、
            // 每包状态/实际长度缺失，依赖 per-packet 结果的驱动（audio 等）
            // 会行为异常。注意描述符 actual_length 之和必须等于 header 的
            // actual_length，否则 vhci 校验失败会断开连接——libusb 对 ISO
            // OUT 的 trx->actual_length 就是各包 actual_length 之和，天然满足
            UsbIpResponse::UsbIpRetSubmit ret;
            if (callback_arg.is_out) {
                if (trx->type == LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) [[unlikely]] {
                    // ISO OUT：转移所有权给响应，由 send_transfer_data 发送
                    // 描述符（OUT 方向不发送数据，见其注释）。
                    // start_frame 恒 0（同 IN 分支的注释：libusb 无帧号概念，
                    // 与 usbipd-libusb 一致）
                    ret = UsbIpResponse::UsbIpRetSubmit::create_ret_submit(
                            callback_arg.seqnum, trxstat2error(trx->status), actual_length,
                            0, trx->num_iso_packets, std::move(callback_arg.transfer));
                }
                else {
                    // 非 ISO OUT 传输：无数据阶段
                    ret = UsbIpResponse::UsbIpRetSubmit::create_ret_submit_with_status_and_no_data(
                            callback_arg.seqnum, trxstat2error(trx->status), actual_length);
                    // OUT 传输：释放 transfer
                    callback_arg.transfer.reset();
                }
            }
            else {
                // IN 传输：有数据，转移所有权。
                // start_frame 恒 0：libusb 传输没有帧号概念（usbfs 的
                // ISO 即 ASAP 调度），与参考项目 usbipd-libusb 一致
                // （stub_common.c 的 ret_submit 打包同样填 0）
                ret = UsbIpResponse::UsbIpRetSubmit::create_ret_submit(callback_arg.seqnum, trxstat2error(trx->status),
                                                                       actual_length,
                                                                       0, // start_frame
                                                                       trx->num_iso_packets,
                                                                       std::move(callback_arg.transfer) // 转移所有权
                );
            }
            ret.error_count = error_count;
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
    // 递减与通知统一走 decrement_pending_and_notify（锁内递减+通知，解决
    // 丢失唤醒与 use-after-free 两个并发问题，依据见其注释）：
    // 回调在 libusb 事件线程，on_disconnection 在 receiver 线程，两者并发
    handler->decrement_pending_and_notify();
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
        // libusb 的接口操作参数是 bInterfaceNumber 而非数组下标：跳号设备
        // （接口号不连续，如 0 和 2）下标与接口号不一致。用绑定接口记录的
        // 接口号（bind 时从配置描述符 bInterfaceNumber 填充）；防御式回退
        // 到下标（正常情况下两者一一对应）
        const int interface_number = intf_i < static_cast<int>(handle_device.interfaces.size())
                                             ? handle_device.interfaces[intf_i].interface_number
                                             : intf_i;
        err = libusb_detach_kernel_driver(native_handle, interface_number);
        if (err && err != LIBUSB_ERROR_NOT_FOUND) {
            SPDLOG_WARN("无法卸载接口 {} 的内核驱动: {}", interface_number, libusb_strerror(err));
        }

        err = libusb_claim_interface(native_handle, interface_number);
        if (err) {
            SPDLOG_ERROR("无法声明接口 {}: {}", interface_number, libusb_strerror(err));
            // 接口 i 在前面已执行 detach，必须先把它的内核驱动挂回去，
            // 否则该接口的内核驱动永久失联（attach 失败无害，忽略返回值）
            libusb_attach_kernel_driver(native_handle, interface_number);
            // 回滚已声明的接口
            for (int j = 0; j < intf_i; j++) {
                const int rollback_number = j < static_cast<int>(handle_device.interfaces.size())
                                                   ? handle_device.interfaces[j].interface_number
                                                   : j;
                libusb_release_interface(native_handle, rollback_number);
                libusb_attach_kernel_driver(native_handle, rollback_number);
            }
            libusb_free_config_descriptor(active_config_desc);
            libusb_close(native_handle);
            native_handle = nullptr;
            return false;
        }
    }

    // 确保所有接口在 alt 0，与 current_altsetting 一致
    for (int intf_i = 0; intf_i < num_interfaces; intf_i++) {
        const int interface_number = intf_i < static_cast<int>(handle_device.interfaces.size())
                                             ? handle_device.interfaces[intf_i].interface_number
                                             : intf_i;
        libusb_set_interface_alt_setting(native_handle, interface_number, 0);
        if (intf_i < static_cast<int>(handle_device.interfaces.size()))
            handle_device.interfaces[intf_i].current_altsetting = 0;
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
    // wrap 的 fd 不会被 libusb_close 关闭（官方文档：The system device handle
    // will not be closed by libusb_close()），fd 由外部调用方管理，因此此处
    // close native_handle 后可反复用同一 fd 重新 wrap；无需 dup

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
        // 同 open_and_claim_device：libusb 接口操作参数是 bInterfaceNumber
        // 而非数组下标（跳号设备的接口号与下标不一致）
        const int interface_number = intf_i < static_cast<int>(handle_device.interfaces.size())
                                             ? handle_device.interfaces[intf_i].interface_number
                                             : intf_i;
        int detach_err = libusb_detach_kernel_driver(native_handle, interface_number);
        if (detach_err && detach_err != LIBUSB_ERROR_NOT_FOUND) {
            SPDLOG_WARN("Android 模式下 detach kernel driver 接口 {} 失败: {}", interface_number,
                        libusb_strerror(detach_err));
        }
        err = libusb_claim_interface(native_handle, interface_number);
        if (err) {
            SPDLOG_ERROR("无法声明接口 {}: {}", interface_number, libusb_strerror(err));
            // 接口 i 在前面已尝试 detach，补一次 attach 把内核驱动挂回去。
            // Android 后端通常两者都返回 NOT_SUPPORTED，失败无害，忽略返回值
            libusb_attach_kernel_driver(native_handle, interface_number);
            // 回滚已声明的接口
            for (int j = 0; j < intf_i; j++) {
                const int rollback_number = j < static_cast<int>(handle_device.interfaces.size())
                                                   ? handle_device.interfaces[j].interface_number
                                                   : j;
                libusb_release_interface(native_handle, rollback_number);
            }
            libusb_free_config_descriptor(active_config_desc);
            libusb_close(native_handle);
            native_handle = nullptr;
            return false;
        }
    }

    // 确保所有接口在 alt 0，与 current_altsetting 一致
    for (int intf_i = 0; intf_i < num_interfaces; intf_i++) {
        const int interface_number = intf_i < static_cast<int>(handle_device.interfaces.size())
                                             ? handle_device.interfaces[intf_i].interface_number
                                             : intf_i;
        libusb_set_interface_alt_setting(native_handle, interface_number, 0);
        if (intf_i < static_cast<int>(handle_device.interfaces.size()))
            handle_device.interfaces[intf_i].current_altsetting = 0;
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

    // 使用绑定时已缓存的接口数量，不要反查 libusb 配置描述符。
    // 设备物理拔出后 libusb_get_active_config_descriptor 会失败返回 0，
    // 导致释放循环被跳过，内核驱动永远无法重新挂载。
    const int num_interfaces = static_cast<int>(handle_device.interfaces.size());

    // 释放所有接口
    for (int intf_i = 0; intf_i < num_interfaces; intf_i++) {
        // 同 open_and_claim_device：libusb 接口操作参数是 bInterfaceNumber
        // 而非数组下标（跳号设备的接口号与下标不一致）
        const int interface_number = intf_i < static_cast<int>(handle_device.interfaces.size())
                                             ? handle_device.interfaces[intf_i].interface_number
                                             : intf_i;
        int err = libusb_release_interface(native_handle, interface_number);
        // LIBUSB_ERROR_NO_DEVICE：设备已物理拔出，内核在 handle 关闭时自动重挂载驱动，静默忽略。
        if (err && err != LIBUSB_ERROR_NO_DEVICE) {
            SPDLOG_ERROR("释放接口 {} 时出错: {}", interface_number, libusb_strerror(err));
        }

        // 重新绑定内核驱动
        err = libusb_attach_kernel_driver(native_handle, interface_number);
        if (err && err != LIBUSB_ERROR_NOT_FOUND && err != LIBUSB_ERROR_NOT_SUPPORTED
                && err != LIBUSB_ERROR_NO_DEVICE) {
            SPDLOG_WARN("重新绑定内核驱动失败 (接口 {}): {}", interface_number, libusb_strerror(err));
        }
    }

    interfaces_claimed_ = false;

    // 关闭 handle
    // 普通模式和 Android 模式都需要调用 libusb_close
    libusb_close(native_handle);
    native_handle = nullptr;

    SPDLOG_INFO("已释放设备接口");
}
