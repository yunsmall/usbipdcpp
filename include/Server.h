#pragma once

#include <vector>
#include <map>
#include <shared_mutex>
#include <memory>
#include <list>
#include <thread>

#include <asio/ip/tcp.hpp>

#include "device.h"


namespace usbipdcpp {
    class Session;

    class Server {
    public:
        friend class Session;

        Server() = default;
        explicit Server(std::vector<UsbDevice> &&devices);
        /**
         * @brief 不阻塞地启动一个服务器，内部启动了一个获取socket的线程
         * @param ep 监听地址
         */
        virtual void start(asio::ip::tcp::endpoint &ep);
        /**
         * @brief 关闭服务，内部关闭监听线程，关闭每个device，关闭每个当前连接
         */
        virtual void stop();

        virtual void add_device(std::shared_ptr<UsbDevice> &&device);

        // virtual bool remove_device(const std::string &busid);

        virtual ~Server();

    protected:
        asio::awaitable<void> do_accept(asio::ip::tcp::acceptor &acceptor);

        bool is_device_using(const std::string &busid);

        void try_moving_device_to_available(const std::string &busid);

        /**
         * @brief Try to move device to using_devices, and return this device,
         * return nullptr if there is no such device in available_devices or moved failed.
         * @param busid device busid
         * @return device or nullptr when error
         */
        std::shared_ptr<UsbDevice> try_moving_device_to_using(const std::string &busid);

        void print_devices();

        //可供导入的设备
        std::vector<std::shared_ptr<UsbDevice>> available_devices;
        //正在使用的设备，busid做索引只供索引使用，与usbip协议无关
        std::map<std::string, std::shared_ptr<UsbDevice>> using_devices;
        //锁available_devices和using_devices两个变量
        std::shared_mutex devices_mutex;

        std::atomic<bool> should_stop = false;

        std::list<std::shared_ptr<Session>> sessions;
        std::shared_mutex session_list_mutex;


        //网络通信请异步使用这个io_context
        asio::io_context asio_io_context;
        //所有网络通信请运行在下面这个线程，网络通信不可运行在其他线程中
        std::thread network_io_thread;
    };
}
