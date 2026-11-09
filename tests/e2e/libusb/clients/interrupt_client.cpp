#include "client_common.hpp"
#include "protocol.hpp"

#include <algorithm>
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

/**
 * @brief 按配置描述符顺序得到的端点地址
 */
struct Endpoints {
    std::uint8_t in = 0;
    std::uint8_t out = 0;
};

Endpoints endpoints;

bool load_endpoints(libusb_device_handle *handle) {
    std::vector<std::uint8_t> addresses;
    if (!get_endpoint_addresses(handle, 0, 0, addresses) || addresses.size() != 2) {
        std::fprintf(stderr, "配置描述符里端点数量不是 2（实际 %zu）\n", addresses.size());
        return false;
    }
    endpoints.in = addresses[0];
    endpoints.out = addresses[1];
    // 设备侧 descriptor 固定为 interrupt、64 字节；类型/包长写错时后续传输
    // 报错指不到根因，先把描述符校验掉
    std::string detail;
    if (!check_endpoint_descriptors(handle, 0, 0, LIBUSB_TRANSFER_TYPE_INTERRUPT, 64, detail)) {
        std::fprintf(stderr, "端点描述符不符: %s\n", detail.c_str());
        return false;
    }
    const bool valid = (endpoints.in & LIBUSB_ENDPOINT_IN) &&
                       !(endpoints.out & LIBUSB_ENDPOINT_IN);
    std::printf("端点: in=0x%02x out=0x%02x\n", endpoints.in, endpoints.out);
    return valid;
}

constexpr std::uint32_t counter_magic = 0x5a5a5a5a;

int vendor_in(libusb_device_handle *handle, proto::Request request, std::uint16_t value,
              std::uint16_t index, std::vector<std::uint8_t> &data, int timeout) {
    return libusb_control_transfer(handle, proto::request_type_in,
                                   static_cast<std::uint8_t>(request), value, index,
                                   data.empty() ? nullptr : data.data(),
                                   static_cast<std::uint16_t>(data.size()), timeout);
}

int vendor_out(libusb_device_handle *handle, proto::Request request, std::uint16_t value,
               std::uint16_t index) {
    return libusb_control_transfer(handle, proto::request_type_out,
                                   static_cast<std::uint8_t>(request), value, index, nullptr, 0,
                                   1000);
}

bool set_in_enabled(libusb_device_handle *handle, bool enabled) {
    return vendor_out(handle, proto::Request::set_in_enabled, enabled ? 1 : 0, endpoints.in) == 0;
}

bool set_reply_len(libusb_device_handle *handle, std::uint32_t length) {
    return vendor_out(handle, proto::Request::set_reply_len, static_cast<std::uint16_t>(length),
                      endpoints.in) == 0;
}

bool get_out_stats(libusb_device_handle *handle, proto::OutStats &stats) {
    std::vector<std::uint8_t> data(sizeof(proto::OutStats));
    const int result = vendor_in(handle, proto::Request::get_out_stats, 0, endpoints.out, data,
                                 1000);
    if (result != static_cast<int>(data.size())) {
        return false;
    }
    std::memcpy(&stats, data.data(), sizeof(stats));
    return true;
}

bool wait_out_bytes(libusb_device_handle *handle, const proto::OutStats &before,
                    std::uint64_t expected, proto::OutStats &after) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (!get_out_stats(handle, after)) {
            return false;
        }
        if (after.bytes - before.bytes >= expected) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

int interrupt_in(libusb_device_handle *handle, std::vector<std::uint8_t> &data, int &transferred,
                 int timeout) {
    return libusb_interrupt_transfer(handle, endpoints.in, data.empty() ? nullptr : data.data(),
                                     static_cast<int>(data.size()), &transferred, timeout);
}

int interrupt_out(libusb_device_handle *handle, std::vector<std::uint8_t> &data, int &transferred,
                  int timeout) {
    return libusb_interrupt_transfer(handle, endpoints.out, data.empty() ? nullptr : data.data(),
                                     static_cast<int>(data.size()), &transferred, timeout);
}

/**
 * @brief 校验一段数据里每个 8 字节组的计数器和校验值
 * @param data 数据
 * @param length 长度
 * @param counters 追加解析出的计数器
 * @return 全部组正确返回 true
 */
bool verify_counter_blocks(const std::uint8_t *data, std::size_t length,
                           std::vector<std::uint32_t> &counters) {
    for (std::size_t offset = 0; offset + 8 <= length; offset += 8) {
        const std::uint32_t counter = read_le32(data + offset);
        const std::uint32_t check = read_le32(data + offset + 4);
        if (check != (counter ^ counter_magic)) {
            return false;
        }
        counters.push_back(counter);
    }
    return true;
}

/**
 * @brief 切到新长度后把旧长度的排队数据读掉，直到读满新长度
 *
 * 设备侧可能还排着上一个长度的 chunk，读到的长度会偏小；由于长度只
 * 递增，旧 chunk 一定比请求短，不会触发 dummy_hcd 的 -EOVERFLOW。
 * 所有读到的完整计数块都累积到 counters 里（包括短包），调用方据此
 * 校验跨请求的计数器连续性。
 */
bool read_until_size(libusb_device_handle *handle, std::size_t length,
                     std::vector<std::uint32_t> &counters) {
    std::vector<std::uint8_t> data(length);
    for (int attempt = 0; attempt < 50; ++attempt) {
        int transferred = 0;
        const int result = interrupt_in(handle, data, transferred, 2000);
        if (result != 0 || transferred <= 0 ||
            static_cast<std::size_t>(transferred) % 8 != 0) {
            return false;
        }
        if (!verify_counter_blocks(data.data(), static_cast<std::size_t>(transferred),
                                   counters)) {
            return false;
        }
        if (static_cast<std::size_t>(transferred) == length) {
            return true;
        }
    }
    return false;
}

void run_in_size_tests(libusb_device_handle *handle, TestRunner &tests) {
    std::vector<std::uint32_t> all_counters;

    // 长度按递增顺序切换，旧 chunk 更短，不会触发 -EOVERFLOW
    for (const std::size_t length :
         {std::size_t {8}, std::size_t {16}, std::size_t {64}, std::size_t {1024}}) {
        const bool configured = set_reply_len(handle, static_cast<std::uint32_t>(length));
        const bool ok = configured && read_until_size(handle, length, all_counters);
        tests.check(ok, "interrupt in len " + std::to_string(length));
    }

    // 设备侧计数器只在写成功后推进；端点被重新启用时可能丢掉已经排队的
    // chunk，所以主机看到的序列不一定从 0 开始，但中间不允许跳号或重复
    bool contiguous = !all_counters.empty();
    std::size_t broken_at = 0;
    for (std::size_t i = 1; i < all_counters.size(); ++i) {
        if (all_counters[i] != all_counters[i - 1] + 1) {
            contiguous = false;
            broken_at = i;
            break;
        }
    }
    tests.check(contiguous, "interrupt counter continuity across requests",
                "counters=" + std::to_string(all_counters.size()) + " first=" +
                        (all_counters.empty() ? std::string("n/a")
                                              : std::to_string(all_counters.front())) +
                        " broken_at=" + std::to_string(broken_at));
}

void run_out_size_tests(libusb_device_handle *handle, TestRunner &tests) {
    // OUT 数据流保持全局逐字节 +1，设备侧会校验跨 read 的连续性并计入 errors
    std::uint8_t seed = 0;
    for (const std::size_t length :
         {std::size_t {1}, std::size_t {8}, std::size_t {64}, std::size_t {1024}}) {
        proto::OutStats before {};
        proto::OutStats after {};
        const bool before_ok = get_out_stats(handle, before);
        std::vector<std::uint8_t> data(length);
        fill_increment(data, seed);
        seed = static_cast<std::uint8_t>(seed + length);
        int transferred = 0;
        const int result = interrupt_out(handle, data, transferred, 5000);
        const bool after_ok = wait_out_bytes(handle, before, length, after);
        const bool ok = before_ok && after_ok && result == 0 &&
                        transferred == static_cast<int>(length) &&
                        after.bytes - before.bytes == length && after.errors == before.errors;
        tests.check(ok, "interrupt out len " + std::to_string(length),
                    "rc=" + libusb_error_string(result) + " transferred=" +
                            std::to_string(transferred) + " errors=" +
                            std::to_string(after.errors - before.errors));
    }
}

void run_concurrency(libusb_context *context, libusb_device_handle *handle, TestRunner &tests) {
    constexpr int count = 8;
    constexpr int transfer_length = 1024;
    std::vector<AsyncResult> results(count);
    // 保存提交时的原始下标：submit 失败会跳过，不能用 transfers 的下标去索引 results
    std::vector<std::pair<libusb_transfer *, std::size_t>> pending;

    const bool configured = set_reply_len(handle, transfer_length);
    for (int i = 0; i < count; ++i) {
        libusb_transfer *transfer =
                submit_async(handle, endpoints.in, transfer_length, 5000, true, &results[i]);
        if (transfer != nullptr) {
            pending.emplace_back(transfer, static_cast<std::size_t>(i));
        }
    }

    bool all_done = pending.size() == static_cast<std::size_t>(count);
    for (auto &[transfer, index] : pending) {
        (void) transfer;
        all_done = wait_async(context, results[index], 5000) && all_done;
    }

    std::vector<std::uint32_t> counters;
    bool ok = all_done && configured;
    for (auto &[transfer, index] : pending) {
        std::vector<std::uint32_t> transfer_counters;
        ok = results[index].status == LIBUSB_TRANSFER_COMPLETED &&
             results[index].actual_length == transfer_length &&
             verify_counter_blocks(results[index].data.data(), results[index].data.size(),
                                   transfer_counters) &&
             ok;
        // 每条传输内部的计数器必须严格 +1
        for (std::size_t j = 1; j < transfer_counters.size() && ok; ++j) {
            ok = transfer_counters[j] == transfer_counters[0] + static_cast<std::uint32_t>(j);
        }
        counters.insert(counters.end(), transfer_counters.begin(), transfer_counters.end());
    }

    // 并发完成顺序由下层决定，所以对全部计数器排序后校验有没有空洞/重复，
    // 不依赖传输完成的先后
    std::sort(counters.begin(), counters.end());
    for (std::size_t i = 1; i < counters.size() && ok; ++i) {
        ok = counters[i] == counters[i - 1] + 1;
    }
    for (auto &[transfer, index] : pending) {
        cancel_and_free_transfer(context, transfer, &results[index], 1000);
    }
    tests.check(ok, "interrupt concurrent in",
                "configured=" + std::to_string(configured) +
                        " submitted=" + std::to_string(pending.size()) +
                        " counters=" + std::to_string(counters.size()));
}

bool drain_until_timeout(libusb_device_handle *handle, int timeout_ms) {
    std::vector<std::uint8_t> buffer(1024);
    for (int attempt = 0; attempt < 20; ++attempt) {
        int transferred = 0;
        const int result = interrupt_in(handle, buffer, transferred, timeout_ms);
        if (result == LIBUSB_ERROR_TIMEOUT) {
            return true;
        }
    }
    return false;
}

void run_cancel(libusb_context *context, libusb_device_handle *handle, TestRunner &tests) {
    set_in_enabled(handle, false);
    const bool drained = drain_until_timeout(handle, 200);
    AsyncResult async;
    libusb_transfer *transfer = submit_async(handle, endpoints.in, 1024, 5000, true, &async);
    bool ok = drained && transfer != nullptr;
    if (transfer != nullptr) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        libusb_cancel_transfer(transfer);
        ok = ok && wait_async(context, async, 3000) && async.status == LIBUSB_TRANSFER_CANCELLED;
        cancel_and_free_transfer(context, transfer, &async, 1000);
    }
    set_in_enabled(handle, true);
    tests.check(ok, "interrupt cancel in flight");

    std::vector<std::uint8_t> data(1024);
    int transferred = 0;
    const int result = interrupt_in(handle, data, transferred, 5000);
    std::vector<std::uint32_t> counters;
    const bool data_ok = result == 0 && transferred == 1024 &&
                         verify_counter_blocks(data.data(), 1024, counters);
    tests.check(data_ok, "interrupt recovery after cancel",
                "rc=" + libusb_error_string(result) + " transferred=" +
                        std::to_string(transferred));
}

void run_timeout(libusb_device_handle *handle, TestRunner &tests) {
    set_in_enabled(handle, false);
    const bool drained = drain_until_timeout(handle, 200);
    std::vector<std::uint8_t> data(1024);
    int transferred = 0;
    const int result = interrupt_in(handle, data, transferred, 100);
    set_in_enabled(handle, true);
    tests.check(drained && result == LIBUSB_ERROR_TIMEOUT, "interrupt in timeout",
                "rc=" + libusb_error_string(result));
}

void run_halt_tests(libusb_device_handle *handle, TestRunner &tests) {
    set_in_enabled(handle, false);
    const bool drained = drain_until_timeout(handle, 200);
    int result = libusb_control_transfer(handle, 0x02, USB_REQ_SET_FEATURE, USB_ENDPOINT_HALT,
                                         endpoints.in, nullptr, 0, 1000);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    {
        std::vector<std::uint8_t> data(64);
        int transferred = 0;
        const int wait_result = interrupt_in(handle, data, transferred, 1000);
        tests.check(result == 0 && drained && wait_result == LIBUSB_ERROR_PIPE,
                    "interrupt halted in endpoint",
                    "in=" + hex_byte(endpoints.in) + " drained=" + std::to_string(drained) +
                            " rc=" + libusb_error_string(wait_result) +
                            " set_feature=" + std::to_string(result));
    }
    result = libusb_clear_halt(handle, endpoints.in);
    set_in_enabled(handle, true);
    {
        std::vector<std::uint8_t> data(1024);
        int transferred = 0;
        const int in_result = interrupt_in(handle, data, transferred, 5000);
        std::vector<std::uint32_t> counters;
        const bool data_ok = in_result == 0 && transferred == 1024 &&
                             verify_counter_blocks(data.data(), 1024, counters);
        tests.check(result == 0 && data_ok, "interrupt recovery after clear_halt",
                    "rc=" + libusb_error_string(in_result) + " transferred=" +
                            std::to_string(transferred));
    }

    // ---- OUT 端点 halt ----
    // OUT worker 平时阻塞在 read 上，只有收到一次主机数据后才会回到循环顶部
    // 处理 halt。触发用的 OUT 可能被拒绝（PIPE），也可能在 halt 生效前被接收，
    // 所以用设备侧统计把真正到达的字节数算进流基准；设备侧 OUT 数据流逐字节
    // +1，下一包的种子必须接上已接收的字节数
    proto::OutStats stream {};
    const bool stream_ok = get_out_stats(handle, stream);
    result = libusb_control_transfer(handle, 0x02, USB_REQ_SET_FEATURE, USB_ENDPOINT_HALT,
                                     endpoints.out, nullptr, 0, 1000);
    proto::OutStats trigger_before {};
    proto::OutStats trigger_after {};
    const bool trigger_before_ok = get_out_stats(handle, trigger_before);
    {
        std::vector<std::uint8_t> trigger(8);
        fill_increment(trigger, static_cast<std::uint8_t>(stream.bytes & 0xff));
        int transferred = 0;
        interrupt_out(handle, trigger, transferred, 300);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const bool trigger_after_ok = get_out_stats(handle, trigger_after);
    {
        std::vector<std::uint8_t> data(8);
        int transferred = 0;
        const int wait_result = interrupt_out(handle, data, transferred, 1000);
        tests.check(result == 0 && stream_ok && trigger_before_ok && trigger_after_ok &&
                            wait_result == LIBUSB_ERROR_PIPE,
                    "interrupt halted out endpoint",
                    "stats=" + std::to_string(stream_ok) + "/" +
                            std::to_string(trigger_before_ok) + "/" +
                            std::to_string(trigger_after_ok) + " rc=" +
                            libusb_error_string(wait_result) +
                            " set_feature=" + std::to_string(result));
    }
    result = libusb_clear_halt(handle, endpoints.out);
    {
        // 清除 halt 会让设备侧挂起的 read 被内核取消，worker 重新提交请求需要
        // 一点时间；这段时间内主机发来的数据会被 dummy_hcd 丢掉
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        proto::OutStats before {};
        proto::OutStats after {};
        const bool before_ok = get_out_stats(handle, before);
        std::vector<std::uint8_t> data(64);
        fill_increment(data, static_cast<std::uint8_t>(before.bytes & 0xff));
        int transferred = 0;
        const int out_result = interrupt_out(handle, data, transferred, 5000);
        const bool after_ok = wait_out_bytes(handle, before, 64, after);
        const bool ok = before_ok && after_ok && out_result == 0 && transferred == 64 &&
                        after.bytes - before.bytes == 64 && after.errors == before.errors;
        tests.check(result == 0 && ok, "interrupt recovery after clear_halt out",
                    "rc=" + libusb_error_string(out_result) + " transferred=" +
                            std::to_string(transferred) + " errors=" +
                            std::to_string(after.errors - before.errors));
    }
}

}  // namespace

int main(int argc, char **argv) {
    using namespace usbipdcpp::e2e;
    Args args = parse_args(argc, argv, proto::vid, proto::pid_interrupt);

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
    if (!load_endpoints(handle)) {
        std::fprintf(stderr, "端点地址不符合预期\n");
        libusb_release_interface(handle, 0);
        libusb_close(handle);
        libusb_exit(context);
        return 2;
    }

    std::vector<std::uint8_t> info(4);
    const int info_result = vendor_in(handle, proto::Request::get_info, 0, 0, info, 1000);
    tests.check(info_result == 4, "interrupt device get_info",
                "rc=" + libusb_error_string(info_result));

    run_in_size_tests(handle, tests);
    run_out_size_tests(handle, tests);
    run_concurrency(context, handle, tests);
    run_cancel(context, handle, tests);
    run_timeout(handle, tests);
    run_halt_tests(handle, tests);

    libusb_release_interface(handle, 0);
    libusb_close(handle);
    libusb_exit(context);
    return tests.summary();
}
