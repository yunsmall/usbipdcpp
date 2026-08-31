// 这个文件的调试信息太多了，开成TRACE打印会疯掉的
// 这里强制改成INFO
#ifdef SPDLOG_ACTIVE_LEVEL
    #undef SPDLOG_ACTIVE_LEVEL
#endif
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_INFO

#include "usbipdcpp/Session.h"

#include <asio.hpp>
#include <optional>
#include <spdlog/spdlog.h>
#include <variant>

#include "usbipdcpp/DeviceHandler/DeviceHandler.h"

#include "usbipdcpp/utils/utils.h"
#include "usbipdcpp/Device.h"
#include "usbipdcpp/Server.h"
#include "usbipdcpp/network.h"
#include "usbipdcpp/protocol.h"

usbipdcpp::Session::Session(Server &server, std::uint64_t id) :
    server(server), id(id) {
}

void usbipdcpp::Session::enqueue_ret_submit(UsbIpResponse::UsbIpRetSubmit &&submit) {
    std::lock_guard lock(swap_mutex);
    write_buffer.emplace_back(std::move(submit));
}

void usbipdcpp::Session::enqueue_ret_unlink(UsbIpResponse::UsbIpRetUnlink &&unlink) {
    std::lock_guard lock(swap_mutex);
    write_buffer.emplace_back(std::move(unlink));
}

void usbipdcpp::Session::wakeup_sender() {
    has_data.store(true, std::memory_order_release);
    data_available_cv.notify_all();
}

void usbipdcpp::Session::submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink &&unlink) {
    enqueue_ret_unlink(std::move(unlink));
    wakeup_sender();
}

void usbipdcpp::Session::submit_ret_submit(UsbIpResponse::UsbIpRetSubmit &&submit) {
    enqueue_ret_submit(std::move(submit));
    wakeup_sender();
}

usbipdcpp::Session::~Session() {
    // 主线程句柄已在 run() 就地 detach 不存成员，子线程（sender）已在
    // transfer_loop 内 join 完毕，此处无需线程句柄收尾
    // 注意：不能在析构中打日志。若进程正在退出（如测试 main 返回），spdlog 的
    // 全局 registry 可能已随静态析构销毁，此时打日志会访问已析构对象导致段错误
    // 通知 Server 析构完成，必须放在析构体末尾：
    // 回调在析构体开头执行的话，stop() 按计数判断可能提前返回并析构 Server，
    // 此处再访问 Server（notify_session_destroyed 触碰 reap_cv）即 use-after-free
    server.notify_session_destroyed();
}

void usbipdcpp::Session::run() {
    // 先获取自身指针，防止被智能指针析构
    auto self = shared_from_this();
    if (server.before_thread_create_callback) {
        server.before_thread_create_callback(ThreadPurpose::SessionMain);
    }
    // 主线程句柄就地 detach 不存成员：线程
    // 收尾时不再触碰自身句柄，避免与 run() 的赋值并发访问 std::thread 对象
    // （std::thread 对象非线程安全）。线程体内持有 self，return 时最后一个
    // 引用释放即自析构
    std::thread main_thread;
    try {
        main_thread = std::thread([self = std::move(self)]() {
            try {
                self->parse_op();
            } catch (const std::exception &e) {
                // 兜底：任何异常都不能逃出线程函数（否则 std::terminate 崩溃整个进程）
                SPDLOG_ERROR("session线程未捕获异常：{}", e.what());
            } catch (...) {
                SPDLOG_ERROR("session线程未捕获未知异常");
            }

            // 处理结束后自动往服务器中删除自身并触发退出回调
            self->server.remove_session(self->id);
        });
    } catch (...) {
        // 线程创建失败（如系统资源不足）：after 回调仍要调用（传 nullptr
        // 表示创建失败），保证 before/after 成对；异常继续传播给
        // accept_loop 的兜底 catch 移除会话
        if (server.after_thread_create_callback) {
            server.after_thread_create_callback(ThreadPurpose::SessionMain, nullptr);
        }
        throw;
    }
    main_thread.detach();
    if (server.after_thread_create_callback) {
        // 用户回调抛异常不在此捕获：用户自己的代码抛了，用户想做的事
        // 可能已不正常，异常按原有路径传播（accept_loop 的兜底 catch
        // 会移除会话记录）——库不为用户回调擦屁股
        server.after_thread_create_callback(ThreadPurpose::SessionMain, &main_thread);
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
    std::visit(
            [&, this](auto &&cmd) {
                using T = std::remove_cvref_t<decltype(cmd)>;
                if constexpr (std::is_same_v<UsbIpCommand::OpReqDevlist, T>) {
                    SPDLOG_TRACE("收到 OpReqDevlist 包");
                    data_type to_be_sent;
                    {
                        std::shared_lock lock(server.devices_mutex);
                        to_be_sent =
                                UsbIpResponse::OpRepDevlist::create_from_devices(server.available_devices).to_bytes();
                    }
                    asio::write(socket, asio::buffer(to_be_sent), ec);
                    if (!ec) [[likely]]
                        SPDLOG_TRACE("成功发送 OpRepDevlist 包");
                    else
                        SPDLOG_TRACE("发送 OpRepDevlist 包出错{}", ec.message());
                }
                else if constexpr (std::is_same_v<UsbIpCommand::OpReqImport, T>) {
                    SPDLOG_TRACE("收到 OpReqImport 包");
                    // busid 是客户端可控的 32 字节，不保证含 \0（from_socket
                    // 只读满 32 字节）。先强制截断最后一个字节再构造 string，
                    // 否则 std::string(ptr) 会越界读栈内存（安全漏洞）
                    cmd.busid[31] = '\0';
                    auto wanted_busid = std::string(reinterpret_cast<char *>(cmd.busid.data()));
                    UsbIpResponse::OpRepImport op_rep_import{};
                    SPDLOG_TRACE("客户端想连接busid为 {} 的设备", wanted_busid);

                    bool target_device_is_using = false;
                    bool open_device_failed = false;
                    // 已经在使用的不支持导出
                    if (server.is_device_using(wanted_busid)) {
                        spdlog::warn("正在使用的设备不支持导出");
                        // 查看内核源码中 tools/usbip/src/usbipd.c 函数 recv_request_import
                        // 源码可以发现应该返回NA而不是DevBusy
                        op_rep_import = UsbIpResponse::OpRepImport::create_on_failure_with_status(
                                static_cast<std::uint32_t>(OperationStatuType::NA));
                        target_device_is_using = true;
                    }
                    else {
                        if (auto using_device = server.try_moving_device_to_using(wanted_busid)) {
                            std::lock_guard lock(current_import_device_data_mutex);
                            spdlog::info("成功将设备放入正在使用的设备中");
                            current_import_device_id = wanted_busid;
                            // 将当前使用的设备指向这个设备
                            current_import_device = using_device;
                            current_handler = using_device->handler;
                            spdlog::info("成功缓存正在使用的设备");

                            // 在发送 OpRepImport 之前尝试打开设备
                            usbipdcpp::error_code open_ec;
                            current_handler->on_new_connection(*this->responder(), open_ec);
                            if (open_ec) {
                                SPDLOG_ERROR("打开设备失败: {}", open_ec.message());
                                open_device_failed = true;
                                // 将设备移回可用列表
                                server.try_moving_device_to_available(wanted_busid);
                                current_import_device.reset();
                                current_handler.reset();
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
                    if (!ec) [[likely]]
                        SPDLOG_TRACE("成功发送 OpRepImport 包", size);
                    else
                        SPDLOG_TRACE("发送 OpRepImport 包出错{}", ec.message());

                    if (!ec) [[likely]] {
                        if (cmd_transferring) {
                            usbipdcpp::error_code transferring_ec;
                            // 进入通信状态
                            transfer_loop(transferring_ec);
                            if (transferring_ec) {
                                // 连接断开类错误（见 459 处判断）是正常路径，降级为 debug
                                auto is_disconnect = transferring_ec == make_error_code(ErrorType::SOCKET_EOF) ||
                                                     transferring_ec == make_error_code(ErrorType::SOCKET_ERR);
                                if (is_disconnect) {
                                    SPDLOG_DEBUG("传输结束（客户端断开）：{}", transferring_ec.message());
                                }
                                else {
                                    SPDLOG_ERROR("Error occurred during transferring : {}", transferring_ec.message());
                                }
                                ec = transferring_ec;
                            }

                            // on_disconnection 和设备清理已在 receiver 中处理
                        }
                    }
                    else if (cmd_transferring) {
                        // OP_REP_IMPORT 发送失败：连接已不可用，立即收尾，
                        // 不等 receiver 读到错误再清理（窗口内设备滞留
                        // using 列表，拔出/stop 竞争时状态不一致）。清理逻辑
                        // 与 transfer_loop 中 sender 线程创建失败的路径一致：
                        // 通知 handler 断连释放设备接口，按 is_device_removed()
                        // 决定从 using 移除或移回可用列表。不能进入
                        // transfer_loop——receiver 会再次清理（current_handler
                        // 已 reset），二次清理是空指针访问
                        SPDLOG_ERROR("发送 OpRepImport 失败，断开本次连接: {}", ec.message());
                        usbipdcpp::error_code disconnect_ec;
                        current_handler->on_disconnection(disconnect_ec);
                        if (current_handler->is_device_removed()) {
                            std::lock_guard lock(server.get_devices_mutex());
                            server.get_using_devices().erase(*current_import_device_id);
                        }
                        else {
                            server.try_moving_device_to_available(*current_import_device_id);
                        }
                        current_import_device_id.reset();
                        current_import_device.reset();
                    }
                }
                else {
                    // 确保处理了所有可能类型
                    static_assert(!std::is_same_v<T, T>);
                }
            },
            op);

close_socket:
    std::error_code ignore_ec;
    SPDLOG_INFO("尝试关闭socket");
    // 收尾统一关闭 socket（shutdown + close），与 immediately_stop 的
    // shutdown/cancel 分属不同线程，close 与 cancel 并发操作同一 socket 是
    // asio 未定义行为（reactive 后端中 close 的 cleanup_descriptor_data 与
    // cancel 的 cancel_ops 操作同一 op_queue，偶发数据竞态导致挂起，本地循环
    // 第 1303 次复现），故用 socket_mutex 互斥。
    // socket 提前 close 后，~Session 的 socket 析构不再访问 io_context
    // （已关闭的 socket 析构快速返回），Session 线程自析构即使晚于 Server
    // 析构也不会悬垂访问
    {
        std::lock_guard lock(socket_mutex);
        socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignore_ec);
        socket.close(ignore_ec);
    }
}

void usbipdcpp::Session::immediately_stop() {
    {
        // 置位须持 swap_mutex（理由同 receiver 收尾处注释）：sender 的
        // 455 wait 谓词在锁内检查 should_immediately_stop，无锁置位会
        // 造成丢失唤醒（lost wakeup）
        std::lock_guard lock(swap_mutex);
        if (should_immediately_stop.exchange(true)) {
            // 幂等：只处理一次。置位即保证 socket 已由本函数处理（shutdown+cancel
            // 打断阻塞读），无需重复操作。注意置位路径不只有本函数——receiver /
            // sender 退出时也会置位（它们只是退出标志，不操作 socket）；真正操作
            // socket 的只有本函数与收尾的 close_socket（socket_mutex 互斥），
            // 不存在"置位但无人操作 socket"的路径
            return;
        }
    }

    std::error_code ignore_ec;
    // shutdown 和 cancel 缺一不可，两个平台打断挂起同步读的机制互补
    // （本地纯 asio 实测）：
    // - Linux/macOS：shutdown 唤醒挂起的 poll，读立即以 EOF 返回；cancel 无效
    // - Windows：shutdown 不唤醒挂起的 WSAPoll（实测 15 秒以上仍卡住，实际要
    //   等 TcpTimedWaitDelay 240 秒连接超时才返回）；cancel 内部调用
    //   WSACancelBlockingCall 立即打断同步读
    // 两者在对方平台都无效但无害。顺序必须先 shutdown 再 cancel：若先 cancel，
    // 而读线程恰好处于"检查缓冲与注册 WSARecv/WSASend 之间"的窗口，cancel
    // 没有挂起操作可取消会被丢弃，随后注册的 I/O 挂起且 shutdown 不打断已
    // 挂起的操作，会话将永久阻塞；先 shutdown 则任何"之后注册"的 I/O 都会
    // 立即失败，cancel 负责取消"已挂起"的。
    // 不用 close：asio 文档明确 close 不线程安全（socket 共享对象只有
    // send/receive/connect/shutdown 相互之间线程安全），跨线程 close 与读并发
    // 曾导致 CI 上 Linux/macOS 卡死。close 只由会话收尾执行（parse_op 的
    // close_socket），与这里的 shutdown/cancel 用 socket_mutex 互斥（close
    // 与 cancel 并发是未定义行为）
    {
        std::lock_guard lock(socket_mutex);
        socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignore_ec);
        socket.cancel(ignore_ec);
    }
    // 唤醒 sender 线程，否则它卡在 data_available_cv.wait() 上直到 receiver 退出。
    data_available_cv.notify_all();
    SPDLOG_INFO("成功调用shutdown");
}

void usbipdcpp::Session::transfer_loop(usbipdcpp::error_code &transferring_ec) {
    // on_new_connection 已在 parse_op 中调用，此处不再调用

    error_code receiver_ec;
    error_code sender_ec;
    if (server.before_thread_create_callback) {
        server.before_thread_create_callback(ThreadPurpose::SessionSender);
    }
    // 先创建 sender 线程再执行接收循环：sender_thread 句柄必然先于收尾
    // （join sender）就绪，不存在句柄未赋值的中间状态——若两线程句柄先后
    // 赋值，主线程可能在子线程句柄赋值前就收尾，随后才创建的子线程会把
    // 残留响应写到已关闭的 socket。
    // 创建失败（如系统资源不足）时连接已建立（on_new_connection 已调用，
    // 设备已打开/声明接口）但传输循环无法启动，走断连收尾（见 catch）：
    // 通知 handler 断连释放设备接口，把设备移回可用列表，否则设备滞留
    // using 列表，下次导入会误报"正在使用"；然后重新抛出，由 run() 的
    // 兜底 catch 移除 session，避免异常逃逸导致 std::terminate
    std::thread sender_thread;
    try {
        sender_thread = std::thread([&, this]() {
            try {
                sender(sender_ec);
            } catch (const std::exception &e) {
                // sender 内 to_socket 的 send_transfer_data 可能抛
                // （SmallVector 扩容 bad_alloc 等），异常不能逃出线程函数
                // （否则 std::terminate 崩溃整个进程）。置错误码供
                // transfer_loop 报告
                SPDLOG_ERROR("sender 线程异常：{}", e.what());
                sender_ec = make_error_code(ErrorType::INTERNAL_ERROR);
            } catch (...) {
                SPDLOG_ERROR("sender 线程未知异常");
                sender_ec = make_error_code(ErrorType::INTERNAL_ERROR);
            }
            // 线程退出标记：唤醒 transfer_loop 的限时等待（sender 函数
            // 正常路径末尾已置位，此处为异常路径兜底，重复置位无害）
            sender_done.store(true);
            data_available_cv.notify_all();
        });
    } catch (...) {
        SPDLOG_ERROR("sender 线程创建失败，断开本次连接");
        usbipdcpp::error_code disconnect_ec;
        current_handler->on_disconnection(disconnect_ec);
        if (current_handler->is_device_removed()) {
            std::lock_guard lock(server.get_devices_mutex());
            server.get_using_devices().erase(*current_import_device_id);
        }
        else {
            server.try_moving_device_to_available(*current_import_device_id);
        }
        current_import_device_id.reset();
        current_import_device.reset();
        // after 回调仍要调用（传 nullptr 表示创建失败），保证 before/after 成对
        if (server.after_thread_create_callback) {
            server.after_thread_create_callback(ThreadPurpose::SessionSender, nullptr);
        }
        throw;
    }
    if (server.after_thread_create_callback) {
        server.after_thread_create_callback(ThreadPurpose::SessionSender, &sender_thread);
    }

    bool receiver_ok = true;
    try {
        receiver(receiver_ec);
    } catch (const std::exception &e) {
        SPDLOG_ERROR("receiver 异常：{}", e.what());
        receiver_ec = make_error_code(ErrorType::INTERNAL_ERROR);
        receiver_ok = false;
    } catch (...) {
        SPDLOG_ERROR("receiver 未知异常");
        receiver_ec = make_error_code(ErrorType::INTERNAL_ERROR);
        receiver_ok = false;
    }
    if (!receiver_ok) {
        // receiver 异常路径跳过了其收尾（on_disconnection + 设备回池），
        // 这里补齐，否则设备滞留 using 列表（清理逻辑与 receiver 末尾
        // 一致）。不能让它逃出 transfer_loop：sender_thread 尚未 join，
        // std::thread 析构会 std::terminate 崩溃整个进程
        usbipdcpp::error_code disconnect_ec;
        try {
            current_handler->on_disconnection(disconnect_ec);
        } catch (...) {
            SPDLOG_ERROR("on_disconnection 异常");
        }
        // 补齐置位与唤醒：receiver 异常退出时正常收尾的置位+notify（572-573）
        // 未执行，sender 还卡在 data_available_cv.wait 上，不置位不 notify
        // 会让 transfer_loop 的限时等待超时后 join 永久卡死
        {
            std::lock_guard lock(swap_mutex);
            should_immediately_stop = true;
        }
        data_available_cv.notify_all();
        if (current_handler->is_device_removed()) {
            std::lock_guard lock(server.get_devices_mutex());
            server.get_using_devices().erase(*current_import_device_id);
        }
        else {
            server.try_moving_device_to_available(*current_import_device_id);
        }
        current_import_device_id.reset();
        current_import_device.reset();
    }
    SPDLOG_INFO("receiver退出");

    // receiver 结束后 sender 必然在退出路上（should_immediately_stop 已置），
    // 唯一可能卡住的是挂起的写（对端不读、TCP 窗口满）。限时等待 sender
    // 自然退出，超时后锁内 close 强制打断：
    // close 与 sender 挂起的 write 并发是设计用途而非 UB，依据如下：
    // 1) asio 文档（cancel 的说明）：可移植取消应"Use the close() function
    //    to simultaneously cancel the outstanding operations and close the
    //    socket"——close 专门用于打断挂起的异步操作（完成 operation_aborted）；
    // 2) Windows：close 即 closesocket，句柄关闭后内核保证挂起的 WSASend
    //    IRP 完成并投递到 IOCP（win_iocp_socket_service_base.ipp），阻塞
    //    中的 write_some 随即返回；
    // 3) Linux：close 经 deregister_descriptor（epoll_reactor.ipp）在
    //    descriptor mutex 下把挂起操作置 operation_aborted 并通过
    //    post_deferred_completions 同步投递，阻塞中的写被唤醒返回。
    // 文档"Shared objects: Unsafe"是语言层面的保守表述，close 取消挂起
    // 操作是文档明确推荐的例外场景。
    {
        std::unique_lock lock(swap_mutex);
        data_available_cv.wait_for(lock, std::chrono::milliseconds(100),
                                   [this] { return sender_done.load(); });
    }
    if (!sender_done.load()) {
        std::error_code ignore_ec;
        {
            std::lock_guard lock(socket_mutex);
            socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignore_ec);
            socket.close(ignore_ec);
        }
    }
    sender_thread.join();
    SPDLOG_INFO("sender thread退出");

    // 在 handler 存活时清空队列，确保 TransferHandle 析构时 handler 仍有效
    write_buffer.clear();
    read_buffer.clear();

    if (sender_ec) {
        SPDLOG_ERROR("An error occur during sending: {}", sender_ec.message());
        transferring_ec = sender_ec;
    }
    // 一般来说receiver_ec的ec重要一点，因此会覆盖掉
    else if (receiver_ec) {
        // 连接断开类错误（客户端 detach/关闭、网络中断）是服务器的日常路径，
        // 不算异常：空闲时断开读到 eof（SOCKET_EOF），传输中断开读到
        // connection_reset 等（归为 SOCKET_ERR），两者等价，均降级为 debug
        auto is_disconnect = receiver_ec == make_error_code(ErrorType::SOCKET_EOF) ||
                             receiver_ec == make_error_code(ErrorType::SOCKET_ERR);
        if (is_disconnect) {
            SPDLOG_DEBUG("连接断开：{}", receiver_ec.message());
        }
        else {
            SPDLOG_ERROR("An error occur during receiving: {}", receiver_ec.message());
        }
        transferring_ec = receiver_ec;
    }
    cmd_transferring = false;
}

std::optional<usbipdcpp::UsbIpResponse::RetVariant> usbipdcpp::Session::sender_get_data(usbipdcpp::error_code &ec) {
    // 如果 read_buffer 为空，尝试交换
    if (read_buffer.empty()) [[likely]] {
        std::unique_lock lock(swap_mutex);
        data_available_cv.wait(
                lock, [this]() { return has_data.load(std::memory_order_acquire) || should_immediately_stop; });
        if (!write_buffer.empty()) {
            read_buffer.swap(write_buffer);
            has_data.store(false, std::memory_order_release);
        }
    }
    if (!read_buffer.empty()) [[likely]] {
        usbipdcpp::UsbIpResponse::RetVariant ret_v = std::move(read_buffer.front());
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

        auto command = UsbIpCommand::get_cmd_from_socket(socket, current_handler.get(), ec);
        if (ec) [[unlikely]] {
            if (ec.value() == static_cast<int>(ErrorType::SOCKET_EOF)) {
                SPDLOG_DEBUG("连接关闭");
            }
            else if (ec.value() == static_cast<int>(ErrorType::SOCKET_ERR)) {
                SPDLOG_DEBUG("发生socket错误");
            }
            else {
                SPDLOG_ERROR("从socket中获取命令时出错：{}", ec.message());
            }
            // 把错误传出给 transfer_loop（否则上层只能看到默认 0，
            // 无法区分"正常断开"与"socket 错误断开"）
            receiver_ec = ec;
            break;
        }
        if (should_immediately_stop) [[unlikely]]
            break;
        std::visit(
                [&, this](auto &&cmd) {
                    using T = std::remove_cvref_t<decltype(cmd)>;
                    if constexpr (std::is_same_v<UsbIpCommand::UsbIpCmdSubmit, T>) {
                        UsbIpCommand::UsbIpCmdSubmit &cmd2 = cmd;
                        LATENCY_TRACK_START(latency_tracker, cmd2.header.seqnum);
                        SPDLOG_TRACE("收到 UsbIpCmdSubmit 包，序列号: {}", cmd2.header.seqnum);
                        auto out = cmd2.header.direction == UsbIpDirection::Out;
                        SPDLOG_TRACE("Usbip传输方向为：{}", out ? "out" : "in");
                        std::uint8_t real_ep = out ? static_cast<std::uint8_t>(cmd2.header.ep)
                                                   : (static_cast<std::uint8_t>(cmd2.header.ep) | 0x80);
                        SPDLOG_TRACE("传输的真实端口为 {:02x}", real_ep);
                        [[maybe_unused]] auto current_seqnum = cmd2.header.seqnum;

                        auto ep_find_ret = current_import_device->find_ep(real_ep);
                        if (ep_find_ret.has_value()) [[likely]] {
                            auto &ep = ep_find_ret->first;
                            auto &intf = ep_find_ret->second;

                            SPDLOG_TRACE("->端口{0:02x}", ep.address);
                            SPDLOG_TRACE("->setup数据{}", get_every_byte(cmd2.setup.to_bytes()));


                            usbipdcpp::error_code ec_during_handling_urb;
                            // start_processing_urb();
                            LATENCY_TRACK(latency_tracker, cmd2.header.seqnum, "准备传入设备receive_urb");
                            current_handler->receive_urb(std::move(cmd2), ep, std::move(intf), ec_during_handling_urb);

                            if (ec_during_handling_urb) [[unlikely]] {
                                SPDLOG_ERROR("Error during handling urb : {}", ec_during_handling_urb.message());
                                // 发生错误代表已经不能继续通信了
                                receiver_ec = ec_during_handling_urb;
                                should_immediately_stop = true;
                                return;
                            }
                        }
                        else {
                            // 找不到 real_ep 对应的端点。静默丢弃、不回 EPIPE，
                            // 与内核 usbip 的 stub_rx.c get_pipe()（if (pipe==-1)
                            // return，无响应）及 usbipd-libusb 的
                            // stub_get_transfer_type()（type>MASK 时 return）一致：
                            // 正常客户端只发设备配置描述符里真实存在的端点，
                            // 找不到说明是异常/非法输入的兜底，丢弃即可
                            SPDLOG_WARN("找不到端点{}，静默丢弃该 CMD_SUBMIT", real_ep);
                        }
                    }
                    else if constexpr (std::is_same_v<UsbIpCommand::UsbIpCmdUnlink, T>) {
                        UsbIpCommand::UsbIpCmdUnlink &cmd2 = cmd;
                        SPDLOG_TRACE("收到 UsbIpCmdUnlink 包，序列号: {}", cmd2.header.seqnum);

                        current_handler->handle_unlink_seqnum(cmd2.unlink_seqnum, cmd2.header.seqnum);
                    }
                    else if constexpr (std::is_same_v<std::monostate, T>) {
                        SPDLOG_ERROR("收到未知包");
                        receiver_ec = make_error_code(ErrorType::UNKNOWN_CMD);
                    }
                    else {
                        // 确保处理了所有可能类型
                        static_assert(!std::is_same_v<T, T>);
                    }
                    return;
                },
                command);
    }
    // 顺序有讲究：必须先通知设备断连，再停 sender，不能反序。
    // handler 在 on_disconnection 被调用之前一直以为连接还在正常通信
    // （虚拟设备会持续把数据写入队列，如虚拟串口收到的字节），
    // on_disconnection 是"连接已断开"的通知：handler 收到后才停止写入、
    // 完成自己的收尾（可能把最后一批数据入队），随后 sender 才停止、
    // 尽力把这批最后的数据发送出去（即使连接已失效，发送失败也走
    // sender 的错误退出路径，无害）。若反序先停 sender，handler 尚未
    // 收到断连通知、收尾入队的数据会被静默丢弃，虚拟设备表现为
    // "毫无预兆地断连"。libusb 后端的 on_disconnection 会阻塞等待全部
    // 传输回调完成（cancel 后回调必触发），期间 sender 仍可能消费队列
    // 并向已失效的 socket 发送（失败即退出），这是设计允许的
    current_handler->on_disconnection(receiver_ec);
    // 然后再关闭发送线程，防止先关闭了但设备因还未被通知到关闭而报错。
    // 置位必须在 swap_mutex 锁内：sender 的 455 wait 谓词在锁内检查
    // should_immediately_stop，无锁置位会让 notify 落在 sender 检查谓词
    // 与挂入 cond_wait 队列之间的窗口里，唤醒丢失（lost wakeup），sender
    // 无限 wait 永久挂起
    {
        std::lock_guard lock(swap_mutex);
        should_immediately_stop = true;
    }
    data_available_cv.notify_all();

    /* 这里先标记为可用是可行的
     * 一是设备on_disconnection需要阻塞，把自身断连需要做的事全处理掉
     * 二是这个session马上就要析构了current_import_device的那两个变量不会重新被使用
     * 因此先标记为可用再清除这两个变量的状态
     */
    if (current_handler->is_device_removed()) {
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
    // RET_SUBMIT 和 RET_UNLINK 共用一个 write_buffer 队列，入队顺序即是发送顺序，
    // FIFO 发送即可。不像内核/usbipd-libusb 中分成 priv_tx 和 unlink_tx 两个独立
    // 队列无法分辨先后，必须手动先发 SUBMIT 再发 UNLINK。
    //
    // 以 libusb 后端为例：transfer_callback 在 transfers_mutex_ 锁内同时完成
    // 「从 map 移除 → 入队 RET_SUBMIT」，handle_unlink_seqnum 想介入必须等锁释放。
    // 等它拿到锁时 map 里已无此传输，此时入队的 RET_UNLINK 天然排在 RET_SUBMIT
    // 之后，顺序正确。
    //
    // 退出时丢弃队列中残留的响应是正确行为：循环只在 receiver 退出（socket
    // 错误/EOF）或 stop() 后退出，这两种情况下连接已不可用——TCP 客户端
    // （Linux vhci/usbip）没有"半关闭后仍等响应"的协议流程，要么正常通信
    // 要么整体关闭连接，客户端不会因丢失响应而挂起；与内核 stub 连接错误
    // 时停止发送、丢弃未发数据的行为一致
    while (!should_immediately_stop) {
        auto send_data_opt = sender_get_data(ec);
        if (ec || should_immediately_stop) [[unlikely]] {
            break;
        }
        if (!send_data_opt.has_value()) [[unlikely]] {
            // 虚假唤醒（入队与唤醒分离后，has_data 为 true 但数据已被前一轮消费）。
            // 仅当 should_immediately_stop 时才退出，否则回去继续等。
            if (should_immediately_stop)
                break;
            continue;
        }
        auto send_data = std::move(send_data_opt.value());

        SPDLOG_TRACE("channel收到消息");
        error_code sending_ec;
        std::visit(
                [&](auto &&cmd) {
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
                },
                send_data);

        if (sending_ec) {
            // TCP 写入失败，立即关闭 session 双向通信。
            // 仅 break 退出 sender 会让 receiver 继续运行直到 keepalive 超时。
            // shutdown + cancel 跨平台打断 receiver 的挂起同步读（见
            // immediately_stop 注释）；与收尾的 close 互斥（socket_mutex）
            should_immediately_stop = true;
            std::error_code ignore_ec;
            {
                std::lock_guard lock(socket_mutex);
                socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignore_ec);
                socket.cancel(ignore_ec);
            }
            // 不把 sending_ec 传给 transfer_loop：发送失败时已 shutdown+cancel
            // 打断 receiver 的挂起读，receiver 必然随后以 socket 错误退出并设置
            // receiver_ec，transfer_loop 的 ec 优先走 receiver_ec（sender_ec 为
            // 默认 0 时走 else if 分支），外部仍能区分错误来源，sending_ec 没有
            // 额外信息量
            // ec = sending_ec;
            break;
        }
    }
    // 线程退出标记：唤醒 transfer_loop 的限时等待（见 transfer_loop 注释）
    sender_done.store(true);
    data_available_cv.notify_all();
}
