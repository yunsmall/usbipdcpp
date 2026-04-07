#include "LibusbHandler/LibusbServer.h"

#include <iostream>


#include "LibusbHandler/LibusbDeviceHandler.h"
#include "LibusbHandler/tools.h"

usbipdcpp::LibusbServer::LibusbServer() {
    server.register_session_exit_callback([this]() {
        std::lock_guard lock(server.get_devices_mutex());
        auto &server_available_devices = server.get_available_devices();
        for (auto it = server_available_devices.begin(); it != server_available_devices.end();) {
            bool removed = false;
            if (auto libusb_handle = std::dynamic_pointer_cast<LibusbDeviceHandler>((*it)->handler)) {
                if (libusb_handle->device_removed) {
                    it = server_available_devices.erase(it);
                    removed = true;
                }
            }
            if (!removed) {
                ++it;
            }
        }
    });
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
        std::shared_lock lock(server.get_devices_mutex());
        auto &server_using_devices = server.get_using_devices();
        auto &server_available_devices = server.get_available_devices();
        if (server_using_devices.contains(busid)) {
            is_used = true;
        }
        for (auto it = server_available_devices.begin(); it != server_available_devices.end(); ++it) {
            if ((*it)->busid == busid) {
                is_available = true;
            }
        }
    }
    std::cout << std::format("Device name: {}-{} ({})", device_name.first, device_name.second,
                             is_used ? "exported" : (is_available ? "available" : "unbinded")) << std::endl;
    std::cout << std::format("busid: {}", busid) << std::endl;
    std::cout << std::format("  VID: 0x{:2x}", desc.idVendor) << std::endl;
    std::cout << std::format("  PID: 0x{:2x}", desc.idProduct) << std::endl;
    auto version = Version(desc.bcdUSB);
    std::cout << std::format("  USB version: {}.{}.{}", version.major, version.minor, version.patch) << std::endl;
    std::cout << std::format("  Class: 0x{:2x}", static_cast<int>(desc.bDeviceClass)) << std::endl;
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

void usbipdcpp::LibusbServer::bind_host_device(libusb_device *dev, bool use_handle,
                                               libusb_device_handle *exist_handle) {
    libusb_device_handle *dev_handle;
    int err;
    if (!use_handle) {
        assert(dev != nullptr && "If use_handle != true then dev can't be nullptr");
        err = libusb_open(dev, &dev_handle);
        if (err) {
            spdlog::warn("无法打开一个设备，忽略这个设备：{}", libusb_strerror(err));
            libusb_unref_device(dev);
            return;
        }
    }
    else {
        assert(dev == nullptr && "If use_handle == true then dev must be nullptr");
        assert(exist_handle != nullptr && "exist_handle can't be nullptr");
        dev_handle = exist_handle;
        dev = libusb_get_device(dev_handle);
        if (dev == nullptr) {
            SPDLOG_ERROR("libusb_get_device returns nullptr");
            throw std::runtime_error("libusb_get_device returns nullptr");
        }
        libusb_device *new_dev = libusb_ref_device(dev);
        if (new_dev == nullptr) {
            SPDLOG_ERROR("libusb_ref_device returns nullptr");
            throw std::runtime_error("libusb_ref_device returns nullptr");
        }
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
    SPDLOG_DEBUG("该设备有{}个interface", active_config_desc->bNumInterfaces);
    std::vector<UsbInterface> interfaces;
    for (auto intf_i = 0; intf_i < active_config_desc->bNumInterfaces; intf_i++) {
        auto &intf = active_config_desc->interface[intf_i];
        SPDLOG_DEBUG("第{}个interface有{}个altsetting", intf_i, intf.num_altsetting);
        //只使用第一个alsetting
        auto &intf_desc = intf.altsetting[0];

        // 手动解绑内核驱动
        err = libusb_detach_kernel_driver(dev_handle, intf_i);
        if (err && err != LIBUSB_ERROR_NOT_FOUND) {
            SPDLOG_WARN("无法卸载接口{}的内核驱动：{}", intf_i, libusb_strerror(err));
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
        std::lock_guard lock(server.get_devices_mutex());
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
        server.get_available_devices().emplace_back(std::move(current_device));
    }
    libusb_free_config_descriptor(active_config_desc);
    libusb_unref_device(dev);
}

void usbipdcpp::LibusbServer::unbind_host_device(libusb_device *device) {
    auto target_busid = get_device_busid(device);
    {
        std::lock_guard lock(server.get_devices_mutex());
        auto &server_using_devices = server.get_using_devices();
        auto &server_available_devices = server.get_available_devices();
        for (auto i = server_available_devices.begin(); i != server_available_devices.end(); ++i) {
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
                        // 重新让内核驱动接管
                        err = libusb_attach_kernel_driver(libusb_device_handler->native_handle, intf_i);
                        if (err && err != LIBUSB_ERROR_NOT_FOUND && err != LIBUSB_ERROR_NOT_SUPPORTED) {
                            SPDLOG_WARN("重新绑定内核驱动失败: {}", libusb_strerror(err));
                        }
                    }
                    libusb_free_config_descriptor(active_config_desc);
                    libusb_close(libusb_device_handler->native_handle);
                }
                server_available_devices.erase(i);
                libusb_unref_device(device);
                spdlog::info("成功取消绑定");
                return;
            }
        }
        SPDLOG_WARN("可使用的设备中无目标设备");

        if (server_using_devices.contains(target_busid)) {
            SPDLOG_WARN("正在使用的设备不支持解绑");
        }
    }
    libusb_unref_device(device);
}

void usbipdcpp::LibusbServer::try_remove_dead_device(const std::string &busid) {
    std::lock_guard lock(server.get_devices_mutex());
    auto &server_using_devices = server.get_using_devices();
    auto &server_available_devices = server.get_available_devices();
    for (auto i = server_available_devices.begin(); i != server_available_devices.end(); ++i) {
        if ((*i)->busid == busid) {
            if (auto libusb_device_handler = std::dynamic_pointer_cast<LibusbDeviceHandler>((*i)->handler)) {
                libusb_close(libusb_device_handler->native_handle);
                server_available_devices.erase(i);
                spdlog::info("删除可用设备中的{}", busid);
                return;
            }
        }
    }
    if (auto device = server_using_devices.find(busid); device != server_using_devices.end()) {
        if (auto libusb_device_handler = std::dynamic_pointer_cast<
            LibusbDeviceHandler>((*device).second->handler)) {
            libusb_close(libusb_device_handler->native_handle);
            server_using_devices.erase(device);
            spdlog::info("删除正在使用设备中的{}", busid);
            return;
        }
    }
    SPDLOG_WARN("无法找到busid为{}的设备", busid);
}

void usbipdcpp::LibusbServer::refresh_available_devices() {
    libusb_device **devs;
    auto get_ret = libusb_get_device_list(nullptr, &devs);
    if (get_ret < 0) {
        SPDLOG_WARN("libusb_get_device_list error: {}", libusb_strerror(get_ret));
        return;
    }
    size_t dev_nums = get_ret;

    {
        std::lock_guard lock(server.get_devices_mutex());
        auto &server_available_devices = server.get_available_devices();
        for (auto i = server_available_devices.begin(); i != server_available_devices.end();) {
            auto libusb_device_handler = std::dynamic_pointer_cast<LibusbDeviceHandler>((*i)->handler);
            bool removed = false;
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
                    i = server_available_devices.erase(i);
                    removed = true;
                }
            }

            if (!removed)
                ++i;
        }
    }

    libusb_free_device_list(devs, 1);
}

void usbipdcpp::LibusbServer::start_hotplug_monitor() {
    if (!hotplug_enabled_by_user_) {
        SPDLOG_DEBUG("热插拔监控已被用户禁用");
        return;
    }

    if (!libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG)) {
        SPDLOG_WARN("当前 libusb 不支持热插拔");
        return;
    }

    int ret = libusb_hotplug_register_callback(
        nullptr,
        static_cast<libusb_hotplug_event>(
            LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT
        ),
        LIBUSB_HOTPLUG_NO_FLAGS,
        LIBUSB_HOTPLUG_MATCH_ANY,
        LIBUSB_HOTPLUG_MATCH_ANY,
        LIBUSB_HOTPLUG_MATCH_ANY,
        hotplug_callback,
        this,
        &hotplug_handle_
    );

    if (ret == 0) {
        hotplug_enabled_ = true;
        SPDLOG_INFO("热插拔监控已启动");
    } else {
        SPDLOG_ERROR("注册热插拔回调失败: {}", libusb_strerror(ret));
    }
}

void usbipdcpp::LibusbServer::stop_hotplug_monitor() {
    if (hotplug_enabled_) {
        libusb_hotplug_deregister_callback(nullptr, hotplug_handle_);
        hotplug_enabled_ = false;
        SPDLOG_INFO("热插拔监控已停止");
    }
}

int LIBUSB_CALL usbipdcpp::LibusbServer::hotplug_callback(
    libusb_context *ctx,
    libusb_device *device,
    libusb_hotplug_event event,
    void *user_data)
{
    auto *server = static_cast<LibusbServer*>(user_data);

    if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED) {
        server->handle_device_arrived(device);
    } else if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT) {
        auto busid = get_device_busid(device);
        server->handle_device_left(busid);
    }

    return 0;
}

void usbipdcpp::LibusbServer::handle_device_arrived(libusb_device *device) {
    auto busid = get_device_busid(device);

    // 检查是否已绑定
    {
        std::shared_lock lock(server.get_devices_mutex());
        for (const auto &dev : server.get_available_devices()) {
            if (dev->busid == busid) {
                SPDLOG_DEBUG("设备 {} 已在已绑定列表中", busid);
                return;
            }
        }
        if (server.get_using_devices().contains(busid)) {
            SPDLOG_DEBUG("设备 {} 正在使用中", busid);
            return;
        }
    }

    // 打印设备信息
    SPDLOG_INFO("检测到新设备插入:");
    print_device(device);
}

void usbipdcpp::LibusbServer::handle_device_left(const std::string &busid) {
    SPDLOG_INFO("检测到设备拔出: {}", busid);

    std::lock_guard lock(server.get_devices_mutex());
    auto &available_devices = server.get_available_devices();
    auto &using_devices = server.get_using_devices();

    // 1. 从 available_devices 中移除
    for (auto it = available_devices.begin(); it != available_devices.end(); ++it) {
        if ((*it)->busid == busid) {
            if (auto handler = std::dynamic_pointer_cast<LibusbDeviceHandler>((*it)->handler)) {
                libusb_close(handler->native_handle);
            }
            available_devices.erase(it);
            SPDLOG_INFO("已从已绑定设备列表移除: {}", busid);
            return;
        }
    }

    // 2. 如果正在使用中，触发断连
    if (auto it = using_devices.find(busid); it != using_devices.end()) {
        if (auto handler = it->second->handler) {
            // 通过 AbstDeviceHandler 接口通知（后端无关）
            handler->on_device_removed();
            // 强制关闭 Session
            SPDLOG_WARN("正在使用的设备被拔出，强制关闭 Session: {}", busid);
            handler->trigger_session_stop();
        }
    }
}

void usbipdcpp::LibusbServer::start(asio::ip::tcp::endpoint &ep) {
    start_hotplug_monitor();

# ifndef USBIPDCPP_ENABLE_BUSY_WAIT
    should_exit_libusb_event_thread = false;
# endif

# ifdef USBIPDCPP_ENABLE_BUSY_WAIT
    // busy-wait 模式：设置回调，不创建独立线程
    server.set_busy_wait_callback([]() {
        struct timeval tv = {0, 0};
        libusb_handle_events_timeout(nullptr, &tv);
    });
    SPDLOG_INFO("启用 busy-wait 模式，libusb 事件将在 sender 线程中处理");
    server.start(ep);
# else
    // 原有逻辑：创建独立的 libusb 事件线程
    libusb_event_thread = std::thread([this]() {
        try {
            SPDLOG_INFO("启动一个libusb device handle的libusb事件循环线程");
            while (!should_exit_libusb_event_thread) {
                //usbipd-libusb说这个函数有性能问题，起初我还不信，延时分析了一下发现果真如此
                // auto ret = libusb_handle_events(nullptr);
                struct timeval tv = {0, 0};
                auto ret = libusb_handle_events_timeout(nullptr, &tv);//所以这里直接死循环？

                if (ret == LIBUSB_ERROR_INTERRUPTED && should_exit_libusb_event_thread)[[unlikely]] {
                    SPDLOG_INFO("libusb事件循环收到中断信号正常退出");
                    break;
                }
                if (ret < 0 && ret != LIBUSB_ERROR_INTERRUPTED)[[unlikely]] {
                    SPDLOG_ERROR("Event handling error: {}\n", libusb_strerror(ret));
                    break;
                }
            }
            SPDLOG_TRACE("退出libusb事件循环");
        } catch (const std::exception &e) {
            SPDLOG_ERROR("An unexpected exception occurs in libusb handler thread: {}", e.what());
            std::exit(1);
        }
    });
    server.start(ep);
# endif
}

void usbipdcpp::LibusbServer::stop() {
    stop_hotplug_monitor();
    server.stop();
    SPDLOG_INFO("usbip服务器关闭");

    {
        std::lock_guard lock(server.get_devices_mutex());
        auto &server_using_devices = server.get_using_devices();
        auto &server_available_devices = server.get_available_devices();
        for (auto i = server_available_devices.begin(); i != server_available_devices.end(); ++i) {
            if (auto libusb_device_handler = std::dynamic_pointer_cast<LibusbDeviceHandler>((*i)->handler)) {
                if (auto device = libusb_get_device(libusb_device_handler->native_handle)) {
                    struct libusb_config_descriptor *active_config_desc;
                    auto err = libusb_get_active_config_descriptor(device, &active_config_desc);
                    if (err == 0) {
                        for (int intf_i = 0; intf_i < active_config_desc->bNumInterfaces; intf_i++) {
                            err = libusb_release_interface(libusb_device_handler->native_handle, intf_i);
                            if (err) {
                                SPDLOG_ERROR("释放设备接口{}时出错: {}", intf_i, libusb_strerror(err));
                            }
                            // 重新让内核驱动接管
                            err = libusb_attach_kernel_driver(libusb_device_handler->native_handle, intf_i);
                            if (err && err != LIBUSB_ERROR_NOT_FOUND && err != LIBUSB_ERROR_NOT_SUPPORTED) {
                                SPDLOG_WARN("重新绑定内核驱动失败: {}", libusb_strerror(err));
                            }
                        }
                        libusb_free_config_descriptor(active_config_desc);
                    }
                    libusb_close(libusb_device_handler->native_handle);
                }
                else {
                    SPDLOG_ERROR("无法获取device的handle");
                }
            }
        }
        server_available_devices.clear();

        for (auto using_dev_i = server_using_devices.begin(); using_dev_i != server_using_devices.end(); ++
             using_dev_i) {
            if (auto libusb_device_handler = std::dynamic_pointer_cast<LibusbDeviceHandler>(
                    using_dev_i->second->handler)) {
                if (auto device = libusb_get_device(libusb_device_handler->native_handle)) {
                    struct libusb_config_descriptor *active_config_desc;
                    auto err = libusb_get_active_config_descriptor(device, &active_config_desc);
                    if (err == 0) {
                        for (int intf_i = 0; intf_i < active_config_desc->bNumInterfaces; intf_i++) {
                            err = libusb_release_interface(libusb_device_handler->native_handle, intf_i);
                            if (err) {
                                SPDLOG_ERROR("释放设备接口时出错: {}", libusb_strerror(err));
                            }
                            // 重新让内核驱动接管
                            err = libusb_attach_kernel_driver(libusb_device_handler->native_handle, intf_i);
                            if (err && err != LIBUSB_ERROR_NOT_FOUND && err != LIBUSB_ERROR_NOT_SUPPORTED) {
                                SPDLOG_WARN("重新绑定内核驱动失败: {}", libusb_strerror(err));
                            }
                        }
                        libusb_free_config_descriptor(active_config_desc);
                    }
                    libusb_close(libusb_device_handler->native_handle);
                }
                else {
                    SPDLOG_ERROR("无法获取device的handle");
                }
            }
        }
        server_using_devices.clear();
    }

# ifndef USBIPDCPP_ENABLE_BUSY_WAIT
    should_exit_libusb_event_thread = true;
    libusb_interrupt_event_handler(nullptr);
    spdlog::info("等待libusb事件线程结束");
    libusb_event_thread.join();
    spdlog::info("libusb事件线程结束");
# endif
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
