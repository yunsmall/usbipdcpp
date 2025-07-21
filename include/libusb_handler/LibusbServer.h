#pragma once

#include <functional>
#include <asio.hpp>
#include <libusb-1.0/libusb.h>

#include "Server.h"

namespace usbipcpp {
    class LibusbServer : public Server {
    public:
        explicit LibusbServer(std::function<bool(libusb_device *)> filter);
        LibusbServer();

        void bind_host_device(libusb_device *dev);
        void unbind_host_device(libusb_device *device);
        void start(asio::ip::tcp::endpoint &ep) override;
        void stop() override;
        // void add_device(std::shared_ptr<UsbDevice> &&device) override;
        // bool remove_device(const std::string &busid) override;
        ~LibusbServer() override;

        static std::pair<std::string, std::string> get_device_names(libusb_device* device);
        static void print_device(libusb_device *dev);
        static void list_host_devices();
        static libusb_device *find_by_busid(const std::string &busid);
    protected:
        void claim_interface(libusb_device_handle* dev_handle, std::error_code& ec);
        void claim_interfaces(libusb_device *dev,std::error_code& ec);

        void release_interface(libusb_device_handle* dev_handle, std::error_code& ec);
        void release_interfaces(libusb_device *dev,std::error_code& ec);

        std::atomic<bool> should_exit_libusb_event_thread=false;

        //不可在这个线程发送网络包
        std::thread libusb_event_thread;

        std::vector<libusb_device *> host_devices;
    };
}
