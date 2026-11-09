#include "client_common.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace usbipdcpp::e2e {

namespace {

void transfer_callback(libusb_transfer *transfer) {
    auto *result = static_cast<AsyncResult *>(transfer->user_data);
    result->status = transfer->status;
    result->actual_length = transfer->actual_length;
    result->done = true;
}

}  // namespace

Args parse_args(int argc, char **argv, std::uint16_t default_vid, std::uint16_t default_pid) {
    Args args;
    args.vid = default_vid;
    args.pid = default_pid;

    for (int i = 1; i < argc; ++i) {
        const char *option = argv[i];
        const bool has_value = i + 1 < argc;
        if (std::strcmp(option, "--bus") == 0 && has_value) {
            args.bus = static_cast<int>(std::strtol(argv[++i], nullptr, 10));
        }
        else if (std::strcmp(option, "--addr") == 0 && has_value) {
            args.address = static_cast<int>(std::strtol(argv[++i], nullptr, 10));
        }
        else if (std::strcmp(option, "--vid") == 0 && has_value) {
            args.vid = static_cast<std::uint16_t>(std::strtoul(argv[++i], nullptr, 0));
        }
        else if (std::strcmp(option, "--pid") == 0 && has_value) {
            args.pid = static_cast<std::uint16_t>(std::strtoul(argv[++i], nullptr, 0));
        }
        else if (std::strcmp(option, "--help") == 0 || std::strcmp(option, "-h") == 0) {
            std::printf("用法: %s [--bus N] [--addr N] [--vid 0xVID] [--pid 0xPID]\n", argv[0]);
            std::exit(0);
        }
        else {
            std::fprintf(stderr, "忽略未知参数: %s\n", option);
        }
    }
    return args;
}

std::string libusb_error_string(int code) {
    const char *name = libusb_error_name(code);
    return name != nullptr ? name : "unknown";
}

std::string hex_byte(std::uint8_t address) {
    char buffer[8];
    std::snprintf(buffer, sizeof(buffer), "0x%02x", static_cast<unsigned>(address));
    return buffer;
}

int open_by_address(libusb_context *context, const Args &args, libusb_device_handle **handle) {
    libusb_device **devices = nullptr;
    const ssize_t count = libusb_get_device_list(context, &devices);
    if (count < 0) {
        std::fprintf(stderr, "枚举 USB 设备失败: %s\n",
                     libusb_error_string(static_cast<int>(count)).c_str());
        return 1;
    }

    libusb_device *target = nullptr;
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device_descriptor descriptor {};
        if (libusb_get_device_descriptor(devices[i], &descriptor) != 0) {
            continue;
        }
        if (descriptor.idVendor != args.vid || descriptor.idProduct != args.pid) {
            continue;
        }
        const int bus = libusb_get_bus_number(devices[i]);
        const int address = libusb_get_device_address(devices[i]);
        if (args.bus >= 0 && bus != args.bus) {
            continue;
        }
        if (args.address >= 0 && address != args.address) {
            continue;
        }
        target = devices[i];
        break;
    }

    if (target == nullptr) {
        std::fprintf(stderr, "找不到设备 %04x:%04x", args.vid, args.pid);
        if (args.bus >= 0) {
            std::fprintf(stderr, "（总线 %d 地址 %d）", args.bus, args.address);
        }
        std::fprintf(stderr, "\n当前匹配 VID/PID 的设备：\n");
        for (ssize_t i = 0; i < count; ++i) {
            libusb_device_descriptor descriptor {};
            if (libusb_get_device_descriptor(devices[i], &descriptor) != 0) {
                continue;
            }
            if (descriptor.idVendor == args.vid && descriptor.idProduct == args.pid) {
                std::fprintf(stderr, "  总线 %d 地址 %d\n", libusb_get_bus_number(devices[i]),
                             libusb_get_device_address(devices[i]));
            }
        }
        libusb_free_device_list(devices, 1);
        return 1;
    }

    const int result = libusb_open(target, handle);
    libusb_free_device_list(devices, 1);
    if (result != 0) {
        std::fprintf(stderr, "打开设备失败: %s\n", libusb_error_string(result).c_str());
        return 1;
    }
    return 0;
}

void TestRunner::check(bool ok, const std::string &name, const std::string &detail) {
    if (ok) {
        ++passed;
        std::printf("[PASS] %s\n", name.c_str());
    }
    else {
        ++failed;
        if (detail.empty()) {
            std::printf("[FAIL] %s\n", name.c_str());
        }
        else {
            std::printf("[FAIL] %s (%s)\n", name.c_str(), detail.c_str());
        }
    }
    std::fflush(stdout);
}

int TestRunner::summary() const {
    std::printf("==== %d passed, %d failed ====\n", passed, failed);
    std::fflush(stdout);
    return failed == 0 ? 0 : 1;
}

void fill_increment(std::vector<std::uint8_t> &buffer, std::uint8_t seed) {
    for (std::size_t i = 0; i < buffer.size(); ++i) {
        buffer[i] = static_cast<std::uint8_t>(seed + i);
    }
}

bool verify_increment(const std::uint8_t *data, std::size_t length) {
    for (std::size_t i = 1; i < length; ++i) {
        if (data[i] != static_cast<std::uint8_t>(data[i - 1] + 1)) {
            return false;
        }
    }
    return true;
}

std::uint16_t read_le16(const std::uint8_t *data) {
    return static_cast<std::uint16_t>(data[0] | (data[1] << 8));
}

std::uint32_t read_le32(const std::uint8_t *data) {
    return static_cast<std::uint32_t>(data[0]) | (static_cast<std::uint32_t>(data[1]) << 8) |
           (static_cast<std::uint32_t>(data[2]) << 16) |
           (static_cast<std::uint32_t>(data[3]) << 24);
}

bool get_endpoint_addresses(libusb_device_handle *handle, int interface_number,
                            int alternate_setting, std::vector<std::uint8_t> &addresses) {
    addresses.clear();
    libusb_device *device = libusb_get_device(handle);
    libusb_config_descriptor *config = nullptr;
    if (libusb_get_active_config_descriptor(device, &config) != 0) {
        return false;
    }
    bool ok = false;
    for (int i = 0; i < config->bNumInterfaces && !ok; ++i) {
        const libusb_interface &interface = config->interface[i];
        for (int j = 0; j < interface.num_altsetting; ++j) {
            const libusb_interface_descriptor &descriptor = interface.altsetting[j];
            if (descriptor.bInterfaceNumber != interface_number ||
                descriptor.bAlternateSetting != alternate_setting) {
                continue;
            }
            for (int k = 0; k < descriptor.bNumEndpoints; ++k) {
                addresses.push_back(descriptor.endpoint[k].bEndpointAddress);
            }
            ok = true;
            break;
        }
    }
    libusb_free_config_descriptor(config);
    return ok;
}

bool check_endpoint_descriptors(libusb_device_handle *handle, int interface_number,
                                int alternate_setting, int expected_type,
                                std::uint16_t expected_max_packet, std::string &detail) {
    detail.clear();
    libusb_device *device = libusb_get_device(handle);
    libusb_config_descriptor *config = nullptr;
    if (libusb_get_active_config_descriptor(device, &config) != 0) {
        detail = "读取配置描述符失败";
        return false;
    }
    bool found = false;
    bool ok = false;
    for (int i = 0; i < config->bNumInterfaces && !found; ++i) {
        const libusb_interface &interface = config->interface[i];
        for (int j = 0; j < interface.num_altsetting; ++j) {
            const libusb_interface_descriptor &descriptor = interface.altsetting[j];
            if (descriptor.bInterfaceNumber != interface_number ||
                descriptor.bAlternateSetting != alternate_setting) {
                continue;
            }
            found = true;
            ok = true;
            for (int k = 0; k < descriptor.bNumEndpoints; ++k) {
                const libusb_endpoint_descriptor &endpoint = descriptor.endpoint[k];
                const int type = endpoint.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
                const auto max_packet =
                        static_cast<std::uint16_t>(endpoint.wMaxPacketSize & 0x7ff);
                if (type != expected_type || max_packet != expected_max_packet) {
                    char buffer[128];
                    std::snprintf(buffer, sizeof(buffer),
                                  "端点0x%02x type=%d maxpacket=%u（期望 type=%d maxpacket=%u）",
                                  endpoint.bEndpointAddress, type, max_packet, expected_type,
                                  expected_max_packet);
                    detail = buffer;
                    ok = false;
                }
            }
            break;
        }
    }
    if (!found) {
        detail = "接口/alt setting 不存在";
    }
    libusb_free_config_descriptor(config);
    return ok;
}

libusb_transfer *submit_async(libusb_device_handle *handle, std::uint8_t endpoint,
                              std::size_t length, int timeout, bool interrupt,
                              AsyncResult *result) {
    if (result->data.size() != length) {
        result->data.resize(length);
    }
    result->done = false;
    result->status = LIBUSB_TRANSFER_ERROR;
    result->actual_length = 0;

    libusb_transfer *transfer = libusb_alloc_transfer(0);
    if (transfer == nullptr) {
        return nullptr;
    }
    if (interrupt) {
        libusb_fill_interrupt_transfer(transfer, handle, endpoint, result->data.data(),
                                       static_cast<int>(length), transfer_callback, result,
                                       timeout);
    }
    else {
        libusb_fill_bulk_transfer(transfer, handle, endpoint, result->data.data(),
                                  static_cast<int>(length), transfer_callback, result, timeout);
    }
    if (libusb_submit_transfer(transfer) != 0) {
        libusb_free_transfer(transfer);
        return nullptr;
    }
    return transfer;
}

libusb_transfer *submit_async_control(libusb_device_handle *handle, std::uint8_t request_type,
                                      std::uint8_t request, std::uint16_t value,
                                      std::uint16_t index, std::size_t length, int timeout,
                                      AsyncResult *result) {
    result->setup.assign(8, 0);
    result->setup[0] = request_type;
    result->setup[1] = request;
    std::memcpy(result->setup.data() + 2, &value, sizeof(value));
    std::memcpy(result->setup.data() + 4, &index, sizeof(index));
    const auto length16 = static_cast<std::uint16_t>(length);
    std::memcpy(result->setup.data() + 6, &length16, sizeof(length16));

    if (result->data.size() != length) {
        result->data.resize(length);
    }
    result->done = false;
    result->status = LIBUSB_TRANSFER_ERROR;
    result->actual_length = 0;

    libusb_transfer *transfer = libusb_alloc_transfer(0);
    if (transfer == nullptr) {
        return nullptr;
    }
    libusb_fill_control_transfer(transfer, handle, result->setup.data(), transfer_callback, result,
                                 timeout);
    if (libusb_submit_transfer(transfer) != 0) {
        libusb_free_transfer(transfer);
        return nullptr;
    }
    return transfer;
}

bool wait_async(libusb_context *context, AsyncResult &result, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!result.done && std::chrono::steady_clock::now() < deadline) {
        timeval timeout {};
        timeout.tv_sec = 0;
        timeout.tv_usec = 50 * 1000;
        int completed = 0;
        libusb_handle_events_timeout_completed(context, &timeout, &completed);
    }
    return result.done;
}

void cancel_and_free_transfer(libusb_context *context, libusb_transfer *transfer,
                              AsyncResult *result, int timeout_ms) {
    if (transfer == nullptr) {
        return;
    }
    if (result != nullptr && !result->done) {
        // 还在传输中的 transfer 不能直接 free；先取消再等回调落地
        libusb_cancel_transfer(transfer);
        for (int attempt = 0; attempt < 3 && !result->done; ++attempt) {
            wait_async(context, *result, timeout_ms);
        }
        if (!result->done) {
            std::fprintf(stderr, "警告: transfer 始终未完成，跳过释放以避免未定义行为\n");
            return;
        }
    }
    libusb_free_transfer(transfer);
}

}  // namespace usbipdcpp::e2e
