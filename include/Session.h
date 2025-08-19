#pragma once

#include <atomic>
#include <map>
#include <shared_mutex>
#include <tuple>

#include <asio/ip/tcp.hpp>
#include <asio/awaitable.hpp>
#include <asio/experimental/channel.hpp>

#include "protocol.h"
#include "type.h"

namespace usbipdcpp {
    class Server;

    class Session {
        friend class Server;

    public:
        Session(Server &server, asio::ip::tcp::socket &&socket);


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
         */
        void submit_ret_unlink_and_then_remove_seqnum_unlink(UsbIpResponse::UsbIpRetUnlink &&unlink,
                                                             std::uint32_t seqnum);

        /**
         * @brief 该函数异步，不阻塞。内部直接向asio context提交任务，因此不用加锁。内部线程安全。调用完别忘记调用remove_seqnum_unlink。
         * 请确保每个urb都需要提交返回的包
         * @param unlink
         */
        void submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink &&unlink);
        /**
         * @brief 该函数异步，不阻塞。内部直接向asio context提交任务，因此不用加锁。内部线程安全。
         * 请确保每个urb都需要提交返回的包
         * @param submit
         */
        void submit_ret_submit(UsbIpResponse::UsbIpRetSubmit &&submit);


        ~Session();

    private:
        /**
         * @brief 新建Session时由Server调用
         * @param ec 返回运行过程中的错误
         */
        asio::awaitable<void> run(usbipdcpp::error_code &ec);

        /**
         * @brief 置停止标志位，并且关闭socket。只能由Server调用
         */
        void immediately_stop();

        /**
         * @brief 不停地传输urb
         * @param transferring_ec 传输urb途中的ec
         * @return
         */
        asio::awaitable<void> transfer_loop(usbipdcpp::error_code &transferring_ec);

        //防止urb还没处理好,session对象就析构了
        void start_processing_urb();
        //防止urb还没处理好,session对象就析构了
        void end_processing_urb();
        //防止urb还没处理好,session对象就析构了
        asio::awaitable<void> wait_for_all_urb_processed();

        std::atomic_bool should_immediately_stop;

        //是否在传输ret_submit的阶段
        std::atomic_bool cmd_transferring = false;

        //传输过程中不允许为空，传输过程中禁止任何写入。不允许在非网络线程读，除非加锁
        std::optional<std::string> current_import_device_id = std::nullopt;
        //传输过程中不允许为空，传输过程中禁止任何写入。不允许在非网络线程读，除非加锁
        std::shared_ptr<UsbDevice> current_import_device = nullptr;
        //上面两个变量的值的锁
        std::shared_mutex current_import_device_data_mutex;

        //从原本的ret_submit的seqnum映射到ret_unlink的seqnum
        std::map<std::uint32_t, std::uint32_t> unlink_map;
        std::shared_mutex unlink_map_mutex;

        Server &server;
        asio::ip::tcp::socket socket;


        std::uint32_t urb_processing_counter = 0;
        asio::experimental::channel<void(asio::error_code)> no_urb_processing_notify_channel;
    };
}
