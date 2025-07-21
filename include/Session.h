#pragma once

#include <atomic>
#include <variant>
#include <map>
#include <shared_mutex>
#include <tuple>
#include <tuple>

#include <asio/ip/tcp.hpp>
#include <asio/awaitable.hpp>

#include "protocol.h"
#include "type.h"

namespace usbipcpp {
    class Server;

    class Session {
    public:
        Session(Server &server, asio::ip::tcp::socket &&socket);

        asio::awaitable<void> run(usbipcpp::error_code &ec);

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
         * @brief 该函数异步，不阻塞。内部直接向asio context提交任务，因此不用加锁。内部线程安全
         * @param unlink
         */
        void submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink &&unlink);
        /**
         * @brief 该函数异步，不阻塞。内部直接向asio context提交任务，因此不用加锁。内部线程安全
         * @param submit
         */
        void submit_ret_submit(UsbIpResponse::UsbIpRetSubmit &&submit);


        ~Session() = default;

    private:
        std::atomic<bool> should_stop;

        std::optional<std::string> current_import_device_id = std::nullopt;
        std::shared_ptr<UsbDevice> current_import_device = nullptr;
        std::shared_mutex current_import_device_data_mutex;

        //从原本的ret_submit的seqnum映射到ret_unlink的seqnum
        std::map<std::uint32_t, std::uint32_t> unlink_map;
        std::shared_mutex unlink_map_mutex;

        Server &server;
        asio::ip::tcp::socket socket;
    };
}
