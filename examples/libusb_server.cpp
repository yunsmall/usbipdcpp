#include <asio.hpp>
#include <iostream>
#include <print>
#include <libusb-1.0/libusb.h>
#include <spdlog/spdlog.h>

#include "libusb_handler/LibusbServer.h"

using namespace usbipcpp;

int main() {
    spdlog::set_level(spdlog::level::trace);
    int err;
    err = libusb_init(nullptr);
    if (err) {
        SPDLOG_ERROR("libusb_init failed: {}", libusb_strerror(err));
        libusb_exit(nullptr);
        return 1;
    }

    LibusbServer server;

    asio::ip::tcp::endpoint endpoint(asio::ip::tcp::v4(), 54322);
    server.start(endpoint);

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
            case 'l': {
                spdlog::info("列出主机所有设备");
                LibusbServer::list_host_devices();
                break;
            }
            case 'b': {
                spdlog::info("绑定设备");
                std::string target_busid;
                std::cin >> target_busid;
                auto device = server.find_by_busid(target_busid);
                if (device) {
                    server.bind_host_device(device);
                }
                else {
                    spdlog::warn("找不到busid为{}的设备", target_busid);
                }
                break;
            }
            case 'u': {
                spdlog::info("解绑设备");
                std::string target_busid;
                std::cin >> target_busid;
                auto device = LibusbServer::find_by_busid(target_busid);
                if (device) {
                    server.unbind_host_device(device);
                }
                else {
                    spdlog::warn("找不到busid为{}的设备", target_busid);
                }
                break;
            }
            case 'q': {
                spdlog::info("尝试关闭服务器");
                server.stop();
                spdlog::info("成功关闭服务器");
                goto loop_end;
                break;
            }
            case 'h': {
                std::cout<<R"(
h 打印本帮助
l 打印本设备所有的可用usb设备
b busid 尝试将一个设备导出
u busid 尝试将一个设备取消导出
q 关闭服务器
)";
                break;
            }
            default: {
                spdlog::warn("未知终端命令");
                break;
            }
        }
    }
loop_end:


    libusb_exit(nullptr);
    return 0;
}
