#include "libusb_handler/LibusbServer.h"

#include <iostream>


#include "libusb_handler/LibusbDeviceHandler.h"
#include "libusb_handler/tools.h"

usbipdcpp::LibusbServer::LibusbServer(std::function<bool(libusb_device *)> filter) {
}

usbipdcpp::LibusbServer::LibusbServer():
    LibusbServer([](auto dev) { return true; }) {
}

std::pair<std::string, std::string> usbipdcpp::LibusbServer::get_device_names(libusb_device *device) {
    libusb_device_handle *handle = nullptr;
    libusb_device_descriptor desc;
    int ret = libusb_get_device_descriptor(device, &desc);
    if (ret < 0) {
        return {"Unknown", "Unknown"};
    }

    // 尝试打开设备获取字符串描述符
    char manufacturer[256] = {0};
    char product[256] = {0};

    if (libusb_open(device, &handle) == 0) {
        if (desc.iManufacturer) {
            libusb_get_string_descriptor_ascii(handle, desc.iManufacturer,
                                               reinterpret_cast<unsigned char *>(manufacturer), sizeof(manufacturer));
        }

        if (desc.iProduct) {
            libusb_get_string_descriptor_ascii(handle, desc.iProduct,
                                               reinterpret_cast<unsigned char *>(product), sizeof(product));
        }

        libusb_close(handle);
    }

    return {
            (manufacturer[0] ? manufacturer : "Unknown Manufacturer"),
            (product[0] ? product : "Unknown Product")
    };
}

void usbipdcpp::LibusbServer::print_device(libusb_device *dev) {
    libusb_device_descriptor desc{};
    // 获取设备描述符
    auto err = libusb_get_device_descriptor(dev, &desc);
    if (err) {
        SPDLOG_ERROR("无法获取设备描述符：{}", libusb_strerror(err));
        return;
    }
    // 打印设备信息
    auto device_name = get_device_names(dev);
    auto busid = get_device_busid(dev);
    bool is_used = false;
    bool is_available = false;
    {
        std::shared_lock lock(devices_mutex);
        if (using_devices.contains(busid)) {
            is_used = true;
        }
        for (auto it = available_devices.begin(); it != available_devices.end(); ++it) {
            if ((*it)->busid == busid) {
                is_available = true;
            }
        }
    }
    std::cout << fmt::format("Device name: {}-{} ({})", device_name.first, device_name.second,
                             is_used ? "exported" : (is_available ? "available" : "unbinded")) << std::endl;
    std::cout << fmt::format("busid: {}", busid) << std::endl;
    std::cout << fmt::format("  VID: 0x{:2x}", desc.idVendor) << std::endl;
    std::cout << fmt::format("  PID: 0x{:2x}", desc.idProduct) << std::endl;
    auto version = Version(desc.bcdUSB);
    std::cout << fmt::format("  USB version: {}.{}.{}", version.major, version.minor, version.patch) << std::endl;
    std::cout << fmt::format("  Class: 0x{:2x}", static_cast<int>(desc.bDeviceClass)) << std::endl;
    std::cout << "  Speed: ";
    // 解析设备速度
    switch (libusb_get_device_speed(dev)) {
        case LIBUSB_SPEED_LOW:
            std::cout << "1.5 Mbps (Low)";
            break;
        case LIBUSB_SPEED_FULL:
            std::cout << "12 Mbps (Full)";
            break;
        case LIBUSB_SPEED_HIGH:
            std::cout << "480 Mbps (High)";
            break;
        case LIBUSB_SPEED_SUPER:
            std::cout << "5 Gbps (Super)";
            break;
        case LIBUSB_SPEED_SUPER_PLUS:
            std::cout << "10 Gbps (Super+)";
            break;
        default:
            std::cout << "Unknown speed";
    }
    std::cout << std::endl;
}

void usbipdcpp::LibusbServer::list_host_devices() {
    libusb_device **devs;
    auto dev_nums = libusb_get_device_list(nullptr, &devs);
    for (auto dev_i = 0; dev_i < dev_nums; dev_i++) {
        print_device(devs[dev_i]);
        std::cout << std::endl;
    }
    libusb_free_device_list(devs, 1);
}

libusb_device *usbipdcpp::LibusbServer::find_by_busid(const std::string &busid) {
    libusb_device **devs;
    int dev_nums = libusb_get_device_list(nullptr, &devs);
    for (auto dev_i = 0; dev_i < dev_nums; dev_i++) {
        auto current_bus = get_device_busid(devs[dev_i]);
        if (current_bus == busid) {
            auto ret_dev = libusb_ref_device(devs[dev_i]);
            libusb_free_device_list(devs, 1);
            return ret_dev;
        }
    }
    libusb_free_device_list(devs, 1);
    return nullptr;
}

void usbipdcpp::LibusbServer::claim_interface(libusb_device_handle *dev_handle, std::error_code &ec) {


}

void usbipdcpp::LibusbServer::claim_interfaces(libusb_device *dev, std::error_code &ec) {
}

void usbipdcpp::LibusbServer::release_interface(libusb_device_handle *dev_handle, std::error_code &ec) {
}

void usbipdcpp::LibusbServer::release_interfaces(libusb_device *dev, std::error_code &ec) {
}

void usbipdcpp::LibusbServer::bind_host_device(libusb_device *dev) {
    libusb_device_handle *dev_handle;
    int err = libusb_open(dev, &dev_handle);
    if (err) {
        spdlog::warn("无法打开一个设备，忽略这个设备：{}", libusb_strerror(err));
        libusb_unref_device(dev);
        return;
    }

    libusb_device_descriptor device_descriptor{};
    err = libusb_get_device_descriptor(dev, &device_descriptor);
    if (err) {
        spdlog::warn("无法获取设备述符，忽略这个设备：{}", libusb_strerror(err));
        libusb_close(dev_handle);
        libusb_unref_device(dev);
        return;
    }

    struct libusb_config_descriptor *active_config_desc;
    err = libusb_get_active_config_descriptor(dev, &active_config_desc);
    if (err) {
        spdlog::warn("无法获取设备当前的配置描述符，忽略这个设备：{}", libusb_strerror(err));
        libusb_close(dev_handle);
        libusb_unref_device(dev);
        return;
    }
    err = libusb_set_auto_detach_kernel_driver(dev_handle, true);
    if (err == LIBUSB_ERROR_NOT_SUPPORTED) {
        SPDLOG_TRACE("系统不支持自动卸载内核设备");
    }
    else if (err) {
        SPDLOG_WARN("无法自动卸载内核设备，忽略这个设备：{}", libusb_strerror(err));
        libusb_free_config_descriptor(active_config_desc);
        libusb_close(dev_handle);
        libusb_unref_device(dev);
        return;
    }
    SPDLOG_DEBUG("该设备有{}个interface", active_config_desc->bNumInterfaces);
    std::vector<UsbInterface> interfaces;
    for (auto intf_i = 0; intf_i < active_config_desc->bNumInterfaces; intf_i++) {
        auto &intf = active_config_desc->interface[intf_i];
        SPDLOG_DEBUG("第{}个interface有{}个altsetting", intf_i, intf.num_altsetting);
        //只使用第一个alsetting
        auto &intf_desc = intf.altsetting[0];

        err = libusb_set_auto_detach_kernel_driver(dev_handle, true);
        if (err == LIBUSB_ERROR_NOT_SUPPORTED) {
            SPDLOG_TRACE("系统不支持自动卸载内核设备");
        }
        else if (err) {
            SPDLOG_WARN("无法自动卸载内核设备，忽略这个设备：{}", libusb_strerror(err));
            libusb_free_config_descriptor(active_config_desc);
            libusb_close(dev_handle);
            libusb_unref_device(dev);
            return;
        }

        err = libusb_claim_interface(dev_handle, intf_i);
        if (err) {
            SPDLOG_ERROR("无法声明接口{}：{}", intf_i, libusb_strerror(err));
            libusb_free_config_descriptor(active_config_desc);
            libusb_close(dev_handle);
            libusb_unref_device(dev);
            return;
        }

        std::vector<UsbEndpoint> endpoints;
        endpoints.reserve(intf_desc.bNumEndpoints);
        for (auto ep_i = 0; ep_i < intf_desc.bNumEndpoints; ep_i++) {
            endpoints.emplace_back(
                    intf_desc.endpoint[ep_i].bEndpointAddress,
                    intf_desc.endpoint[ep_i].bmAttributes,
                    intf_desc.endpoint[ep_i].wMaxPacketSize,
                    intf_desc.endpoint[ep_i].bInterval
                    );
        }
        interfaces.emplace_back(
                UsbInterface{
                        intf_desc.bInterfaceClass,
                        intf_desc.bInterfaceSubClass,
                        intf_desc.bInterfaceProtocol,
                        std::move(endpoints)
                }
                //直接全用libusb控制，不用走端口
                // .with_handler<LibusbInterfaceHandler>()
                );
    }

    {
        std::lock_guard lock(devices_mutex);
        auto current_device = std::make_shared<UsbDevice>(UsbDevice{
                .path = std::format("/sys/bus/{}/{}/{}", libusb_get_bus_number(dev),
                                    libusb_get_device_address(dev), libusb_get_port_number(dev)),
                .busid = get_device_busid(dev),
                .bus_num = libusb_get_bus_number(dev),
                .dev_num = libusb_get_port_number(dev),
                .speed = (std::uint32_t) libusb_speed_to_usb_speed(libusb_get_device_speed(dev)),
                .vendor_id = device_descriptor.idVendor,
                .product_id = device_descriptor.idProduct,
                .device_bcd = device_descriptor.bcdDevice,
                .device_class = device_descriptor.bDeviceClass,
                .device_subclass = device_descriptor.bDeviceSubClass,
                .device_protocol = device_descriptor.bDeviceProtocol,
                .configuration_value = active_config_desc->bConfigurationValue,
                .num_configurations = device_descriptor.bNumConfigurations,
                .interfaces = std::move(interfaces),
                .ep0_in = UsbEndpoint::get_ep0_in(device_descriptor.bMaxPacketSize0),
                .ep0_out = UsbEndpoint::get_ep0_out(device_descriptor.bMaxPacketSize0),
        });
        current_device->with_handler<LibusbDeviceHandler>(dev_handle);
        available_devices.emplace_back(std::move(current_device));
    }
    libusb_free_config_descriptor(active_config_desc);
    libusb_unref_device(dev);
}

void usbipdcpp::LibusbServer::unbind_host_device(libusb_device *device) {
    auto target_busid = get_device_busid(device);
    {
        std::shared_lock lock(devices_mutex);
        for (auto i = available_devices.begin(); i != available_devices.end(); ++i) {
            if ((*i)->busid == target_busid) {

                auto libusb_device_handler = std::dynamic_pointer_cast<LibusbDeviceHandler>((*i)->handler);
                if (libusb_device_handler) {
                    struct libusb_config_descriptor *active_config_desc;
                    auto err = libusb_get_active_config_descriptor(device, &active_config_desc);
                    for (int intf_i = 0; intf_i < active_config_desc->bNumInterfaces; intf_i++) {
                        err = libusb_release_interface(libusb_device_handler->native_handle, intf_i);
                        if (err) {
                            SPDLOG_ERROR("释放设备接口时出错: {}", libusb_strerror(err));
                        }
                    }
                    libusb_close(libusb_device_handler->native_handle);
                }
                available_devices.erase(i);
                libusb_unref_device(device);
                spdlog::info("成功取消绑定");
                return;
            }
        }
        SPDLOG_WARN("可使用的设备中无目标设备");

        if (using_devices.contains(target_busid)) {
            SPDLOG_WARN("正在使用的设备不支持解绑");
        }
    }
    libusb_unref_device(device);
}

void usbipdcpp::LibusbServer::refresh_available_devices() {
    libusb_device **devs;
    int dev_nums = libusb_get_device_list(nullptr, &devs);

    {
        std::lock_guard lock(devices_mutex);
        for (auto i = available_devices.begin(); i != available_devices.end(); ++i) {
            auto libusb_device_handler = std::dynamic_pointer_cast<LibusbDeviceHandler>((*i)->handler);
            if (libusb_device_handler) {
                auto device = libusb_get_device(libusb_device_handler->native_handle);
                bool found = false;
                for (size_t device_i = 0; device_i < dev_nums; device_i++) {
                    if (device == devs[device_i]) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    i = available_devices.erase(i);
                }
            }
        }
    }

    libusb_free_device_list(devs, 1);
}

void usbipdcpp::LibusbServer::start(asio::ip::tcp::endpoint &ep) {
    Server::start(ep);
    libusb_event_thread = std::thread([this]() {
        try {
            SPDLOG_INFO("启动一个libusb device handle的libusb事件循环线程");
            while (!should_exit_libusb_event_thread) {
                auto ret = libusb_handle_events(nullptr);
                if (ret == LIBUSB_ERROR_INTERRUPTED && should_exit_libusb_event_thread) {
                    SPDLOG_INFO("libusb事件循环收到中断信号正常退出");
                    break;
                }
                if (ret < 0 && ret != LIBUSB_ERROR_INTERRUPTED) {
                    fprintf(stderr, "Event handling error: %s\n", libusb_error_name(ret));
                    break;
                }
            }
            SPDLOG_TRACE("退出libusb事件循环");
        } catch (const std::exception &e) {
            SPDLOG_ERROR("An unexpected exception occurs in libusb handler thread: {}", e.what());
            std::exit(1);
        }
    });
}

void usbipdcpp::LibusbServer::stop() {
    Server::stop();

    {
        std::shared_lock lock(devices_mutex);

        for (auto i = available_devices.begin(); i != available_devices.end(); ++i) {
            if (auto libusb_device_handler = std::dynamic_pointer_cast<LibusbDeviceHandler>((*i)->handler)) {
                if (auto device = libusb_get_device(libusb_device_handler->native_handle)) {
                    struct libusb_config_descriptor *active_config_desc;
                    auto err = libusb_get_active_config_descriptor(device, &active_config_desc);
                    for (int intf_i = 0; intf_i < active_config_desc->bNumInterfaces; intf_i++) {
                        err = libusb_release_interface(libusb_device_handler->native_handle, intf_i);
                        if (err) {
                            SPDLOG_ERROR("释放设备接口{}时出错: {}", intf_i, libusb_strerror(err));
                        }
                    }
                    libusb_close(libusb_device_handler->native_handle);
                }
                else {
                    SPDLOG_ERROR("无法获取device的handle");
                }
            }
        }


        for (auto using_dev_i = using_devices.begin(); using_dev_i != using_devices.end(); ++using_dev_i) {
            if (auto libusb_device_handler = std::dynamic_pointer_cast<LibusbDeviceHandler>(
                    using_dev_i->second->handler)) {
                if (auto device = libusb_get_device(libusb_device_handler->native_handle)) {
                    struct libusb_config_descriptor *active_config_desc;
                    auto err = libusb_get_active_config_descriptor(device, &active_config_desc);
                    for (int intf_i = 0; intf_i < active_config_desc->bNumInterfaces; intf_i++) {
                        err = libusb_release_interface(libusb_device_handler->native_handle, intf_i);
                        if (err) {
                            SPDLOG_ERROR("释放设备接口时出错: {}", libusb_strerror(err));
                        }
                    }
                    libusb_close(libusb_device_handler->native_handle);
                }
                else {
                    SPDLOG_ERROR("无法获取device的handle");
                }
            }

        }
    }

    should_exit_libusb_event_thread = true;
    libusb_interrupt_event_handler(nullptr);
    spdlog::info("等待libusb事件线程结束");
    libusb_event_thread.join();
    spdlog::info("libusb事件线程结束");
}

// void usbipcpp::LibusbServer::add_device(std::shared_ptr<UsbDevice> &&device) {
//     Server::add_device(std::move(device));
// }
//
// bool usbipcpp::LibusbServer::remove_device(const std::string &busid) {
//     return Server::remove_device(busid);
// }

usbipdcpp::LibusbServer::~LibusbServer() {
}
