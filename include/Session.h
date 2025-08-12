#pragma once

#include <atomic>
#include <map>
#include <shared_mutex>
#include <condition_variable>
#include <tuple>

#include <asio/ip/tcp.hpp>
#include <asio/awaitable.hpp>

#include "protocol.h"
#include "type.h"

namespace usbipdcpp {
    class Server;

    class Session {
    public:
        Session(Server &server, asio::ip::tcp::socket &&socket);

        asio::awaitable<void> run(usbipdcpp::error_code &ec);

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

        void stop();

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


        ~Session() = default;


    private:
        //防止urb还没处理好,session对象就析构了
        void end_processing_urb();
        //防止urb还没处理好,session对象就析构了
        void start_processing_urb();
        //防止urb还没处理好,session对象就析构了
        void wait_for_all_urb_processed();


        std::atomic<bool> should_stop;

        std::optional<std::string> current_import_device_id = std::nullopt;
        std::shared_ptr<UsbDevice> current_import_device = nullptr;
        std::shared_mutex current_import_device_data_mutex;

        //从原本的ret_submit的seqnum映射到ret_unlink的seqnum
        std::map<std::uint32_t, std::uint32_t> unlink_map;
        std::shared_mutex unlink_map_mutex;

        Server &server;
        asio::ip::tcp::socket socket;


        std::uint32_t urb_processing_counter = 0;
        std::mutex urb_process_mutex;
        std::condition_variable urb_process_cv;
    };
}
