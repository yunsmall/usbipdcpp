#include "usbipdcpp/LibusbHandler/LibusbServer.h"

#include <iostream>

#include <spdlog/spdlog.h>

#include "usbipdcpp/LibusbHandler/LibusbDeviceHandler.h"
#include "usbipdcpp/LibusbHandler/tools.h"

using namespace usbipdcpp;

namespace {
void log_device_state(Server &server) {
    std::lock_guard lock(server.get_devices_mutex());
    auto &available = server.get_available_devices();
    auto &using_devices = server.get_using_devices();

    std::string avail_busids;
    for (auto &d: available) {
        if (!avail_busids.empty())
            avail_busids += ", ";
        avail_busids += d->busid;
    }
    std::string using_busids;
    for (auto &[busid, d]: using_devices) {
        if (!using_busids.empty())
            using_busids += ", ";
        using_busids += busid;
    }

    SPDLOG_INFO("设备状态: 可用 {} 个 [{}] | 使用中 {} 个 [{}]", available.size(), avail_busids, using_devices.size(),
                using_busids);
}
} // namespace

LibusbServer::LibusbServer(const LibusbServerConfig& config) : config(config) {
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

std::pair<std::string, std::string> LibusbServer::get_device_names(libusb_device *device) {
    libusb_device_handle *handle = nullptr;
    libusb_device_descriptor desc{};
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
            libusb_get_string_descriptor_ascii(handle, desc.iProduct, reinterpret_cast<unsigned char *>(product),
                                               sizeof(product));
        }

        libusb_close(handle);
    }

    return {(manufacturer[0] ? manufacturer : "Unknown Manufacturer"), (product[0] ? product : "Unknown Product")};
}

void LibusbServer::print_device(libusb_device *dev, bool skip_name) {
    libusb_device_descriptor desc{};
    // 获取设备描述符
    auto err = libusb_get_device_descriptor(dev, &desc);
    if (err) {
        SPDLOG_ERROR("无法获取设备描述符：{}", libusb_strerror(err));
        return;
    }
    // 打印设备信息
    std::string device_name;
    if (skip_name) {
        device_name = std::format("{:04x}:{:04x}", desc.idVendor, desc.idProduct);
    } else {
        auto [mfr, product] = get_device_names(dev);
        device_name = std::format("{}-{}", mfr, product);
    }
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
        for (auto & server_available_device : server_available_devices) {
            if (server_available_device->busid == busid) {
                is_available = true;
            }
        }
    }
    std::cout << std::format("Device name: {} ({})", device_name,
                             is_used ? "exported" : (is_available ? "available" : "unbinded"))
              << std::endl;
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

void LibusbServer::list_host_devices() {
    libusb_device **devs;
    auto dev_nums = libusb_get_device_list(nullptr, &devs);
    for (auto dev_i = 0; dev_i < dev_nums; dev_i++) {
        if (config.skip_hub) {
            libusb_device_descriptor desc{};
            if (libusb_get_device_descriptor(devs[dev_i], &desc) == 0 &&
                desc.bDeviceClass == LIBUSB_CLASS_HUB)
                continue;
        }
        print_device(devs[dev_i]);
        std::cout << std::endl;
    }
    libusb_free_device_list(devs, 1);
}

libusb_device *LibusbServer::find_by_busid(const std::string &busid) {
    libusb_device **devs;
    int dev_nums = libusb_get_device_list(nullptr, &devs);
    for (auto dev_i = 0; dev_i < dev_nums; dev_i++) {
        if (get_device_busid(devs[dev_i]) == busid) {
            auto ret_dev = libusb_ref_device(devs[dev_i]);
            libusb_free_device_list(devs, 1);
            return ret_dev;
        }
    }
    libusb_free_device_list(devs, 1);
    return nullptr;
}

DeviceOperationResult LibusbServer::bind_host_device(libusb_device *dev) {
    if (dev == nullptr) {
        SPDLOG_ERROR("dev 不能为空");
        return DeviceOperationResult::DeviceNotFound;
    }

    // 获取设备描述符
    libusb_device_descriptor device_descriptor{};
    int err = libusb_get_device_descriptor(dev, &device_descriptor);
    if (err) {
        SPDLOG_WARN("无法获取设备描述符：{}", libusb_strerror(err));
        libusb_unref_device(dev);
        return DeviceOperationResult::GetDescriptorFailed;
    }

    // 跳过 hub 设备（bDeviceClass == 0x09），hub 对 USB/IP 无意义
    if (config.skip_hub && device_descriptor.bDeviceClass == LIBUSB_CLASS_HUB) {
        SPDLOG_DEBUG("跳过 hub 设备: busid={}", get_device_busid(dev));
        libusb_unref_device(dev);
        return DeviceOperationResult::HubFiltered;
    }

    // 获取配置描述符
    struct libusb_config_descriptor *active_config_desc;
    err = libusb_get_active_config_descriptor(dev, &active_config_desc);
    if (err) {
        SPDLOG_WARN("无法获取设备当前的配置描述符：{}", libusb_strerror(err));
        libusb_unref_device(dev);
        return DeviceOperationResult::GetConfigFailed;
    }

    // 构建接口信息
    SPDLOG_DEBUG("该设备有{}个interface", active_config_desc->bNumInterfaces);
    std::vector<UsbInterface> interfaces;
    for (auto intf_i = 0; intf_i < active_config_desc->bNumInterfaces; intf_i++) {
        auto &intf = active_config_desc->interface[intf_i];
        SPDLOG_DEBUG("第{}个interface有{}个altsetting", intf_i, intf.num_altsetting);

        std::vector<std::vector<UsbEndpoint>> endpoints;
        endpoints.reserve(intf.num_altsetting);
        for (auto alt_i = 0; alt_i < intf.num_altsetting; alt_i++) {
            auto &intf_desc = intf.altsetting[alt_i];
            std::vector<UsbEndpoint> alt_endpoints;
            alt_endpoints.reserve(intf_desc.bNumEndpoints);
            for (auto ep_i = 0; ep_i < intf_desc.bNumEndpoints; ep_i++) {
                alt_endpoints.emplace_back(intf_desc.endpoint[ep_i].bEndpointAddress,
                                           intf_desc.endpoint[ep_i].bmAttributes & 0x03,
                                           intf_desc.endpoint[ep_i].wMaxPacketSize, intf_desc.endpoint[ep_i].bInterval);
            }
            endpoints.push_back(std::move(alt_endpoints));
        }
        interfaces.emplace_back(UsbInterface{.interface_class = intf.altsetting[0].bInterfaceClass,
                                             .interface_subclass = intf.altsetting[0].bInterfaceSubClass,
                                             .interface_protocol = intf.altsetting[0].bInterfaceProtocol,
                                             .interface_number = intf.altsetting[0].bInterfaceNumber,
                                             .endpoints = std::move(endpoints)});
    }

    // 创建 UsbDevice 和 LibusbDeviceHandler
    {
        std::lock_guard lock(server.get_devices_mutex());
        // 拒绝重复绑定同一 busid：导入时 try_moving_device_to_using 按
        // busid 匹配，重复项会让同一 busid 对应多个设备、导入结果不确定；
        // 拔出清理（handle_device_left）也只按 busid 移除一个，重复项会残留。
        // 在锁内手写查重（不能调 has_bound_device——它内部取读锁，与这里
        // 的写锁重入未定义行为）
        for (const auto &d: server.get_available_devices()) {
            if (d->busid == get_device_busid(dev)) {
                libusb_unref_device(dev);
                return DeviceOperationResult::DeviceAlreadyBound;
            }
        }
        if (server.get_using_devices().contains(get_device_busid(dev))) {
            libusb_unref_device(dev);
            return DeviceOperationResult::DeviceInUse;
        }
        auto current_device = std::make_shared<UsbDevice>(UsbDevice{
                .path = std::format("/sys/bus/{}/{}/{}", libusb_get_bus_number(dev), libusb_get_device_address(dev),
                                    libusb_get_port_number(dev)),
                .busid = get_device_busid(dev),
                .bus_num = libusb_get_bus_number(dev),
                // devnum 与参考项目 usbipd-libusb 一致填端口号
                // （usbip_host_driver.c: udev->devnum = libusb_get_port_number(dev)）。
                // 该字段仅用于 usbip list 展示，导入匹配走 busid，不影响功能；
                // 内核 usbipd 的 devnum 是设备地址（sysfs devnum 属性），两者
                // 语义不同但均不被客户端依赖
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

        // 普通模式：传入设备引用（handler 持有引用所有权）
        current_device->with_handler<LibusbDeviceHandler>(dev);
        server.get_available_devices().emplace_back(std::move(current_device));
    }

    libusb_free_config_descriptor(active_config_desc);
    SPDLOG_INFO("设备 {} 已添加到可用列表", get_device_busid(dev));
    log_device_state(server);
    return DeviceOperationResult::Success;
}

DeviceOperationResult LibusbServer::bind_host_device_with_wrapped_fd(intptr_t fd) {
    if (fd < 0) {
        SPDLOG_ERROR("fd 无效");
        return DeviceOperationResult::DeviceNotFound;
    }

    // Android 模式：临时 wrap fd 获取设备信息，然后关闭
    libusb_device_handle *temp_handle = nullptr;
    int err = libusb_wrap_sys_device(nullptr, fd, &temp_handle);
    if (err) {
        SPDLOG_ERROR("libusb_wrap_sys_device 失败: {}", libusb_strerror(err));
        return DeviceOperationResult::DeviceOpenFailed;
    }

    // 从临时 handle 获取设备信息
    libusb_device *device_for_info = libusb_get_device(temp_handle);
    if (!device_for_info) {
        SPDLOG_ERROR("libusb_get_device returns nullptr");
        libusb_close(temp_handle);
        return DeviceOperationResult::DeviceNotFound;
    }

    // 获取设备描述符
    libusb_device_descriptor device_descriptor{};
    err = libusb_get_device_descriptor(device_for_info, &device_descriptor);
    if (err) {
        SPDLOG_WARN("无法获取设备描述符：{}", libusb_strerror(err));
        libusb_close(temp_handle);
        return DeviceOperationResult::GetDescriptorFailed;
    }

    // 获取配置描述符
    struct libusb_config_descriptor *active_config_desc;
    err = libusb_get_active_config_descriptor(device_for_info, &active_config_desc);
    if (err) {
        SPDLOG_WARN("无法获取设备当前的配置描述符：{}", libusb_strerror(err));
        libusb_close(temp_handle);
        return DeviceOperationResult::GetConfigFailed;
    }

    // 构建接口信息
    SPDLOG_DEBUG("该设备有{}个interface", active_config_desc->bNumInterfaces);
    std::vector<UsbInterface> interfaces;
    for (auto intf_i = 0; intf_i < active_config_desc->bNumInterfaces; intf_i++) {
        auto &intf = active_config_desc->interface[intf_i];
        SPDLOG_DEBUG("第{}个interface有{}个altsetting", intf_i, intf.num_altsetting);

        std::vector<std::vector<UsbEndpoint>> endpoints;
        endpoints.reserve(intf.num_altsetting);
        for (auto alt_i = 0; alt_i < intf.num_altsetting; alt_i++) {
            auto &intf_desc = intf.altsetting[alt_i];
            std::vector<UsbEndpoint> alt_endpoints;
            alt_endpoints.reserve(intf_desc.bNumEndpoints);
            for (auto ep_i = 0; ep_i < intf_desc.bNumEndpoints; ep_i++) {
                alt_endpoints.emplace_back(intf_desc.endpoint[ep_i].bEndpointAddress,
                                           intf_desc.endpoint[ep_i].bmAttributes & 0x03,
                                           intf_desc.endpoint[ep_i].wMaxPacketSize, intf_desc.endpoint[ep_i].bInterval);
            }
            endpoints.push_back(std::move(alt_endpoints));
        }
        interfaces.emplace_back(UsbInterface{.interface_class = intf.altsetting[0].bInterfaceClass,
                                             .interface_subclass = intf.altsetting[0].bInterfaceSubClass,
                                             .interface_protocol = intf.altsetting[0].bInterfaceProtocol,
                                             .interface_number = intf.altsetting[0].bInterfaceNumber,
                                             .endpoints = std::move(endpoints)});
    }

    // 保存设备信息
    auto busid = get_device_busid(device_for_info);
    auto bus_num = libusb_get_bus_number(device_for_info);
    auto dev_addr = libusb_get_device_address(device_for_info);
    // devnum 填端口号（同普通模式，见 bind_host_device 的注释）
    auto dev_num = libusb_get_port_number(device_for_info);
    auto speed = (std::uint32_t) libusb_speed_to_usb_speed(libusb_get_device_speed(device_for_info));
    auto configuration_value = active_config_desc->bConfigurationValue;

    // 关闭临时 handle
    libusb_free_config_descriptor(active_config_desc);
    libusb_close(temp_handle);

    // 创建 UsbDevice 和 LibusbDeviceHandler
    {
        std::lock_guard lock(server.get_devices_mutex());
        // 拒绝重复绑定同一 busid（同 bind_host_device 的注释：导入按 busid
        // 匹配、拔出按 busid 移除，重复项会导致行为不确定）。锁内手写查重
        for (const auto &d: server.get_available_devices()) {
            if (d->busid == busid) {
                return DeviceOperationResult::DeviceAlreadyBound;
            }
        }
        if (server.get_using_devices().contains(busid)) {
            return DeviceOperationResult::DeviceInUse;
        }
        auto current_device = std::make_shared<UsbDevice>(UsbDevice{
                .path = std::format("/sys/bus/{}/{}/{}", bus_num, dev_addr, dev_num),
                .busid = busid,
                .bus_num = bus_num,
                .dev_num = dev_num,
                .speed = speed,
                .vendor_id = device_descriptor.idVendor,
                .product_id = device_descriptor.idProduct,
                .device_bcd = device_descriptor.bcdDevice,
                .device_class = device_descriptor.bDeviceClass,
                .device_subclass = device_descriptor.bDeviceSubClass,
                .device_protocol = device_descriptor.bDeviceProtocol,
                .configuration_value = configuration_value,
                .num_configurations = device_descriptor.bNumConfigurations,
                .interfaces = std::move(interfaces),
                .ep0_in = UsbEndpoint::get_ep0_in(device_descriptor.bMaxPacketSize0),
                .ep0_out = UsbEndpoint::get_ep0_out(device_descriptor.bMaxPacketSize0),
        });

        // Android 模式：传入 fd（每次连接时重新 wrap）
        current_device->with_handler<LibusbDeviceHandler>(fd);
        server.get_available_devices().emplace_back(std::move(current_device));
    }

    SPDLOG_INFO("设备 {} 已添加到可用列表 (fd={})", busid, fd);
    log_device_state(server);
    return DeviceOperationResult::Success;
}

DeviceOperationResult LibusbServer::unbind_host_device(libusb_device *device) {
    auto target_busid = get_device_busid(device);
    auto result = DeviceOperationResult::DeviceNotFound;
    {
        std::lock_guard lock(server.get_devices_mutex());
        auto &server_using_devices = server.get_using_devices();
        auto &server_available_devices = server.get_available_devices();
        for (auto i = server_available_devices.begin(); i != server_available_devices.end(); ++i) {
            if ((*i)->busid == target_busid) {
                auto libusb_device_handler = std::dynamic_pointer_cast<LibusbDeviceHandler>((*i)->handler);
                if (libusb_device_handler) {
                    // 如果接口已声明，需要释放
                    if (libusb_device_handler->interfaces_claimed_) {
                        libusb_device_handler->release_and_close_device();
                    }
                    // 释放设备引用
                    if (libusb_device_handler->native_device_) {
                        libusb_unref_device(libusb_device_handler->native_device_);
                        libusb_device_handler->native_device_ = nullptr;
                    }
                }
                server_available_devices.erase(i);
                libusb_unref_device(device);
                spdlog::info("成功取消绑定");
                result = DeviceOperationResult::Success;
                break;
            }
        }
        if (result == DeviceOperationResult::DeviceNotFound) {
            SPDLOG_WARN("可使用的设备中无目标设备");

            if (server_using_devices.contains(target_busid)) {
                SPDLOG_WARN("正在使用的设备不支持解绑");
                libusb_unref_device(device);
                result = DeviceOperationResult::DeviceInUse;
            }
            else {
                libusb_unref_device(device);
            }
        }
    }
    log_device_state(server);
    return result;
}

DeviceOperationResult LibusbServer::unbind_host_device_by_fd(intptr_t fd) {
    auto result = DeviceOperationResult::DeviceNotFound;
    {
        std::lock_guard lock(server.get_devices_mutex());
        auto &server_using_devices = server.get_using_devices();
        auto &server_available_devices = server.get_available_devices();

        // 先检查 available_devices
        for (auto i = server_available_devices.begin(); i != server_available_devices.end(); ++i) {
            if (auto libusb_device_handler = std::dynamic_pointer_cast<LibusbDeviceHandler>((*i)->handler)) {
                if (libusb_device_handler->wrapped_fd_ == fd) {
                    // 如果接口已声明，需要释放
                    if (libusb_device_handler->interfaces_claimed_) {
                        libusb_device_handler->release_and_close_device();
                    }
                    // Android 模式没有 native_device_，无需释放

                    auto busid = (*i)->busid;
                    server_available_devices.erase(i);
                    SPDLOG_INFO("成功取消绑定设备 {} (fd={})", busid, fd);
                    result = DeviceOperationResult::Success;
                    break;
                }
            }
        }

        // 再检查 using_devices（设备正在使用中）
        if (result != DeviceOperationResult::Success) {
            for (auto it = server_using_devices.begin(); it != server_using_devices.end(); ++it) {
                if (auto libusb_device_handler = std::dynamic_pointer_cast<LibusbDeviceHandler>(it->second->handler)) {
                    if (libusb_device_handler->wrapped_fd_ == fd) {
                        SPDLOG_WARN("设备正在使用中，不支持解绑 (fd={})", fd);
                        result = DeviceOperationResult::DeviceInUse;
                        break;
                    }
                }
            }
        }

        if (result == DeviceOperationResult::DeviceNotFound) {
            SPDLOG_WARN("未找到 fd={} 的设备", fd);
        }
    }
    log_device_state(server);
    return result;
}

DeviceOperationResult LibusbServer::try_remove_dead_device(const std::string &busid) {
    std::lock_guard lock(server.get_devices_mutex());
    auto &server_using_devices = server.get_using_devices();
    auto &server_available_devices = server.get_available_devices();
    for (auto i = server_available_devices.begin(); i != server_available_devices.end(); ++i) {
        if ((*i)->busid == busid) {
            if (auto libusb_device_handler = std::dynamic_pointer_cast<LibusbDeviceHandler>((*i)->handler)) {
                // 设备可能未打开
                if (libusb_device_handler->native_handle) {
                    libusb_close(libusb_device_handler->native_handle);
                    libusb_device_handler->native_handle = nullptr;
                }
                // 释放设备引用
                if (libusb_device_handler->native_device_) {
                    libusb_unref_device(libusb_device_handler->native_device_);
                    libusb_device_handler->native_device_ = nullptr;
                }
                server_available_devices.erase(i);
                spdlog::info("删除可用设备中的{}", busid);
                return DeviceOperationResult::Success;
            }
        }
    }
    if (auto device = server_using_devices.find(busid); device != server_using_devices.end()) {
        // 设备正在使用中：不能直接 close handle / unref device——挂起的
        // libusb 传输回调持有 handler 裸指针，erase 后 handler 引用计数归零
        // 即析构，回调再访问 handler 是 use-after-free；且 session 对设备
        // 拔出毫无感知。与 handle_device_left 的拔出路径一致：置
        // device_removed 并触发会话停止，由 session 收尾（on_disconnection）
        // 完成取消传输、等待回调、释放接口的完整清理，随后设备按
        // is_device_removed() 从 using 列表移除，不会移回可用列表
        if (std::dynamic_pointer_cast<LibusbDeviceHandler>(device->second->handler)) {
            device->second->handler->on_device_removed();
            SPDLOG_WARN("正在使用的设备被拔出，强制关闭 Session: {}", busid);
            device->second->handler->trigger_session_stop();
            return DeviceOperationResult::Success;
        }
    }
    SPDLOG_WARN("无法找到busid为{}的设备", busid);
    return DeviceOperationResult::DeviceNotFound;
}

// 预留 API：当前内部无调用点（热插拔拔出逻辑由 handle_device_left 承担，
// 逻辑与之基本相同），保留供外部使用者按需调用
DeviceOperationResult LibusbServer::notify_device_removed(const std::string &busid) {
    SPDLOG_INFO("设备被拔出: {}", busid);

    std::lock_guard lock(server.get_devices_mutex());
    auto &available_devices = server.get_available_devices();
    auto &using_devices = server.get_using_devices();

    // 1. 从 available_devices 中移除
    for (auto it = available_devices.begin(); it != available_devices.end(); ++it) {
        if ((*it)->busid == busid) {
            if (auto handler = std::dynamic_pointer_cast<LibusbDeviceHandler>((*it)->handler)) {
                if (handler->native_handle) {
                    libusb_close(handler->native_handle);
                    handler->native_handle = nullptr;
                }
                if (handler->native_device_) {
                    libusb_unref_device(handler->native_device_);
                    handler->native_device_ = nullptr;
                }
            }
            available_devices.erase(it);
            SPDLOG_INFO("已从已绑定设备列表移除: {}", busid);
            return DeviceOperationResult::Success;
        }
    }

    // 2. 如果正在使用中，触发断连
    if (auto it = using_devices.find(busid); it != using_devices.end()) {
        if (auto handler = it->second->handler) {
            handler->on_device_removed();
            SPDLOG_WARN("正在使用的设备被拔出，强制关闭 Session: {}", busid);
            handler->trigger_session_stop();
        }
        return DeviceOperationResult::Success;
    }

    SPDLOG_WARN("未找到设备: {}", busid);
    return DeviceOperationResult::DeviceNotFound;
}

void LibusbServer::start_hotplug_monitor() {
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
            static_cast<libusb_hotplug_event>(LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED | LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT),
            LIBUSB_HOTPLUG_NO_FLAGS, LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY, LIBUSB_HOTPLUG_MATCH_ANY,
            hotplug_callback, this, &hotplug_handle_);

    if (ret == 0) {
        hotplug_enabled_ = true;
        SPDLOG_INFO("热插拔监控已启动");
    }
    else {
        SPDLOG_ERROR("注册热插拔回调失败: {}", libusb_strerror(ret));
    }
}

void LibusbServer::stop_hotplug_monitor() {
    if (hotplug_enabled_) {
        libusb_hotplug_deregister_callback(nullptr, hotplug_handle_);
        hotplug_enabled_ = false;
        SPDLOG_INFO("热插拔监控已停止");
    }
}

int LIBUSB_CALL LibusbServer::hotplug_callback(libusb_context *ctx, libusb_device *device, libusb_hotplug_event event,
                                               void *user_data) {
    auto *server = static_cast<LibusbServer *>(user_data);

    if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED) {
        server->handle_device_arrived(device);
    }
    else if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT) {
        auto busid = get_device_busid(device);
        server->handle_device_left(busid);
    }

    return 0;
}

void LibusbServer::handle_device_arrived(libusb_device *device) {
    auto busid = get_device_busid(device);

    // 检查是否已绑定
    {
        std::shared_lock lock(server.get_devices_mutex());
        for (const auto &dev: server.get_available_devices()) {
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

    // 打印设备信息（热插拔回调运行在 libusb 事件线程，必须始终跳过设备名
    // 查询——get_device_names 会 libusb_open，可能阻塞事件线程的事件处理；
    // 显示 VID:PID 已足够。设备名查询只在主线程的 list_host_devices 使用）
    SPDLOG_INFO("检测到新设备插入:");
    print_device(device, true);

    // 自动绑定的 bind_host_device 会调 libusb_get_active_config_descriptor：
    // Linux 主平台（usbfs 后端）该调用是同步 ioctl，不经过 libusb 事件
    // 循环，在热插拔回调中调用安全；darwin（macOS）等同步 I/O 走事件循环
    // 的后端理论上有嵌套事件处理风险，但本项目无该平台的真设备运行场景。
    // 不把绑定投递到工作线程：引入队列/线程/生命周期管理复杂度，嵌入式
    // 平台实现困难，而主平台行为已被验证安全
    if (config.auto_bind_hotplug) {
        SPDLOG_INFO("自动绑定新设备: {}", busid);
        libusb_ref_device(device); // bind_host_device 会接管引用
        auto result = bind_host_device(device);
        if (result == DeviceOperationResult::Success) {
            SPDLOG_INFO("自动绑定成功: {}", busid);
        } else if (result == DeviceOperationResult::HubFiltered) {
            SPDLOG_DEBUG("新设备 {} 为 hub，跳过", busid);
        } else {
            SPDLOG_WARN("自动绑定失败: {}", busid);
        }
    }
}

void LibusbServer::handle_device_left(const std::string &busid) {
    SPDLOG_INFO("检测到设备拔出: {}", busid);

    std::lock_guard lock(server.get_devices_mutex());
    auto &available_devices = server.get_available_devices();
    auto &using_devices = server.get_using_devices();

    // 1. 从 available_devices 中移除
    for (auto it = available_devices.begin(); it != available_devices.end(); ++it) {
        if ((*it)->busid == busid) {
            if (auto handler = std::dynamic_pointer_cast<LibusbDeviceHandler>((*it)->handler)) {
                // 设备可能未打开
                if (handler->native_handle) {
                    libusb_close(handler->native_handle);
                    handler->native_handle = nullptr;
                }
                if (handler->native_device_) {
                    libusb_unref_device(handler->native_device_);
                    handler->native_device_ = nullptr;
                }
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

void LibusbServer::bind_existing_devices() {
    libusb_device **devs = nullptr;
    ssize_t dev_nums = libusb_get_device_list(nullptr, &devs);
    if (dev_nums < 0) {
        SPDLOG_ERROR("libusb_get_device_list 失败: {}", libusb_strerror(static_cast<int>(dev_nums)));
        return;
    }
    int bound = 0, skipped = 0;
    for (ssize_t i = 0; i < dev_nums; i++) {
        auto busid = get_device_busid(devs[i]);
        // 跳过已绑定或在用的设备
        {
            std::shared_lock lock(server.get_devices_mutex());
            bool already_known = server.get_using_devices().contains(busid);
            if (!already_known) {
                for (const auto &d : server.get_available_devices()) {
                    if (d->busid == busid) { already_known = true; break; }
                }
            }
            if (already_known) continue;
        }
        libusb_ref_device(devs[i]); // bind_host_device 接管引用
        auto result = bind_host_device(devs[i]);
        switch (result) {
            case DeviceOperationResult::Success: bound++; break;
            case DeviceOperationResult::HubFiltered: skipped++; break;
            default: break;
        }
    }
    // 第二个参数传 1：列表本身为每个设备持有一个引用，必须释放。
    // bind 成功的设备另有 libusb_ref_device 的引用由 handler 接管（析构时 unref），互不冲突。
    // 传 0 会导致每个设备泄漏一个引用，libusb_device 永不销毁
    libusb_free_device_list(devs, 1);
    SPDLOG_INFO("扫描绑定完成: {} 个绑定, {} 个 hub 跳过", bound, skipped);
}

usbipdcpp::error_code LibusbServer::start(asio::ip::tcp::endpoint &ep) {
    start_hotplug_monitor();

    should_exit_libusb_event_thread = false;

    // 线程创建失败（系统资源不足）时 std::thread 构造抛异常，start 承诺
    // 不抛异常（error_code 报告错误），捕获后清理已启动的热插拔监控并返回
    // 错误码（与 Server::start 的线程创建失败处理一致）
    try {
        libusb_event_thread = std::thread([this]() {
            try {
                SPDLOG_INFO("启动一个libusb device handle的libusb事件循环线程");
                while (!should_exit_libusb_event_thread) {
                    auto ret = libusb_handle_events(nullptr);

                    if (ret == LIBUSB_ERROR_INTERRUPTED && should_exit_libusb_event_thread) [[unlikely]] {
                        SPDLOG_INFO("libusb事件循环收到中断信号正常退出");
                        break;
                    }
                    if (ret < 0 && ret != LIBUSB_ERROR_INTERRUPTED) [[unlikely]] {
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
    } catch (...) {
        stop_hotplug_monitor();
        SPDLOG_ERROR("libusb 事件线程创建失败");
        return std::make_error_code(std::errc::resource_unavailable_try_again);
    }
    // Server::start 不抛异常，错误通过返回值报告（便于无异常环境的嵌入式平台）
    auto ec = server.start(ep);
    if (ec) [[unlikely]] {
        // start 失败（如端口被占）时调用方按失败处理、不再调 stop()，这里必须
        // 回收已启动的资源：注销热插拔监控并停掉 libusb 事件线程（join），
        // 否则线程仍 joinable，LibusbServer 析构（空析构）会触发 std::terminate
        stop_hotplug_monitor();
        should_exit_libusb_event_thread = true;
        libusb_interrupt_event_handler(nullptr);
        if (libusb_event_thread.joinable()) {
            libusb_event_thread.join();
        }
    }
    return ec;
}

void LibusbServer::stop() {
    stop_hotplug_monitor();
    server.stop();
    SPDLOG_INFO("usbip服务器关闭");

    {
        std::lock_guard lock(server.get_devices_mutex());
        auto &server_using_devices = server.get_using_devices();
        auto &server_available_devices = server.get_available_devices();
        for (auto i = server_available_devices.begin(); i != server_available_devices.end(); ++i) {
            if (auto libusb_device_handler = std::dynamic_pointer_cast<LibusbDeviceHandler>((*i)->handler)) {
                if (libusb_device_handler->interfaces_claimed_) {
                    libusb_device_handler->release_and_close_device();
                }
                if (libusb_device_handler->native_device_) {
                    libusb_unref_device(libusb_device_handler->native_device_);
                    libusb_device_handler->native_device_ = nullptr;
                }
            }
        }
        server_available_devices.clear();

        for (auto using_dev_i = server_using_devices.begin(); using_dev_i != server_using_devices.end();
             ++using_dev_i) {
            if (auto libusb_device_handler =
                        std::dynamic_pointer_cast<LibusbDeviceHandler>(using_dev_i->second->handler)) {
                if (libusb_device_handler->interfaces_claimed_) {
                    libusb_device_handler->release_and_close_device();
                }
                if (libusb_device_handler->native_device_) {
                    libusb_unref_device(libusb_device_handler->native_device_);
                    libusb_device_handler->native_device_ = nullptr;
                }
            }
        }
        server_using_devices.clear();
    }

    should_exit_libusb_event_thread = true;
    libusb_interrupt_event_handler(nullptr);
    spdlog::info("等待libusb事件线程结束");
    // 防御：start 失败（内部已 join）或从未 start 时线程不可 join，
    // 直接 join 会 std::terminate
    if (libusb_event_thread.joinable()) {
        libusb_event_thread.join();
    }
    spdlog::info("libusb事件线程结束");
}

// void usbipcpp::LibusbServer::add_device(std::shared_ptr<UsbDevice> &&device) {
//     Server::add_device(std::move(device));
// }
//
// bool usbipcpp::LibusbServer::remove_device(const std::string &busid) {
//     return Server::remove_device(busid);
// }

LibusbServer::~LibusbServer() {
}
