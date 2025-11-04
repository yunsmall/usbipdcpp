#include <asio.hpp>
#include <iostream>
#include <libusb-1.0/libusb.h>
#include <spdlog/spdlog.h>

#include "LibusbHandler/LibusbServer.h"

using namespace usbipdcpp;

int main() {
    spdlog::set_level(spdlog::level::trace);
    int err;
    err = libusb_init(nullptr);
    if (err) {
        SPDLOG_ERROR("libusb_init failed: {}", libusb_strerror(err));
        libusb_exit(nullptr);
        return 1;
    }

    LibusbServer libusb_server;

    asio::ip::tcp::endpoint endpoint(asio::ip::tcp::v4(), 54322);
    libusb_server.start(endpoint);

    // SPDLOG_DEBUG("直接绑定3-5-1");
    // server.bind_host_device(server.find_by_busid("3-5-1"));
    // auto target_busid = "1-1";
    // SPDLOG_DEBUG("直接绑定1-1");
    // auto found = LibusbServer::find_by_busid("1-1");
    // if (found) {
    //     server.bind_host_device(found);
    // }
    // else {
    //     SPDLOG_ERROR("不存在设备{}", target_busid);
    // }


    char cmd;
    while (true) {
        std::cin >> cmd;
        switch (cmd) {
            case 's': {
                spdlog::info("There are {} sessions in this server", libusb_server.get_server().get_session_count());
                break;
            }
            case 'l': {
                spdlog::info("List all usb devices in the host");
                libusb_server.list_host_devices();
                break;
            }
            case 'd': {
                libusb_server.get_server().print_bound_devices();
                break;
            }
            case 'b': {
                spdlog::info("Binding device");
                std::string target_busid;
                std::cin >> target_busid;
                auto device = LibusbServer::find_by_busid(target_busid);
                if (device) {
                    libusb_server.bind_host_device(device);
                }
                else {
                    spdlog::warn("Can't find a device with busid {}", target_busid);
                }
                break;
            }
            case 'u': {
                spdlog::info("Unbinding device");
                std::string target_busid;
                std::cin >> target_busid;
                auto device = LibusbServer::find_by_busid(target_busid);
                if (device) {
                    libusb_server.unbind_host_device(device);
                }
                //主机上找不到，这个设备，如果还处于绑定状态但找不到则需要清除这个设备
                else if (libusb_server.get_server().has_bound_device(target_busid)) {
                    spdlog::warn("Can't find target busid {} in server, but it has been bound."
                                 "Has it been removed?", target_busid);
                    spdlog::warn("Try remove dead device:{}", target_busid);
                    libusb_server.try_remove_dead_device(target_busid);
                }
                else {
                    spdlog::warn("Can't find a device with busid {}", target_busid);
                }
                break;
            }
            case 'f': {
                spdlog::info("Refresh available device");
                libusb_server.refresh_available_devices();
                break;
            }
            case 'q': {
                spdlog::info("Trying to close server");
                libusb_server.stop();
                spdlog::info("Closed server successfully");
                goto loop_end;
                break;
            }

            default: {
                spdlog::warn("Unknown command {}", cmd);
            }
            case 'h': {
                std::cout << R"(
h : Print this help information.
s : show how many sessions the server has.
l : List all usb devices in the host.
d : Show all bound devices.
b busid : Try to bind a device.
u busid : Try to unbind a device.
f : refresh available devices.
q : Close the server.)" << std::endl;
                break;
            }
        }
    }
loop_end:


    libusb_exit(nullptr);
    return 0;
}
