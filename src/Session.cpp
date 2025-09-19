#include "Session.h"

#include <asio.hpp>
#include <spdlog/spdlog.h>
#include <asio/experimental/parallel_group.hpp>
#include <asio/experimental/awaitable_operators.hpp>

#include "Server.h"
#include "device.h"
#include "protocol.h"
#include "utils.h"

usbipdcpp::Session::Session(Server &server):
    server(server),
    reading_pause_channel(session_io_context),
    no_urb_processing_notify_channel(session_io_context),
    socket(session_io_context) {
}

std::tuple<bool, std::uint32_t> usbipdcpp::Session::get_unlink_seqnum(std::uint32_t seqnum) {
    std::shared_lock lock(unlink_map_mutex);
    if (unlink_map.contains(seqnum)) {
        return {true, unlink_map[seqnum]};
    }
    return {false, 0};
}

void usbipdcpp::Session::remove_seqnum_unlink(std::uint32_t seqnum) {
    std::lock_guard lock(unlink_map_mutex);
    unlink_map.erase(seqnum);
}


void usbipdcpp::Session::submit_ret_unlink_and_then_remove_seqnum_unlink(UsbIpResponse::UsbIpRetUnlink &&unlink,
                                                                         std::uint32_t seqnum) {
    submit_ret_unlink(std::move(unlink));
    remove_seqnum_unlink(seqnum);
}

void usbipdcpp::Session::submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink &&unlink) {
    SPDLOG_DEBUG("收到提交的unlink包 {}", unlink.header.seqnum);
    // #ifdef TRANSFER_DELAY_RECORD
    //     {
    //         std::shared_lock lock(time_record_mutex);
    //         spdlog::trace("{}unlink被提交经历了{}微秒", unlink.header.seqnum,
    //                       std::chrono::duration_cast<std::chrono::microseconds>(
    //                               std::chrono::steady_clock::now() - time_record[unlink.header.seqnum]).count());
    //     }
    // #endif

    error_code send_ec;
    transfer_channel->async_send(send_ec, UsbIpResponse::RetVariant{std::move(unlink)}, asio::detached);

    //     //从其他线程提交任务到io_context的run线程
    //     asio::co_spawn(server.asio_io_context, [this,unlink=std::move(unlink)]()-> asio::awaitable<void> {
    //
    // #if SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_TRACE
    //         {
    //             auto to_be_sent = unlink.to_bytes();
    //             SPDLOG_TRACE("尝试向服务器发送 UsbIpRetUnlink 包 {}，序列号：{}，共{}字节", get_every_byte(to_be_sent),
    //                          unlink.header.seqnum,
    //                          to_be_sent.size());
    //         }
    // #endif
    //
    //
    //         asio::error_code write_ec;
    //         co_await unlink.to_socket(socket, write_ec);
    //         if (write_ec) {
    //             SPDLOG_ERROR("尝试发送 UsbIpRetUnlink 包时出错：{}", write_ec.message());
    //         }
    //         else {
    //             SPDLOG_TRACE("成功发送 UsbIpRetUnlink 包");
    //         }
    //
    //         end_processing_urb();
    //     }, if_has_value_than_rethrow);
}

void usbipdcpp::Session::submit_ret_submit(UsbIpResponse::UsbIpRetSubmit &&submit) {
    SPDLOG_DEBUG("收到提交的submit包{}", submit.header.seqnum);
    // #ifdef TRANSFER_DELAY_RECORD
    //     {
    //         std::shared_lock lock(time_record_mutex);
    //         spdlog::trace("{}submit被提交经历了{}微秒", submit.header.seqnum,
    //                       std::chrono::duration_cast<std::chrono::microseconds>(
    //                               std::chrono::steady_clock::now() - time_record[submit.header.seqnum]).count());
    //     }
    // #endif

    error_code send_ec;
    transfer_channel->async_send(send_ec, UsbIpResponse::RetVariant{std::move(submit)}, asio::detached);

    //从其他线程提交任务到io_context的run线程
    //     asio::co_spawn(server.asio_io_context, [this,submit=std::move(submit)]()-> asio::awaitable<void> {
    //
    // #if SPDLOG_ACTIVE_LEVEL <= SPDLOG_LEVEL_TRACE
    //         {
    //             auto to_be_sent = submit.to_bytes();
    //             SPDLOG_TRACE("尝试向服务器发送 UsbIpRetSubmit 包{}，序列号: {}，共{}字节", get_every_byte(to_be_sent),
    //                          submit.header.seqnum,
    //                          to_be_sent.size());
    //         }
    // #endif
    // #ifdef TRANSFER_DELAY_RECORD
    //         {
    //             std::lock_guard lock(time_record_mutex);
    //             spdlog::trace("{}submit传输经历了{}微秒", submit.header.seqnum,
    //                           std::chrono::duration_cast<std::chrono::microseconds>(
    //                                   std::chrono::steady_clock::now() - time_record[submit.header.seqnum]).count());
    //             time_record.erase(submit.header.seqnum);
    //         }
    // #endif
    //         asio::error_code write_ec;
    //         co_await submit.to_socket(socket, write_ec);
    //         if (write_ec) {
    //             SPDLOG_ERROR("尝试发送 UsbIpRetSubmit 包时出错：{}", write_ec.message());
    //         }
    //         else {
    //             SPDLOG_TRACE("成功发送 UsbIpRetSubmit 包");
    //         }
    //         end_processing_urb();
    //     }, if_has_value_than_rethrow);
}

usbipdcpp::Session::~Session() {
    SPDLOG_TRACE("Session析构");
}

void usbipdcpp::Session::run() {
    auto self = shared_from_this();
    asio::co_spawn(session_io_context, [this]() {
        return parse_op();
    }, if_has_value_than_rethrow);

    SPDLOG_TRACE("创建Session线程");
    //这个线程结束后自动析构this
    run_thread = std::thread([self=std::move(self)]() {
        self->session_io_context.run();

        //处理结束后自动往服务器中删除自身

        {
            std::lock_guard lock(self->server.session_list_mutex);
            for (auto it = self->server.sessions.begin(); it != self->server.sessions.end();) {
                if (auto s = it->lock()) {
                    if (s == self) {
                        it = self->server.sessions.erase(it);
                        break;
                    }
                    else {
                        ++it;
                    }
                }
                else {
                    // 清除已失效的 weak_ptr
                    it = self->server.sessions.erase(it);
                }
            }
        }
        //把当前这个线程detach了，防止线程内部析构自己导致报错
        self->run_thread.detach();
    });
}

asio::awaitable<void> usbipdcpp::Session::parse_op() {
    usbipdcpp::error_code ec;
    SPDLOG_TRACE("尝试读取OP");
    auto op = co_await UsbIpCommand::get_op_from_socket(socket, ec);
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
    co_await std::visit([&,this](auto &&cmd)-> asio::awaitable<void> {
        using T = std::remove_cvref_t<decltype(cmd)>;
        if constexpr (std::is_same_v<UsbIpCommand::OpReqDevlist, T>) {
            SPDLOG_TRACE("收到 OpReqDevlist 包");
            data_type to_be_sent;
            {
                std::shared_lock lock(server.devices_mutex);
                to_be_sent = UsbIpResponse::OpRepDevlist::create_from_devices(server.available_devices).to_bytes();
            }
            co_await asio::async_write(socket, asio::buffer(to_be_sent), asio::use_awaitable);
            SPDLOG_TRACE("成功发送 OpRepDevlist 包");
        }
        else if constexpr (std::is_same_v<UsbIpCommand::OpReqImport, T>) {
            SPDLOG_TRACE("收到 OpReqImport 包");
            auto wanted_busid = std::string(reinterpret_cast<char *>(cmd.busid.data()));
            UsbIpResponse::OpRepImport op_rep_import{};
            SPDLOG_TRACE("客户端想连接busid为 {} 的设备", wanted_busid);

            bool target_device_is_using = false;
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
                    //从这里开始会一直占用锁
                    std::lock_guard lock(current_import_device_data_mutex);
                    spdlog::info("成功将设备放入正在使用的设备中");
                    current_import_device_id = wanted_busid;
                    //将当前使用的设备指向这个设备
                    current_import_device = using_device;
                    spdlog::info("成功缓存正在使用的设备");
                }
            }

            std::shared_lock lock(current_import_device_data_mutex);
            if (!target_device_is_using) {
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
                auto to_be_sent = op_rep_import.to_bytes();
                [[maybe_unused]] auto size = co_await asio::async_write(socket, asio::buffer(to_be_sent),
                                                                        asio::use_awaitable);
                SPDLOG_TRACE("即将向服务器发送{}，共{}字节", get_every_byte(to_be_sent), to_be_sent.size());
                SPDLOG_TRACE("成功发送 OpRepImport 包", size);
            }

            if (cmd_transferring) {
                usbipdcpp::error_code transferring_ec;
                //进入通信状态
                co_await transfer_loop(transferring_ec);
                if (transferring_ec) {
                    SPDLOG_ERROR("Error occurred during transferring : {}", transferring_ec.message());
                    ec = transferring_ec;
                }
            }
        }
        else if constexpr (std::is_same_v<std::monostate, T>) {
            SPDLOG_ERROR("收到未知包");
            ec = make_error_code(ErrorType::UNKNOWN_CMD);
        }
        else {
            //确保处理了所有可能类型
            static_assert(!std::is_same_v<T, T>);
        }
    }, op);

    if (this->transfer_channel) {
        this->transfer_channel->close();
    }

close_socket:
    std::error_code ignore_ec;
    SPDLOG_INFO("尝试关闭socket");
    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignore_ec);
    socket.close(ignore_ec);
}

void usbipdcpp::Session::immediately_stop() {
    should_immediately_stop = true;

    asio::post(session_io_context,
               [this]() {
                   //关闭后才会析构，直接使用this安全
                   this->socket.close();
                   if (this->transfer_channel) {
                       this->transfer_channel->close();
                   }
               });

    // SPDLOG_TRACE("session stop");
    // {
    //     std::shared_lock lock(current_import_device_data_mutex);
    //     // if (current_import_device) {
    //     //     current_import_device->stop_transfer();
    //     // }
    // }
}

asio::awaitable<void> usbipdcpp::Session::transfer_loop(usbipdcpp::error_code &transferring_ec) {
    current_import_device->on_new_connection(transferring_ec);
    if (transferring_ec)
        co_return;

    error_code receiver_ec;
    error_code sender_ec;
    using namespace asio::experimental::awaitable_operators;

    transfer_channel = std::make_unique<transfer_channel_type>(server.asio_io_context, transfer_channel_size);

    co_await (receiver(receiver_ec) && sender(sender_ec));

    //能够返回ec就行了，哪的ec不重要
    if (sender_ec) {
        transferring_ec = sender_ec;
    }
    else if (receiver_ec) {
        transferring_ec = sender_ec;
    }
    cmd_transferring = false;
}

asio::awaitable<void> usbipdcpp::Session::receiver(usbipdcpp::error_code &receiver_ec) {
    spdlog::info("should_immediately_stop:{}", should_immediately_stop.load());
    while (!should_immediately_stop) {
        usbipdcpp::error_code ec;

        //如果不能读的时候，阻塞当前协程直到能获取到消息
        if (!can_read) {
            SPDLOG_DEBUG("receive被阻塞了");
            co_await reading_pause_channel.async_receive(asio::use_awaitable);
            SPDLOG_DEBUG("receive收到消息，恢复了");
        }

        auto command = co_await UsbIpCommand::get_cmd_from_socket(socket, ec);
        if (ec) {
            SPDLOG_DEBUG("从socket中获取命令时出错：{}", ec.message());
            if (ec.value() == static_cast<int>(ErrorType::SOCKET_EOF)) {
                SPDLOG_DEBUG("连接关闭");
            }
            else if (ec.value() == static_cast<int>(ErrorType::SOCKET_ERR)) {
                SPDLOG_DEBUG("发生socket错误");
            }
            break;
        }
        else {
            if (should_immediately_stop)
                break;
            co_await std::visit([&,this](auto &&cmd)-> asio::awaitable<void> {
                using T = std::remove_cvref_t<decltype(cmd)>;
                if constexpr (std::is_same_v<UsbIpCommand::UsbIpCmdSubmit, T>) {
                    UsbIpCommand::UsbIpCmdSubmit &cmd2 = cmd;
                    SPDLOG_TRACE("收到 UsbIpCmdSubmit 包，序列号: {}", cmd2.header.seqnum);
                    auto out = cmd2.header.direction == UsbIpDirection::Out;
                    SPDLOG_TRACE("Usbip传输方向为：{}", out ? "out" : "in");
                    std::uint8_t real_ep = out
                                               ? static_cast<std::uint8_t>(cmd2.header.ep)
                                               : (static_cast<std::uint8_t>(cmd2.header.ep) | 0x80);
                    SPDLOG_TRACE("传输的真实端口为 {:02x}", real_ep);
                    auto current_seqnum = cmd2.header.seqnum;

                    auto ep_find_ret = current_import_device->find_ep(real_ep);

                    if (ep_find_ret.has_value()) {
                        auto &ep = ep_find_ret->first;
                        auto &intf = ep_find_ret->second;

                        SPDLOG_TRACE("->端口{0:02x}", ep.address);
                        SPDLOG_TRACE("->setup数据{}", get_every_byte(cmd2.setup.to_bytes()));
                        SPDLOG_TRACE("->请求数据{}", get_every_byte(cmd2.data));

#ifdef TRANSFER_DELAY_RECORD
                        if (ep.attributes == static_cast<std::uint8_t>(EndpointAttributes::Control)) {
                            spdlog::trace("{}为控制传输", cmd2.header.seqnum);
                        }
                        else if (ep.attributes == static_cast<std::uint8_t>(EndpointAttributes::Interrupt)) {
                            spdlog::trace("{}为中断传输", cmd2.header.seqnum);
                        }
                        else if (ep.attributes == static_cast<std::uint8_t>(EndpointAttributes::Bulk)) {
                            spdlog::trace("{}为块传输", cmd2.header.seqnum);
                        }
                        else {
                            spdlog::trace("{}为等时传输", cmd2.header.seqnum);
                        }
#endif

                        usbipdcpp::error_code ec_during_handling_urb;
                        // start_processing_urb();
                        current_import_device->handle_urb(
                                *this,
                                cmd2,
                                current_seqnum,
                                ep,
                                intf, cmd2.transfer_buffer_length, cmd2.setup, cmd2.data,
                                cmd2.iso_packet_descriptor, ec_during_handling_urb
                                );

                        if (ec_during_handling_urb) {
                            SPDLOG_ERROR("Error during handling urb : {}", ec_during_handling_urb.message());
                            //发生错误代表已经不能继续通信了
                            receiver_ec = ec_during_handling_urb;
                            should_immediately_stop = true;
                            co_return;
                        }
                    }
                    else {
                        SPDLOG_WARN("找不到端点{}", real_ep);
                        UsbIpResponse::UsbIpRetSubmit ret_submit;
                        ret_submit = UsbIpResponse::UsbIpRetSubmit::usbip_ret_submit_fail_with_status(
                                cmd2.header.seqnum,EPIPE);
                        auto to_be_sent = ret_submit.to_bytes();
                        SPDLOG_TRACE("即将向服务器发送{}，共{}字节", get_every_byte(to_be_sent), to_be_sent.size());
                        co_await asio::async_write(socket, asio::buffer(to_be_sent), asio::use_awaitable);
                        SPDLOG_TRACE("成功发送 UsbIpRetSubmit 包");
                    }
                }
                else if constexpr (std::is_same_v<UsbIpCommand::UsbIpCmdUnlink, T>) {
                    UsbIpCommand::UsbIpCmdUnlink &cmd2 = cmd;
                    SPDLOG_TRACE("收到 UsbIpCmdUnlink 包，序列号: {}", cmd2.header.seqnum);

                    {
                        std::lock_guard lock(unlink_map_mutex);
                        unlink_map.emplace(cmd2.unlink_seqnum, cmd2.header.seqnum);
                    }
                    current_import_device->handle_unlink_seqnum(cmd2.unlink_seqnum);
                }
                else if constexpr (std::is_same_v<std::monostate, T>) {
                    SPDLOG_ERROR("收到未知包");
                    receiver_ec = make_error_code(ErrorType::UNKNOWN_CMD);
                }
                else {
                    //确保处理了所有可能类型
                    static_assert(!std::is_same_v<T, T>);
                }
                co_return;
            }, command);
        }
    }
    //先取消设备传输再等待urb全部处理好
    transfer_channel->close();
    current_import_device->on_disconnection(receiver_ec);
    server.try_moving_device_to_available(*current_import_device_id);
    current_import_device_id.reset();
    current_import_device.reset();
    SPDLOG_TRACE("将当前导入设备的busid设为空");

    // if (!should_immediately_stop) {
        // 没发生严重错误或者外部要求的stop才进行等待
        // socket断连表明客户端执行了usbip detach，正常断开时会进行等待
        // co_await wait_for_all_urb_processed();
    // }

}

asio::awaitable<void> usbipdcpp::Session::sender(usbipdcpp::error_code &ec) {
    while (!should_immediately_stop) {
        //不停从channel中获取数据
        auto send_data = co_await transfer_channel->async_receive(asio::redirect_error(asio::use_awaitable, ec));
        if (ec) {
            break;
        }
        SPDLOG_TRACE("channel收到消息");
        error_code sending_ec;
        co_await std::visit([&](auto &&cmd)-> asio::awaitable<void> {
            using T = std::remove_cvref_t<decltype(cmd)>;
            if constexpr (std::is_same_v<UsbIpResponse::UsbIpRetSubmit, T>) {
                co_await cmd.to_socket(socket, sending_ec);
                // end_processing_urb();
            }
            else if constexpr (std::is_same_v<UsbIpResponse::UsbIpRetUnlink, T>) {
                co_await cmd.to_socket(socket, sending_ec);
                // end_processing_urb();
            }
            else if constexpr (std::is_same_v<std::monostate, T>) {
                SPDLOG_ERROR("收到未知包");
                sending_ec = make_error_code(ErrorType::UNKNOWN_CMD);
            }
            else {
                //确保处理了所有可能类型
                static_assert(!std::is_same_v<T, T>);
            }
        }, send_data);
        if (sending_ec) {
            // 直接不处理发送过程中的错误
            // ec = sending_ec;
            break;
        }
    }
    //防止reciever阻塞
    // no_urb_processing_notify_channel.try_send(asio::error_code{});

    if (ec == asio::experimental::error::channel_closed || ec == asio::experimental::error::channel_cancelled) {
        SPDLOG_DEBUG("sender ec:{}",ec.message());
        ec.clear();
    }
}

// void usbipdcpp::Session::pause_receive() {
//     SPDLOG_DEBUG("暂停receive");
//     can_read = false;
// }
//
// void usbipdcpp::Session::resume_receive() {
//     SPDLOG_DEBUG("恢复receive");
//     can_read = true;
//     reading_pause_channel.async_send(asio::error_code{}, asio::detached);
// }

// void usbipdcpp::Session::start_processing_urb() {
//     SPDLOG_TRACE("开始处理urb时，urb_processing_counter:{}", urb_processing_counter);
//     urb_processing_counter++;
// }
//
// void usbipdcpp::Session::end_processing_urb() {
//     if (--urb_processing_counter == 0) {
//         no_urb_processing_notify_channel.try_send(asio::error_code{});
//     }
//     SPDLOG_TRACE("结束处理urb后,urb_processing_counter:{}", urb_processing_counter);
// }

asio::awaitable<void> usbipdcpp::Session::wait_for_all_urb_processed() {
    if (urb_processing_counter != 0) {
        SPDLOG_DEBUG("当前还有{}个URB正在传输，等待其传输完", urb_processing_counter);
        co_await no_urb_processing_notify_channel.async_receive(asio::use_awaitable);
        SPDLOG_TRACE("收到空消息，结束等待");
    }
    else {
        SPDLOG_TRACE("无正在传输的urb，无需等待");
    }
    SPDLOG_DEBUG("结束等待");
}
