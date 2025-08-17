#include "Session.h"

#include <asio.hpp>
#include <spdlog/spdlog.h>


#include "Server.h"
#include "device.h"
#include "protocol.h"
#include "utils.h"

usbipdcpp::Session::Session(Server &server, asio::ip::tcp::socket &&socket):
    server(server), socket(std::move(socket)), no_urb_processing_notify_channel(server.asio_io_context) {
}

asio::awaitable<void> usbipdcpp::Session::run(usbipdcpp::error_code &ec) {
    usbipdcpp::error_code ec2;
    SPDLOG_TRACE("尝试读取OP");
    auto op = co_await UsbIpCommand::get_op_from_socket(socket, ec2);
    if (ec2) {
        SPDLOG_DEBUG("从socket中获取op时出错：{}", ec2.message());
        if (ec2.value() == static_cast<int>(ErrorType::SOCKET_EOF)) {
            SPDLOG_DEBUG("连接关闭");
        }
        else if (ec2.value() == static_cast<int>(ErrorType::SOCKET_ERR)) {
            SPDLOG_DEBUG("发生socket错误");
        }
        else {
            ec = ec2;
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
                op_rep_import = UsbIpResponse::OpRepImport::create_on_failure_with_status(
                        static_cast<std::uint32_t>(OperationStatuType::DevBusy));
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

close_socket:
    std::error_code ignore_ec;
    SPDLOG_DEBUG("尝试关闭socket");
    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignore_ec);
    socket.close(ignore_ec);
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

void usbipdcpp::Session::immediately_stop() {
    should_immediately_stop = true;

    socket.close();
    SPDLOG_TRACE("session stop");
    // {
    //     std::shared_lock lock(current_import_device_data_mutex);
    //     // if (current_import_device) {
    //     //     current_import_device->stop_transfer();
    //     // }
    // }
}

void usbipdcpp::Session::submit_ret_unlink_and_then_remove_seqnum_unlink(UsbIpResponse::UsbIpRetUnlink &&unlink,
                                                                         std::uint32_t seqnum) {
    submit_ret_unlink(std::move(unlink));
    remove_seqnum_unlink(seqnum);
}

void usbipdcpp::Session::submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink &&unlink) {
    SPDLOG_TRACE("收到提交的unlink包");
    //从其他线程提交任务到io_context的run线程
    asio::co_spawn(server.asio_io_context, [this,unlink=std::move(unlink)]()-> asio::awaitable<void> {
        auto to_be_sent = unlink.to_bytes();
        SPDLOG_TRACE("尝试向服务器发送 UsbIpRetUnlink 包 {}，序列号：{}，共{}字节", get_every_byte(to_be_sent),
                     unlink.header.seqnum,
                     to_be_sent.size());
        asio::error_code write_ec;
        co_await asio::async_write(socket, asio::buffer(to_be_sent),
                                   asio::redirect_error(asio::use_awaitable, write_ec));
        if (write_ec) {
            SPDLOG_ERROR("尝试发送 UsbIpRetUnlink 包时出错：{}", write_ec.message());
        }
        else {
            SPDLOG_TRACE("成功发送 UsbIpRetUnlink 包");
        }
        end_processing_urb();
    }, if_has_value_than_rethrow);
}

void usbipdcpp::Session::submit_ret_submit(UsbIpResponse::UsbIpRetSubmit &&submit) {
    SPDLOG_TRACE("收到提交的submit包");
    //从其他线程提交任务到io_context的run线程
    asio::co_spawn(server.asio_io_context, [this,submit=std::move(submit)]()-> asio::awaitable<void> {
        auto to_be_sent = submit.to_bytes();
        SPDLOG_TRACE("尝试向服务器发送 UsbIpRetSubmit 包{}，序列号: {}，共{}字节", get_every_byte(to_be_sent),
                     submit.header.seqnum,
                     to_be_sent.size());
        asio::error_code write_ec;
        co_await asio::async_write(socket, asio::buffer(to_be_sent),
                                   asio::redirect_error(asio::use_awaitable, write_ec));
        if (write_ec) {
            SPDLOG_ERROR("尝试发送 UsbIpRetSubmit 包时出错：{}", write_ec.message());
        }
        else {
            SPDLOG_TRACE("成功发送 UsbIpRetSubmit 包");
        }
        end_processing_urb();
    }, if_has_value_than_rethrow);
}

usbipdcpp::Session::~Session() {
    SPDLOG_TRACE("Session析构");
}

asio::awaitable<void> usbipdcpp::Session::transfer_loop(usbipdcpp::error_code &transferring_ec) {
    while (!should_immediately_stop) {
        usbipdcpp::error_code ec;
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


                        usbipdcpp::error_code ec_during_handling_urb;
                        start_processing_urb();
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
                            transferring_ec = ec_during_handling_urb;
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
                    transferring_ec = make_error_code(ErrorType::UNKNOWN_CMD);
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
    current_import_device->cancer_all_transfer();
    server.try_moving_device_to_available(*current_import_device_id);
    current_import_device_id.reset();
    current_import_device.reset();
    SPDLOG_TRACE("将当前导入设备的busid设为空");

    if (!should_immediately_stop) {
        //没发生严重错误或者外部要求的stop才进行等待
        //socket断连表明客户端执行了usbip detach，正常断开时会进行等待
        co_await wait_for_all_urb_processed();
    }
    cmd_transferring = false;
}

void usbipdcpp::Session::start_processing_urb() {
    SPDLOG_TRACE("开始处理urb时，urb_processing_counter:{}", urb_processing_counter);
    urb_processing_counter++;
}

void usbipdcpp::Session::end_processing_urb() {
    if (--urb_processing_counter == 0) {
        no_urb_processing_notify_channel.try_send(asio::error_code{});
    }
    SPDLOG_TRACE("结束处理urb后,urb_processing_counter:{}", urb_processing_counter);
}

asio::awaitable<void> usbipdcpp::Session::wait_for_all_urb_processed() {
    if (urb_processing_counter != 0) {
        SPDLOG_DEBUG("当前还有{}个URB正在传输，等待其传输完", urb_processing_counter);
        co_await no_urb_processing_notify_channel.async_receive(asio::use_awaitable);
        SPDLOG_TRACE("收到空消息，结束等待");
    }
    else {
        SPDLOG_TRACE("无正在传输的urb，无需等待");
    }
}
