#pragma once

#include <functional>
#include <asio.hpp>
#include <libusb-1.0/libusb.h>

#include "Server.h"

namespace usbipdcpp {
    class LibusbServer {
    public:
        LibusbServer();

        /**
         * @brief If dev has value, then this function will own the value.
         * @param dev target device
         * @param use_handle use already exit dev handle to bind a host device, most time it's for Android
         * @param exist_handle an existing handle
         */
        void bind_host_device(libusb_device *dev, bool use_handle = false,
                              libusb_device_handle *exist_handle = nullptr);
        /**
         * @brief this function will also own the device argument.
         * @param device target device
         */
        void unbind_host_device(libusb_device *device);

        /**
         * @brief 禁止传入仍然可用的busid，只会删除libusb的设备，其他设备不处理
         * @param busid
         */
        void try_remove_dead_device(const std::string &busid);
        void refresh_available_devices();
        void start(asio::ip::tcp::endpoint &ep);
        void stop();
        // void add_device(std::shared_ptr<UsbDevice> &&device) override;
        // bool remove_device(const std::string &busid) override;
        ~LibusbServer();

        void print_device(libusb_device *dev);
        void list_host_devices();

        Server &get_server() {
            return server;
        }

        static std::pair<std::string, std::string> get_device_names(libusb_device *device);

        /**
         * @brief return a libusb_device pointer which you need to call libusb_unref_device after using.
         * @param busid
         * @return nullptr is not found else found
         */
        static libusb_device *find_by_busid(const std::string &busid);

    protected:
        Server server;

        std::atomic<bool> should_exit_libusb_event_thread = false;

        //不可在这个线程发送网络包
        std::thread libusb_event_thread;

        std::vector<libusb_device *> host_devices;
    };
}
