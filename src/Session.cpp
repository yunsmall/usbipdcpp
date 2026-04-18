#include "Session.h"

#include <asio.hpp>
#include <optional>
#include <variant>
#include <spdlog/spdlog.h>

#include "DeviceHandler/DeviceHandler.h"
#include "utils/SmallVector.h"

#include "Server.h"
#include "Device.h"
#include "protocol.h"
#include "network.h"
#include "../include/utils/utils.h"

usbipdcpp::Session::Session(Server &server) :
    server(server),
    socket(session_io_context) {
}

void usbipdcpp::Session::submit_ret_submit_impl(SessionResponse &&submit) {
    // 多线程并发或 busy-wait 模式下都需要加锁保护 write_buffer
    std::lock_guard lock(swap_mutex);
    write_buffer.emplace_back(std::move(submit));
    has_data.store(true, std::memory_order_release);
# ifndef USBIPDCPP_ENABLE_BUSY_WAIT
    data_available_cv.notify_one();
# endif
}

void usbipdcpp::Session::submit_ret_unlink_impl(UsbIpResponse::UsbIpRetUnlink &&unlink) {
    // 多线程并发或 busy-wait 模式下都需要加锁保护 write_buffer
    std::lock_guard lock(swap_mutex);
    write_buffer.emplace_back(SessionResponse{std::move(unlink)});
    has_data.store(true, std::memory_order_release);
# ifndef USBIPDCPP_ENABLE_BUSY_WAIT
    data_available_cv.notify_one();
# endif
}

void usbipdcpp::Session::submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink &&unlink) {
    submit_ret_unlink_impl(std::move(unlink));
}

void usbipdcpp::Session::submit_ret_submit(UsbIpResponse::UsbIpRetSubmit &&submit) {
    submit_session_response(SessionResponse{std::move(submit)});
}

void usbipdcpp::Session::submit_session_response(SessionResponse &&response) {
    submit_ret_submit_impl(std::move(response));
}

usbipdcpp::Session::~Session() {
    SPDLOG_TRACE("Session析构");
}

void usbipdcpp::Session::run() {
    //先获取自身指针，防止被智能指针析构
    auto self = shared_from_this();
    if (server.before_thread_create_callback) {
        server.before_thread_create_callback(ThreadPurpose::SessionMain);
    }
    run_thread = std::thread([self=std::move(self)]() {
        self->parse_op();

        //处理结束后自动往服务器中删除自身并触发退出回调
        self->server.remove_session(self.get());
        //把当前这个线程detach了，防止线程内部析构自己导致报错
        self->run_thread.detach();
    });
    if (server.after_thread_create_callback) {
        server.after_thread_create_callback(ThreadPurpose::SessionMain, run_thread);
    }
}

void usbipdcpp::Session::parse_op() {
    usbipdcpp::error_code ec;
    SPDLOG_TRACE("尝试读取OP");
    auto op = UsbIpCommand::get_op_from_socket(socket, ec);
    if (ec) {
        SPDLOG_DEBUG("从socket中获取op时出错：{}", ec.message());
        if (ec.value() == static_cast<int>(ErrorType::SOCKET_EOF)) {
            SPDLOG_DEBUG("连接关闭");
        }
        else if (ec.value() == static_cast<int>(ErrorType::SOCKET_ERR)) {
            SPDLOG_DEBUG("发生socket错误");
        }

        goto close_socket;
    }
    std::visit([&,this](auto &&cmd) {
        using T = std::remove_cvref_t<decltype(cmd)>;
        if constexpr (std::is_same_v<UsbIpCommand::OpReqDevlist, T>) {
            SPDLOG_TRACE("收到 OpReqDevlist 包");
            data_type to_be_sent;
            {
                std::shared_lock lock(server.devices_mutex);
                to_be_sent = UsbIpResponse::OpRepDevlist::create_from_devices(server.available_devices).to_bytes();
            }
            asio::write(socket, asio::buffer(to_be_sent), ec);
            if (!ec)[[likely]]
                    SPDLOG_TRACE("成功发送 OpRepDevlist 包");
            else
                SPDLOG_TRACE("发送 OpRepDevlist 包出错{}", ec.message());
        }
        else if constexpr (std::is_same_v<UsbIpCommand::OpReqImport, T>) {
            SPDLOG_TRACE("收到 OpReqImport 包");
            auto wanted_busid = std::string(reinterpret_cast<char *>(cmd.busid.data()));
            UsbIpResponse::OpRepImport op_rep_import{};
            SPDLOG_TRACE("客户端想连接busid为 {} 的设备", wanted_busid);

            bool target_device_is_using = false;
            bool open_device_failed = false;
            //已经在使用的不支持导出
            if (server.is_device_using(wanted_busid)) {
                spdlog::warn("正在使用的设备不支持导出");
                //查看内核源码中 tools/usbip/src/usbipd.c 函数 recv_request_import 源码可以发现应该返回NA而不是DevBusy
                op_rep_import = UsbIpResponse::OpRepImport::create_on_failure_with_status(
                        static_cast<std::uint32_t>(OperationStatuType::NA));
                target_device_is_using = true;
            }
            else {
                if (auto using_device = server.try_moving_device_to_using(wanted_busid)) {
                    std::lock_guard lock(current_import_device_data_mutex);
                    spdlog::info("成功将设备放入正在使用的设备中");
                    current_import_device_id = wanted_busid;
                    //将当前使用的设备指向这个设备
                    current_import_device = using_device;
                    spdlog::info("成功缓存正在使用的设备");

                    // 在发送 OpRepImport 之前尝试打开设备
                    usbipdcpp::error_code open_ec;
                    current_import_device->on_new_connection(*this, open_ec);
                    if (open_ec) {
                        SPDLOG_ERROR("打开设备失败: {}", open_ec.message());
                        open_device_failed = true;
                        // 将设备移回可用列表
                        server.try_moving_device_to_available(wanted_busid);
                        current_import_device.reset();
                        current_import_device_id.reset();
                    }
                }
            }

            if (!target_device_is_using && !open_device_failed) {
                std::shared_lock lock(current_import_device_data_mutex);
                if (current_import_device) {
                    spdlog::info("找到目标设备，可以导入");
                    op_rep_import = UsbIpResponse::OpRepImport::create_on_success(current_import_device);
                    cmd_transferring = true;
                }
                else {
                    spdlog::info("不存在目标设备，不可导入");
                    op_rep_import = UsbIpResponse::OpRepImport::create_on_failure_with_status(
                            static_cast<std::uint32_t>(OperationStatuType::NoDev));
                }
            }
            else if (open_device_failed) {
                op_rep_import = UsbIpResponse::OpRepImport::create_on_failure_with_status(
                        static_cast<std::uint32_t>(OperationStatuType::NA));
            }

            auto to_be_sent = op_rep_import.to_bytes();
            SPDLOG_TRACE("即将向服务器发送{}，共{}字节", get_every_byte(to_be_sent), to_be_sent.size());
            [[maybe_unused]] auto size = asio::write(socket, asio::buffer(to_be_sent), ec);
            if (!ec)[[likely]]
                    SPDLOG_TRACE("成功发送 OpRepImport 包", size);
            else
                SPDLOG_TRACE("发送 OpRepImport 包出错{}", ec.message());

            if (cmd_transferring) {
                usbipdcpp::error_code transferring_ec;
                //进入通信状态
                transfer_loop(transferring_ec);
                if (transferring_ec) {
                    SPDLOG_ERROR("Error occurred during transferring : {}", transferring_ec.message());
                    ec = transferring_ec;
                }

                // on_disconnection 和设备清理已在 receiver 中处理
            }
        }
        else {
            //确保处理了所有可能类型
            static_assert(!std::is_same_v<T, T>);
        }
    }, op);

close_socket:
    std::error_code ignore_ec;
    SPDLOG_INFO("尝试关闭socket");
    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignore_ec);
    socket.close(ignore_ec);
}

void usbipdcpp::Session::immediately_stop() {
    should_immediately_stop = true;

    std::error_code ignore_ec;
    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignore_ec);
    SPDLOG_INFO("成功调用shutdown");
}

void usbipdcpp::Session::transfer_loop(usbipdcpp::error_code &transferring_ec) {
    // on_new_connection 已在 parse_op 中调用，此处不再调用

    error_code receiver_ec;
    error_code sender_ec;
    if (server.before_thread_create_callback) {
        server.before_thread_create_callback(ThreadPurpose::SessionSender);
    }
    std::thread sender_thread([&,this]() {
        sender(sender_ec);
    });
    if (server.after_thread_create_callback) {
        server.after_thread_create_callback(ThreadPurpose::SessionSender, sender_thread);
    }

    receiver(receiver_ec);
    SPDLOG_INFO("receiver退出");
    sender_thread.join();
    SPDLOG_INFO("sender thread退出");

    if (sender_ec) {
        SPDLOG_ERROR("An error occur during sending: {}", sender_ec.message());
        transferring_ec = sender_ec;
    }
    //一般来说receiver_ec的ec重要一点，因此会覆盖掉
    else if (receiver_ec) {
        SPDLOG_ERROR("An error occur during receiving: {}", receiver_ec.message());
        transferring_ec = receiver_ec;
    }
    cmd_transferring = false;
}

std::optional<usbipdcpp::SessionResponse> usbipdcpp::Session::sender_get_data(usbipdcpp::error_code &ec) {
    // 如果 read_buffer 为空，尝试交换
    if (read_buffer.empty())[[likely]] {
        std::unique_lock lock(swap_mutex);
# ifndef USBIPDCPP_ENABLE_BUSY_WAIT
        // 非 busy-wait: 使用条件变量等待数据
        data_available_cv.wait(lock, [this]() {
            return has_data.load(std::memory_order_acquire) || should_immediately_stop;
        });
# endif
        if (!write_buffer.empty()) {
            read_buffer.swap(write_buffer);
            has_data.store(false, std::memory_order_release);
        }
    }
    if (!read_buffer.empty())[[likely]] {
        usbipdcpp::SessionResponse ret_v = std::move(read_buffer.front());
        read_buffer.pop_front();
        return ret_v;
    }
    else {
        return std::nullopt;
    }
}

void usbipdcpp::Session::receiver(usbipdcpp::error_code &receiver_ec) {
    // spdlog::info("should_immediately_stop:{}", should_immediately_stop.load());
    while (!should_immediately_stop) {
        usbipdcpp::error_code ec;

        auto command = UsbIpCommand::get_cmd_from_socket(socket, ec);
        if (ec)[[unlikely]] {
            if (ec.value() == static_cast<int>(ErrorType::SOCKET_EOF)) {
                SPDLOG_DEBUG("连接关闭");
            }
            else if (ec.value() == static_cast<int>(ErrorType::SOCKET_ERR)) {
                SPDLOG_DEBUG("发生socket错误");
            }
            else {
                SPDLOG_ERROR("从socket中获取命令时出错：{}", ec.message());
            }
            break;
        }
        if (should_immediately_stop)[[unlikely]]
            break;
        std::visit([&,this](auto &&cmd) {
            using T = std::remove_cvref_t<decltype(cmd)>;
            if constexpr (std::is_same_v<UsbIpCommand::UsbIpCmdSubmit, T>) {
                UsbIpCommand::UsbIpCmdSubmit &cmd2 = cmd;
                LATENCY_TRACK_START(latency_tracker, cmd2.header.seqnum);
                SPDLOG_TRACE("收到 UsbIpCmdSubmit 包，序列号: {}", cmd2.header.seqnum);
                auto out = cmd2.header.direction == UsbIpDirection::Out;
                SPDLOG_TRACE("Usbip传输方向为：{}", out ? "out" : "in");
                std::uint8_t real_ep = out
                                           ? static_cast<std::uint8_t>(cmd2.header.ep)
                                           : (static_cast<std::uint8_t>(cmd2.header.ep) | 0x80);
                SPDLOG_TRACE("传输的真实端口为 {:02x}", real_ep);
                auto current_seqnum = cmd2.header.seqnum;

                auto ep_find_ret = current_import_device->find_ep(real_ep);
                if (ep_find_ret.has_value())[[likely]] {
                    auto &ep = ep_find_ret->first;
                    auto &intf = ep_find_ret->second;

                    SPDLOG_TRACE("->端口{0:02x}", ep.address);
                    SPDLOG_TRACE("->setup数据{}", get_every_byte(cmd2.setup.to_bytes()));
                    SPDLOG_TRACE("->请求数据{}", get_every_byte(cmd2.data));


                    usbipdcpp::error_code ec_during_handling_urb;
                    // start_processing_urb();
                    LATENCY_TRACK(latency_tracker, cmd2.header.seqnum, "准备传入设备handle_urb");
                    current_import_device->handle_urb(
                            cmd2,
                            current_seqnum,
                            ep,
                            intf,
                            cmd2.transfer_buffer_length, cmd2.setup, std::move(cmd2.data),
                            std::move(cmd2.iso_packet_descriptor),
                            ec_during_handling_urb
                            );

                    if (ec_during_handling_urb)[[unlikely]] {
                        SPDLOG_ERROR("Error during handling urb : {}", ec_during_handling_urb.message());
                        //发生错误代表已经不能继续通信了
                        receiver_ec = ec_during_handling_urb;
                        should_immediately_stop = true;
                        return;
                    }
                }
                else {
                    SPDLOG_WARN("找不到端点{}", real_ep);
                    UsbIpResponse::UsbIpRetSubmit ret_submit =
                            UsbIpResponse::UsbIpRetSubmit::create_ret_submit_epipe_without_data(
                                    cmd2.header.seqnum, 0);
                    ret_submit.to_socket(socket, ec);
                    SPDLOG_TRACE("成功发送 UsbIpRetSubmit 包");
                }
            }
            else if constexpr (std::is_same_v<UsbIpCommand::UsbIpCmdUnlink, T>) {
                UsbIpCommand::UsbIpCmdUnlink &cmd2 = cmd;
                SPDLOG_TRACE("收到 UsbIpCmdUnlink 包，序列号: {}", cmd2.header.seqnum);

                current_import_device->handle_unlink_seqnum(cmd2.unlink_seqnum, cmd2.header.seqnum);
            }
            else if constexpr (std::is_same_v<std::monostate, T>) {
                SPDLOG_ERROR("收到未知包");
                receiver_ec = make_error_code(ErrorType::UNKNOWN_CMD);
            }
            else {
                //确保处理了所有可能类型
                static_assert(!std::is_same_v<T, T>);
            }
            return;
        }, command);
    }
    //通知设备断连，告诉设备禁止再发消息
    current_import_device->on_disconnection(receiver_ec);
    //然后再关闭发送线程，防止先关闭了但设备因还未被通知到关闭而报错
    should_immediately_stop = true;
# ifndef USBIPDCPP_ENABLE_BUSY_WAIT
    data_available_cv.notify_one();
# endif

    /* 这里先标记为可用是可行的
     * 一是设备on_disconnection需要阻塞，把自身断连需要做的事全处理掉
     * 二是这个session马上就要析构了current_import_device的那两个变量不会重新被使用
     * 因此先标记为可用再清除这两个变量的状态
    */
    if (current_import_device->is_device_removed()) {
        // 设备已物理拔出，直接从 using_devices 移除
        SPDLOG_INFO("设备已物理拔出，不再移回可用列表");
        std::lock_guard lock(server.get_devices_mutex());
        server.get_using_devices().erase(*current_import_device_id);
    }
    else {
        server.try_moving_device_to_available(*current_import_device_id);
    }
    current_import_device_id.reset();
    current_import_device.reset();
    SPDLOG_TRACE("将当前导入设备的busid设为空");
}

void usbipdcpp::Session::sender(usbipdcpp::error_code &ec) {
# ifdef USBIPDCPP_ENABLE_BUSY_WAIT
    // busy-wait 模式：批量发送所有完成的数据
    while (!should_immediately_stop) {
        // 执行回调（如 libusb 事件处理）
        if (server.busy_wait_callback) {
            server.busy_wait_callback();
        }

        // 批量获取所有待发送数据（双缓冲交换）
        SmallVector<SessionResponse, 32> batch;
        {
            std::lock_guard lock(swap_mutex);
            if (!write_buffer.empty()) {
                read_buffer.swap(write_buffer);
                has_data.store(false, std::memory_order_release);
            }
        }
        // 从 read_buffer 批量获取数据
        while (!read_buffer.empty()) {
            batch.emplace_back(std::move(read_buffer.front()));
            read_buffer.pop_front();
        }

        // 发送所有数据，每发送一个就处理一次事件以捕获新完成的 transfer
        for (auto &send_data: batch) {
            error_code sending_ec;
            std::visit([&](auto &&cmd) {
                using T = std::remove_cvref_t<decltype(cmd)>;
                if constexpr (std::is_same_v<UsbIpResponse::UsbIpRetSubmit, T>) {
                    cmd.to_socket(socket, sending_ec);
                    LATENCY_TRACK_END_MSG(latency_tracker, cmd.header.seqnum, "to_socket调用结束");
                }
                else if constexpr (std::is_same_v<UsbIpResponse::UsbIpRetUnlink, T>) {
                    cmd.to_socket(socket, sending_ec);
                    LATENCY_TRACK_END_MSG(latency_tracker, cmd.header.seqnum, "to_socket调用结束");
                }
                else if constexpr (std::is_same_v<std::monostate, T>) {
                    SPDLOG_ERROR("收到未知包");
                    sending_ec = make_error_code(ErrorType::UNKNOWN_CMD);
                }
                else {
                    static_assert(!std::is_same_v<T, T>);
                }
            }, send_data.response);

            // 清理资源（在发送线程中执行，减少回调工作量）
            if (send_data.cleanup_func) {
                send_data.cleanup_func(send_data.cleanup_context, send_data.cleanup_transfer);
            }

            if (sending_ec) {
                break;
            }

            // 发送完后立即处理事件，确保新完成的 transfer 能被及时捕获
            if (server.busy_wait_callback) {
                server.busy_wait_callback();
            }
        }

        if (batch.empty()) {
            continue;
        }
    }

    // 退出后继续处理事件，直到所有传输完成
    while (current_import_device && current_import_device->has_pending_transfers() && server.busy_wait_callback) {
        server.busy_wait_callback();
    }
# else
    // 非 busy-wait 模式：原有逻辑
    while (!should_immediately_stop) {
        auto send_data_opt = sender_get_data(ec);
        if (ec || should_immediately_stop)[[unlikely]] {
            break;
        }
        if (!send_data_opt.has_value())[[unlikely]] {
            break;
        }
        auto send_data = std::move(send_data_opt.value());

        SPDLOG_TRACE("channel收到消息");
        error_code sending_ec;
        std::visit([&](auto &&cmd) {
            using T = std::remove_cvref_t<decltype(cmd)>;
            if constexpr (std::is_same_v<UsbIpResponse::UsbIpRetSubmit, T>) {
                cmd.to_socket(socket, sending_ec);
                LATENCY_TRACK_END_MSG(latency_tracker, cmd.header.seqnum, "to_socket调用结束");
            }
            else if constexpr (std::is_same_v<UsbIpResponse::UsbIpRetUnlink, T>) {
                cmd.to_socket(socket, sending_ec);
                LATENCY_TRACK_END_MSG(latency_tracker, cmd.header.seqnum, "to_socket调用结束");
            }
            else if constexpr (std::is_same_v<std::monostate, T>) {
                SPDLOG_ERROR("收到未知包");
                sending_ec = make_error_code(ErrorType::UNKNOWN_CMD);
            }
            else {
                static_assert(!std::is_same_v<T, T>);
            }
        }, send_data.response);

        // 清理资源（在发送线程中执行，减少回调工作量）
        if (send_data.cleanup_func) {
            send_data.cleanup_func(send_data.cleanup_context, send_data.cleanup_transfer);
        }

        if (sending_ec) {
            // 直接不处理发送过程中的错误
            // ec = sending_ec;
            break;
        }
    }
# endif
}
