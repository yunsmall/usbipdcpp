#pragma once

#include <atomic>
#include <unordered_map>
#include <shared_mutex>
#include <tuple>

#include <chrono>
#include <thread>
#include <condition_variable>

#include <asio/ip/tcp.hpp>
#ifdef USBIPDCPP_USE_COROUTINE
#include <asio/awaitable.hpp>
#include <asio/experimental/channel.hpp>
#else
#include <deque>
#endif

#include "protocol.h"
#include "type.h"

namespace usbipdcpp {
    class Server;

    /**
     * @brief 自行处理生命周期，一个连接创建一个Session，创建完服务器就对Session脱离管控了。
     * 请确保Session存活的时候Server未被析构，不然是未定义行为
     */
    class Session : public std::enable_shared_from_this<Session> {
        friend class Server;

    public:
        explicit Session(Server &server);
        Session(const Session &) = delete;
        Session(Session &&) = delete;

        /**
         * @brief 线程安全，用来查询某一序列是否被unlink了。
         * @param seqnum
         * @return true表示被unlink了，第二个值为发过来的unlink包的seqnum而不是将要取消的包的seqnum
         */
        std::tuple<bool, std::uint32_t> get_unlink_seqnum(std::uint32_t seqnum);

        /**
         * @brief 线程安全，删除某序列的标记
         * @param seqnum 被unlink的包的seqnum
         */
        void remove_seqnum_unlink(std::uint32_t seqnum);

        /**
         * @brief 推荐使用这个函数。该函数异步，不阻塞。内部直接向asio context提交任务，因此不用加锁。内部线程安全。
         * 请确保每个urb都需要提交返回的包
         * @param unlink
         * @param seqnum 被unlink的包的seqnum
         * @note 根据USBIPDCPP_USE_COROUTINE宏选择不同实现：
         *       - 协程版本：通过transfer_channel发送
         *       - 非协程版本：通过send_data队列发送
         */
        void submit_ret_unlink_and_then_remove_seqnum_unlink(UsbIpResponse::UsbIpRetUnlink &&unlink,
                                                             std::uint32_t seqnum);

        /**
         * @brief 该函数异步，不阻塞。内部直接向asio context提交任务，因此不用加锁。内部线程安全。调用完别忘记调用remove_seqnum_unlink。
         * 请确保每个urb都需要提交返回的包
         * @param unlink
         * @note 根据USBIPDCPP_USE_COROUTINE宏选择不同实现：
         *       - 协程版本：通过transfer_channel发送
         *       - 非协程版本：通过send_data队列发送
         */
        void submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink &&unlink);

        /**
         * @brief 该函数异步，不阻塞。内部直接向asio context提交任务，因此不用加锁。内部线程安全。
         * 请确保每个urb都需要提交返回的包
         * @param submit
         * @note 根据USBIPDCPP_USE_COROUTINE宏选择不同实现：
         *       - 协程版本：通过transfer_channel发送
         *       - 非协程版本：通过send_data队列发送
         */
        void submit_ret_submit(UsbIpResponse::UsbIpRetSubmit &&submit);


        ~Session();

    private:
#ifdef USBIPDCPP_USE_COROUTINE
        void submit_ret_submit_co(UsbIpResponse::UsbIpRetSubmit &&submit);
        void submit_ret_unlink_co(UsbIpResponse::UsbIpRetUnlink &&unlink);
#else
        void submit_ret_submit_impl(UsbIpResponse::UsbIpRetSubmit &&submit);
        void submit_ret_unlink_impl(UsbIpResponse::UsbIpRetUnlink &&unlink);
#endif

    private:
        /**
         * @brief 新建Session时由Server调用（协程版本）
         */
        void run_co();

        /**
         * @brief 新建Session时由Server调用（非协程版本）
         */
        void run();

#ifdef USBIPDCPP_USE_COROUTINE
        asio::awaitable<void> parse_op_co();
#else
        void parse_op();
#endif

        /**
         * @brief 置停止标志位，并且关闭socket。只能由Server调用。
         * 内部不会关闭线程，只会通知线程关闭
         */
        void immediately_stop();

#ifdef USBIPDCPP_USE_COROUTINE
        using transfer_channel_type = asio::experimental::channel<void(asio::error_code, UsbIpResponse::RetVariant)>;
        static constexpr std::size_t transfer_channel_size = 100;
        std::unique_ptr<transfer_channel_type> transfer_channel = nullptr;
#else
        //用于非协程发送数据
        std::deque<UsbIpResponse::RetVariant> send_data;
        std::mutex send_data_mutex;
        std::condition_variable send_data_cv;
#endif
        /**
         * @brief 不停地传输urb
         * @param transferring_ec 传输urb途中的ec
         */
#ifdef USBIPDCPP_USE_COROUTINE
        asio::awaitable<void> transfer_loop_co(usbipdcpp::error_code &transferring_ec);
        asio::awaitable<void> receiver_co(usbipdcpp::error_code &receiver_ec);
        asio::awaitable<void> sender_co(usbipdcpp::error_code &ec);
#else
        void transfer_loop(usbipdcpp::error_code &transferring_ec);
        void receiver(usbipdcpp::error_code &receiver_ec);
        void sender(usbipdcpp::error_code &ec);
        std::optional<UsbIpResponse::RetVariant> sender_get_data(usbipdcpp::error_code &ec);
#endif

        std::atomic_bool should_immediately_stop = false;

        //是否在传输ret_submit的阶段
        std::atomic_bool cmd_transferring = false;

        //传输过程中不允许为空，传输过程中禁止任何写入。不允许在非网络线程读，除非加锁
        std::optional<std::string> current_import_device_id = std::nullopt;
        //传输过程中不允许为空，传输过程中禁止任何写入。不允许在非网络线程读，除非加锁
        std::shared_ptr<UsbDevice> current_import_device = nullptr;
        //上面两个变量的值的锁
        std::shared_mutex current_import_device_data_mutex;

        //从原本的ret_submit的seqnum映射到ret_unlink的seqnum
        std::unordered_map<std::uint32_t, std::uint32_t> unlink_map;
        std::shared_mutex unlink_map_mutex;

        Server &server;
        asio::io_context session_io_context{};
        asio::ip::tcp::socket socket;


        //这个线程结束后自动析构this
        std::thread run_thread;
    };
}
