#include "client_common.hpp"
#include "protocol.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <libusb-1.0/libusb.h>
#include <linux/usb/ch9.h>

namespace {

using namespace usbipdcpp::e2e;

int vendor_in(libusb_device_handle *handle, proto::Request request, std::uint16_t value,
              std::uint16_t index, std::vector<std::uint8_t> &data, int timeout) {
    return libusb_control_transfer(handle, proto::request_type_in,
                                   static_cast<std::uint8_t>(request), value, index,
                                   data.empty() ? nullptr : data.data(),
                                   static_cast<std::uint16_t>(data.size()), timeout);
}

int vendor_out(libusb_device_handle *handle, proto::Request request, std::uint16_t value,
               std::uint16_t index, std::vector<std::uint8_t> data, int timeout) {
    return libusb_control_transfer(handle, proto::request_type_out,
                                   static_cast<std::uint8_t>(request), value, index,
                                   data.empty() ? nullptr : data.data(),
                                   static_cast<std::uint16_t>(data.size()), timeout);
}

std::string length_name(const char *prefix, std::size_t length) {
    return std::string(prefix) + std::to_string(length);
}

void run_tests(libusb_context *context, libusb_device_handle *handle, TestRunner &tests) {
    // ---- 厂商请求：版本 ----
    {
        std::vector<std::uint8_t> data(4);
        const int result = vendor_in(handle, proto::Request::get_info, 0, 0, data, 1000);
        tests.check(result == 4 && read_le32(data.data()) == proto::version, "vendor get_info",
                    "rc=" + libusb_error_string(result));
    }

    // ---- 厂商请求：控制 IN 各长度 ----
    // 4096 是 Linux usbfs/libusb 的控制传输数据上限（linux_usbfs.c 的
    // MAX_CTRL_BUFFER_LENGTH），再大会在客户端 libusb 里直接返回 INVALID_PARAM
    for (const std::size_t length :
         {std::size_t {0}, std::size_t {1}, std::size_t {63}, std::size_t {64}, std::size_t {65},
          std::size_t {512}, std::size_t {4096}}) {
        std::vector<std::uint8_t> data(length);
        const int result = vendor_in(handle, proto::Request::echo_in, 0x22, 0, data, 10000);
        bool ok = result == static_cast<int>(length);
        if (ok && length > 0) {
            ok = data[0] == 0x22 && verify_increment(data.data(), length);
        }
        tests.check(ok, length_name("vendor echo_in len ", length),
                    "rc=" + libusb_error_string(result));
    }

    // ---- 厂商请求：控制 OUT 各长度，并回读长度/校验和 ----
    for (const std::size_t length :
         {std::size_t {0}, std::size_t {1}, std::size_t {64}, std::size_t {65}, std::size_t {512},
          std::size_t {4096}}) {
        std::vector<std::uint8_t> data(length);
        fill_increment(data, 0x30);
        std::uint32_t expected_sum = 0;
        for (const std::uint8_t value : data) {
            expected_sum += value;
        }

        const int result = vendor_out(handle, proto::Request::echo_out, 0, 0, data, 10000);
        std::vector<std::uint8_t> stats(8);
        const int stats_result =
                vendor_in(handle, proto::Request::get_last_out, 0, 0, stats, 1000);
        const bool ok = result == static_cast<int>(length) && stats_result == 8 &&
                        read_le32(stats.data()) == length &&
                        read_le32(stats.data() + 4) == expected_sum;
        tests.check(ok, length_name("vendor echo_out len ", length),
                    "rc=" + libusb_error_string(result));
    }

    // ---- 短包：设备只回 100 字节 ----
    {
        std::vector<std::uint8_t> data(4096);
        const int result = vendor_in(handle, proto::Request::short_in, 100, 0, data, 2000);
        const bool ok = result == 100 && data[0] == 0x55 && verify_increment(data.data(), 100);
        tests.check(ok, "vendor short_in 100/4096", "rc=" + libusb_error_string(result));
    }

    // ---- 零长数据阶段 ----
    {
        std::vector<std::uint8_t> data(4096);
        const int result = vendor_in(handle, proto::Request::zero_in, 0, 0, data, 2000);
        tests.check(result == 0, "vendor zero_in 4096", "rc=" + libusb_error_string(result));
    }

    // ---- stall ----
    {
        std::vector<std::uint8_t> data(4);
        const int result = vendor_in(handle, proto::Request::stall, 0, 0, data, 2000);
        tests.check(result == LIBUSB_ERROR_PIPE, "vendor stall",
                    "rc=" + libusb_error_string(result));
    }
    {
        std::vector<std::uint8_t> data(16);
        fill_increment(data, 0);
        const int result = vendor_out(handle, proto::Request::stall_out_data, 0, 0, data, 2000);
        tests.check(result == LIBUSB_ERROR_PIPE, "vendor stall_out_data",
                    "rc=" + libusb_error_string(result));
    }
    {
        std::vector<std::uint8_t> data(4);
        const int result = libusb_control_transfer(handle, proto::request_type_in, 0x7f, 0, 0,
                                                   data.data(), 4, 1000);
        tests.check(result == LIBUSB_ERROR_PIPE, "vendor unknown request",
                    "rc=" + libusb_error_string(result));
    }

    // ---- 非法接口号：接口 recipient 的请求应被 FunctionFS 拒绝并 stall ----
    {
        std::vector<std::uint8_t> data(4);
        const int result = libusb_control_transfer(handle, 0xc1,
                                                   static_cast<std::uint8_t>(
                                                           proto::Request::get_info),
                                                   0, 5, data.data(), 4, 1000);
        tests.check(result == LIBUSB_ERROR_PIPE, "vendor invalid interface index",
                    "rc=" + libusb_error_string(result));
    }

    // ---- 延时响应与超时 ----
    {
        std::vector<std::uint8_t> data(1);
        const int result = vendor_in(handle, proto::Request::delay_in, 50, 0, data, 2000);
        tests.check(result == 1 && data[0] == 0xaa, "vendor delay_in 50ms",
                    "rc=" + libusb_error_string(result));
    }
    {
        std::vector<std::uint8_t> data(1);
        const int result = vendor_in(handle, proto::Request::delay_in, 500, 0, data, 100);
        tests.check(result == LIBUSB_ERROR_TIMEOUT, "vendor delay_in timeout",
                    "rc=" + libusb_error_string(result));
    }
    {
        std::vector<std::uint8_t> data(4);
        const int result = vendor_in(handle, proto::Request::get_info, 0, 0, data, 1000);
        tests.check(result == 4, "vendor recovery after timeout",
                    "rc=" + libusb_error_string(result));
    }

    // ---- 异步控制传输 + 取消 ----
    {
        AsyncResult async;
        libusb_transfer *transfer = submit_async_control(
                handle, proto::request_type_in, static_cast<std::uint8_t>(proto::Request::delay_in),
                500, 0, 1, 5000, &async);
        if (transfer == nullptr) {
            tests.check(false, "control async cancel submit", "submit failed");
        }
        else {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            libusb_cancel_transfer(transfer);
            const bool completed = wait_async(context, async, 2000);
            tests.check(completed && async.status == LIBUSB_TRANSFER_CANCELLED,
                        "control async cancel", "completed=" + std::to_string(completed));
            cancel_and_free_transfer(context, transfer, &async, 1000);
        }
    }

    // ---- 标准请求：描述符 ----
    {
        std::vector<std::uint8_t> data(18);
        const int result = libusb_get_descriptor(handle, LIBUSB_DT_DEVICE, 0, data.data(), 18);
        tests.check(result == 18 && data[1] == LIBUSB_DT_DEVICE && data[0] == 18,
                    "standard get device descriptor", "rc=" + libusb_error_string(result));
    }
    {
        std::vector<std::uint8_t> data(8);
        const int result = libusb_control_transfer(handle, 0x80, USB_REQ_GET_DESCRIPTOR,
                                                   (USB_DT_DEVICE << 8) | 0, 0, data.data(), 8,
                                                   1000);
        tests.check(result == 8 && data[0] == 18 && data[1] == LIBUSB_DT_DEVICE,
                    "standard get device descriptor truncated",
                    "rc=" + libusb_error_string(result));
    }
    {
        std::vector<std::uint8_t> header(9);
        int result = libusb_control_transfer(handle, 0x80, USB_REQ_GET_DESCRIPTOR,
                                             (USB_DT_CONFIG << 8) | 0, 0, header.data(), 9, 1000);
        const std::uint16_t total = result == 9 ? read_le16(header.data() + 2) : 0;
        std::vector<std::uint8_t> full(total);
        const int full_result =
                total > 0 ? libusb_control_transfer(handle, 0x80, USB_REQ_GET_DESCRIPTOR,
                                                    (USB_DT_CONFIG << 8) | 0, 0, full.data(), total,
                                                    1000)
                          : -1;
        // 除了 bNumInterfaces，配置描述符里的 bConfigurationValue（c.1 → 1）、
        // bmAttributes（configfs 设的 0xc0，bit7 必须为 1）、bMaxPower 也逐字段
        // 校验：这些字段被服务端缓存/tweak 写坏时，只看 bNumInterfaces 发现不了。
        // configfs 的 MaxPower 单位是 mA（脚本设 0x32 = 50mA），描述符 bMaxPower
        // 单位是 2mA，所以这里期望 25（0x19）而不是 0x32
        const bool header_ok = result == 9 && total >= 9;
        const bool config_fields_ok = header_ok && full[4] == 1 && full[5] == 1 &&
                                      (full[7] & 0x80) != 0 && full[8] == 25;
        tests.check(full_result == total && config_fields_ok, "standard get config descriptor",
                    "header=" + std::to_string(result) + " full=" + std::to_string(full_result) +
                            " bNumInterfaces=" + (header_ok ? std::to_string(full[4]) : "?") +
                            " bConfigurationValue=" + (header_ok ? std::to_string(full[5]) : "?") +
                            " bmAttributes=" + (header_ok ? std::to_string(full[7]) : "?") +
                            " bMaxPower=" + (header_ok ? std::to_string(full[8]) : "?"));
    }
    {
        libusb_device_descriptor descriptor {};
        std::string manufacturer;
        std::string product;
        std::string serial;
        bool ok = libusb_get_device_descriptor(libusb_get_device(handle), &descriptor) == 0;
        auto read_string = [&](std::uint8_t index, std::string &out) {
            if (index == 0) {
                return false;
            }
            unsigned char buffer[128];
            const int result =
                    libusb_get_string_descriptor_ascii(handle, index, buffer, sizeof(buffer));
            if (result <= 0) {
                return false;
            }
            out.assign(reinterpret_cast<const char *>(buffer), static_cast<std::size_t>(result));
            return true;
        };
        // 索引由设备描述符给出，不假设固定编号
        ok = ok && read_string(descriptor.iManufacturer, manufacturer) &&
             read_string(descriptor.iProduct, product) &&
             read_string(descriptor.iSerialNumber, serial);
        const bool strings_ok = manufacturer == "usbipdcpp" &&
                                product == "usbipdcpp control" &&
                                serial == "usbipdcpp-e2e-control";
        tests.check(ok && strings_ok, "standard get string descriptors",
                    "manufacturer=" + manufacturer + " product=" + product +
                            " serial=" + serial);
    }
    {
        std::vector<std::uint8_t> data(16);
        const int result = libusb_control_transfer(handle, 0x80, USB_REQ_GET_DESCRIPTOR,
                                                   (0x0f << 8) | 0, 0, data.data(), 16, 1000);
        tests.check(result == LIBUSB_ERROR_PIPE, "standard unknown descriptor type",
                    "rc=" + libusb_error_string(result));
    }

    // ---- 标准请求：状态/配置/接口 ----
    {
        std::vector<std::uint8_t> data(2);
        const int result = libusb_control_transfer(handle, 0x80, USB_REQ_GET_STATUS, 0, 0,
                                                   data.data(), 2, 1000);
        // dummy_hcd 的 device status 只有 remote wakeup/self-powered 两位
        // （bmAttributes 带 self-powered，所以 bit0 可能为 1），其余位必须为 0
        tests.check(result == 2 && (data[0] & 0xfc) == 0 && data[1] == 0,
                    "standard get_status device",
                    "rc=" + libusb_error_string(result) + " status=" +
                            std::to_string(data[0]) + "," + std::to_string(data[1]));
    }
    {
        std::vector<std::uint8_t> data(2);
        const int result = libusb_control_transfer(handle, 0x81, USB_REQ_GET_STATUS, 0, 0,
                                                   data.data(), 2, 1000);
        // 接口没有 halt 位，规范要求返回全 0
        tests.check(result == 2 && data[0] == 0 && data[1] == 0,
                    "standard get_status interface",
                    "rc=" + libusb_error_string(result) + " status=" +
                            std::to_string(data[0]) + "," + std::to_string(data[1]));
    }
    {
        std::vector<std::uint8_t> data(1);
        const int result = libusb_control_transfer(handle, 0x80, USB_REQ_GET_CONFIGURATION, 0, 0,
                                                   data.data(), 1, 1000);
        tests.check(result == 1 && data[0] == 1, "standard get_configuration",
                    "rc=" + libusb_error_string(result));
    }
    {
        const int result = libusb_control_transfer(handle, 0x00, USB_REQ_SET_CONFIGURATION, 1, 0,
                                                   nullptr, 0, 1000);
        tests.check(result == 0, "standard set_configuration same",
                    "rc=" + libusb_error_string(result));
    }
    {
        std::vector<std::uint8_t> data(1);
        int result = libusb_control_transfer(handle, 0x81, USB_REQ_GET_INTERFACE, 0, 0,
                                             data.data(), 1, 1000);
        const bool initial_ok = result == 1 && data[0] == 0;
        result = libusb_control_transfer(handle, 0x01, USB_REQ_SET_INTERFACE, 1, 0, nullptr, 0, 1000);
        const bool set_ok = result == 0;
        std::vector<std::uint8_t> alt(1);
        const int alt_result = libusb_control_transfer(handle, 0x81, USB_REQ_GET_INTERFACE, 0, 0,
                                                       alt.data(), 1, 1000);
        const bool alt_ok = alt_result == 1 && alt[0] == 1;
        result = libusb_control_transfer(handle, 0x01, USB_REQ_SET_INTERFACE, 0, 0, nullptr, 0, 1000);
        std::vector<std::uint8_t> back(1);
        const int back_result = libusb_control_transfer(handle, 0x81, USB_REQ_GET_INTERFACE, 0, 0,
                                                        back.data(), 1, 1000);
        tests.check(initial_ok && set_ok && alt_ok && back_result == 1 && back[0] == 0,
                    "standard set/get interface alt",
                    "initial=" + std::to_string(initial_ok) + " set=" + std::to_string(set_ok) +
                            " alt=" + std::to_string(alt_ok));
    }

    // ---- 收尾：设备仍然可用 ----
    {
        std::vector<std::uint8_t> data(4);
        const int result = vendor_in(handle, proto::Request::get_info, 0, 0, data, 1000);
        tests.check(result == 4, "final get_info", "rc=" + libusb_error_string(result));
    }
}

}  // namespace

int main(int argc, char **argv) {
    using namespace usbipdcpp::e2e;
    Args args = parse_args(argc, argv, proto::vid, proto::pid_control);

    libusb_context *context = nullptr;
    if (libusb_init(&context) != 0) {
        std::fprintf(stderr, "libusb_init 失败\n");
        return 2;
    }

    libusb_device_handle *handle = nullptr;
    if (open_by_address(context, args, &handle) != 0) {
        libusb_exit(context);
        return 2;
    }
    libusb_set_auto_detach_kernel_driver(handle, 1);
    const int claim_result = libusb_claim_interface(handle, 0);
    if (claim_result != 0) {
        std::fprintf(stderr, "claim interface 0 失败: %s\n",
                     libusb_error_string(claim_result).c_str());
        libusb_close(handle);
        libusb_exit(context);
        return 2;
    }

    TestRunner tests;
    run_tests(context, handle, tests);

    libusb_release_interface(handle, 0);
    libusb_close(handle);
    libusb_exit(context);
    return tests.summary();
}
