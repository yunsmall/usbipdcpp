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

/**
 * @brief 一对 bulk 端点（同属一个 alt setting）
 */
struct Pair {
    std::uint8_t in = 0;
    std::uint8_t out = 0;
};

// 设备描述符布局：
// - 接口 0 alt 0：pair main（持续 4096 字节数据流）
// - 接口 0 alt 1：pair alt（持续 100 字节短包）
// - 接口 1 alt 0：无端点
// - 接口 1 alt 1：pair second
Pair main_pair;
Pair alt_pair;
Pair second_interface_pair;

std::uint64_t main_out_total = 0;
std::uint64_t alt_out_total = 0;
std::uint64_t second_out_total = 0;

bool load_pair(libusb_device_handle *handle, int interface_number, int alternate_setting,
               Pair &pair, const char *name) {
    std::vector<std::uint8_t> addresses;
    if (!get_endpoint_addresses(handle, interface_number, alternate_setting, addresses) ||
        addresses.size() != 2) {
        std::fprintf(stderr, "接口 %d alt %d 的端点数量不是 2（实际 %zu）\n", interface_number,
                     alternate_setting, addresses.size());
        return false;
    }
    pair.in = addresses[0];
    pair.out = addresses[1];
    // bulk 设备的端点固定为 high speed 下的 512 字节；类型/包长写错时
    // 后续 bulk 传输会失败，但错误信息指不到描述符，这里先把描述符本身校验掉
    std::string detail;
    const bool descriptors_ok = check_endpoint_descriptors(
            handle, interface_number, alternate_setting, LIBUSB_TRANSFER_TYPE_BULK, 512, detail);
    if (!descriptors_ok) {
        std::fprintf(stderr, "端点 %s 描述符不符: %s\n", name, detail.c_str());
    }
    const bool valid = (pair.in & LIBUSB_ENDPOINT_IN) && !(pair.out & LIBUSB_ENDPOINT_IN) &&
                       descriptors_ok;
    std::printf("端点 %s: in=0x%02x out=0x%02x\n", name, pair.in, pair.out);
    return valid;
}

bool load_endpoints(libusb_device_handle *handle) {
    return load_pair(handle, 0, 0, main_pair, "接口0 alt0") &&
           load_pair(handle, 0, 1, alt_pair, "接口0 alt1") &&
           load_pair(handle, 1, 1, second_interface_pair, "接口1 alt1");
}

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

bool set_in_enabled(libusb_device_handle *handle, std::uint8_t endpoint, bool enabled) {
    return vendor_out(handle, proto::Request::set_in_enabled, enabled ? 1 : 0, endpoint) == 0;
}

bool set_reply_len(libusb_device_handle *handle, std::uint8_t endpoint, std::uint32_t length) {
    return vendor_out(handle, proto::Request::set_reply_len, static_cast<std::uint16_t>(length),
                      endpoint) == 0;
}

bool get_out_stats(libusb_device_handle *handle, std::uint8_t endpoint, proto::OutStats &stats) {
    std::vector<std::uint8_t> data(sizeof(proto::OutStats));
    const int result = vendor_in(handle, proto::Request::get_out_stats, 0, endpoint, data, 1000);
    if (result != static_cast<int>(data.size())) {
        return false;
    }
    std::memcpy(&stats, data.data(), sizeof(stats));
    return true;
}

/**
 * @brief 等设备侧 OUT worker 把数据读走
 *
 * 主机的 OUT 传输完成时，gadget 侧的 read 可能还没跟上，直接查统计
 * 会少算刚发出去的字节。这里按累计字节数轮询，避免“上一笔还没统计完”
 * 导致按增量比较时多算。
 * min_chunks 用于零长度传输：bytes 不会变化，只能靠设备侧 read 次数
 * （chunks）确认 ZLP 确实被读走了。
 * @param handle 设备句柄
 * @param endpoint OUT 端点地址
 * @param total 期望的累计字节数
 * @param after 输出最新统计
 * @param min_chunks 期望的最小累计 read 次数
 * @return 追上返回 true，超时返回 false
 */
bool wait_out_total(libusb_device_handle *handle, std::uint8_t endpoint,
                    std::uint64_t total, proto::OutStats &after,
                    std::uint32_t min_chunks = 0) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        if (!get_out_stats(handle, endpoint, after)) {
            return false;
        }
        if (after.bytes >= total && after.chunks >= min_chunks) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

int bulk_in(libusb_device_handle *handle, std::uint8_t endpoint, std::vector<std::uint8_t> &data,
            int &transferred, int timeout) {
    return libusb_bulk_transfer(handle, endpoint, data.empty() ? nullptr : data.data(),
                                static_cast<int>(data.size()), &transferred, timeout);
}

int bulk_out(libusb_device_handle *handle, std::uint8_t endpoint, std::vector<std::uint8_t> &data,
             int &transferred, int timeout) {
    return libusb_bulk_transfer(handle, endpoint, data.empty() ? nullptr : data.data(),
                                static_cast<int>(data.size()), &transferred, timeout);
}

bool drain_until_timeout(libusb_device_handle *handle, std::uint8_t endpoint, int timeout_ms) {
    std::vector<std::uint8_t> buffer(4096);
    for (int attempt = 0; attempt < 20; ++attempt) {
        int transferred = 0;
        const int result = bulk_in(handle, endpoint, buffer, transferred, timeout_ms);
        if (result == LIBUSB_ERROR_TIMEOUT) {
            return true;
        }
    }
    return false;
}

/**
 * @brief 带 libusb transfer flag 的异步 bulk IN
 *
 * libusb_bulk_transfer 不接受 flags，SHORT_NOT_OK 这类语义只能手动建
 * transfer 再提交，这里同步等它完成。
 */
struct FlagTransferResult {
    libusb_transfer_status status = LIBUSB_TRANSFER_ERROR;
    int transferred = 0;
    bool done = false;
    bool submit_failed = false;
};

FlagTransferResult bulk_in_with_flags(libusb_context *context, libusb_device_handle *handle,
                                      std::uint8_t endpoint, std::vector<std::uint8_t> &data,
                                      int timeout, std::uint8_t flags) {
    FlagTransferResult result;
    auto *transfer = libusb_alloc_transfer(0);
    if (transfer == nullptr) {
        result.submit_failed = true;
        return result;
    }
    libusb_fill_bulk_transfer(
            transfer, handle, endpoint, data.empty() ? nullptr : data.data(),
            static_cast<int>(data.size()),
            [](libusb_transfer *completed) {
                auto *state = static_cast<FlagTransferResult *>(completed->user_data);
                state->status = completed->status;
                state->transferred = completed->actual_length;
                state->done = true;
            },
            &result, timeout);
    transfer->flags = flags;
    if (libusb_submit_transfer(transfer) != 0) {
        result.submit_failed = true;
        libusb_free_transfer(transfer);
        return result;
    }

    const auto wait_until = [&](int milliseconds) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
        while (!result.done && std::chrono::steady_clock::now() < deadline) {
            timeval slice{};
            slice.tv_sec = 0;
            slice.tv_usec = 20000;
            libusb_handle_events_timeout_completed(context, &slice, nullptr);
        }
    };
    wait_until(timeout > 0 ? timeout : 1000);
    if (!result.done) {
        libusb_cancel_transfer(transfer);
        wait_until(1000);
        if (!result.done) {
            // 等不到回调就放弃释放（进程即将退出，避免未定义行为）
            return result;
        }
    }
    libusb_free_transfer(transfer);
    return result;
}

/**
 * @brief IN 端点数据流的跨传输连续性跟踪
 *
 * verify_increment 只校验一段数据内部相邻字节 +1，起始字节是不是接着上一次
 * 读取（设备侧 next_byte 的累加）它看不出来，这里补上。
 */
struct InStream {
    bool initialized = false;
    std::uint8_t expected = 0;
};

/**
 * @brief 校验一段 IN 数据是否内部连续并接得上上一段
 * @param data 数据
 * @param length 长度
 * @param stream 该端点的流跟踪状态
 * @param detail 失败细节输出
 * @return 正确返回 true
 */
bool check_stream(const std::uint8_t *data, std::size_t length, InStream &stream,
                  std::string &detail) {
    if (length == 0 || !verify_increment(data, length)) {
        detail = "数据内部不连续";
        return false;
    }
    if (stream.initialized && data[0] != stream.expected) {
        detail = "起始字节 " + std::to_string(data[0]) + " 应为 " +
                 std::to_string(stream.expected);
        return false;
    }
    stream.initialized = true;
    stream.expected = static_cast<std::uint8_t>(data[length - 1] + 1);
    return true;
}

/**
 * @brief 读一次指定长度的数据并校验连续性
 * @param handle 设备句柄
 * @param endpoint IN 端点
 * @param length 期望长度
 * @param stream 跨传输的流跟踪状态，传 nullptr 只校验内部连续性
 * @param detail 失败细节输出
 * @return 成功返回 true
 */
bool read_and_verify(libusb_device_handle *handle, std::uint8_t endpoint, std::size_t length,
                     InStream *stream, std::string &detail) {
    std::vector<std::uint8_t> data(length);
    int transferred = 0;
    const int result = bulk_in(handle, endpoint, data, transferred, 5000);
    bool ok = result == 0 && transferred == static_cast<int>(length);
    if (ok) {
        if (stream != nullptr) {
            ok = check_stream(data.data(), length, *stream, detail);
        }
        else {
            ok = verify_increment(data.data(), length);
        }
    }
    if (!ok && detail.empty()) {
        detail = "in=" + hex_byte(endpoint) + " rc=" + libusb_error_string(result) +
                 " transferred=" + std::to_string(transferred);
    }
    return ok;
}

/**
 * @brief 发一次数据并等设备侧统计追上
 * @param handle 设备句柄
 * @param pair 端点对
 * @param length 数据长度
 * @param total 该 OUT 端点的累计字节数（调用方维护）
 * @param detail 失败细节输出
 * @return 成功返回 true
 */
bool write_and_verify(libusb_device_handle *handle, const Pair &pair, std::size_t length,
                      std::uint64_t &total, std::string &detail) {
    std::vector<std::uint8_t> data(length);
    fill_increment(data, static_cast<std::uint8_t>(total & 0xff));
    int transferred = 0;
    const int result = bulk_out(handle, pair.out, data, transferred, 5000);
    const std::uint64_t expected_total = total + length;
    proto::OutStats after {};
    const bool after_ok = wait_out_total(handle, pair.out, expected_total, after);
    const bool ok = result == 0 && transferred == static_cast<int>(length) && after_ok &&
                    after.bytes == expected_total && after.errors == 0;
    if (!ok) {
        detail = "out=" + hex_byte(pair.out) + " rc=" + libusb_error_string(result) +
                 " transferred=" + std::to_string(transferred) + " bytes=" +
                 std::to_string(after.bytes) + "/" + std::to_string(expected_total) +
                 " errors=" + std::to_string(after.errors);
    }
    else {
        // 只有真正到达设备才推进累计基准，失败时不污染后续用例
        total = expected_total;
    }
    return ok;
}

/**
 * @brief 读 GET_INTERFACE 看当前 alt setting
 * @param handle 设备句柄
 * @param interface_number 接口号
 * @return 当前 alt，失败返回 -1
 */
int get_interface_alt(libusb_device_handle *handle, int interface_number) {
    std::uint8_t value = 0xff;
    const int result = libusb_control_transfer(handle, 0x81, USB_REQ_GET_INTERFACE, 0,
                                               static_cast<std::uint16_t>(interface_number),
                                               &value, 1, 1000);
    return result == 1 ? static_cast<int>(value) : -1;
}

bool set_interface_alt(libusb_device_handle *handle, int interface_number, int alternate_setting) {
    return libusb_set_interface_alt_setting(handle, interface_number, alternate_setting) == 0;
}

void run_in_size_tests(libusb_device_handle *handle, TestRunner &tests) {
    // IN 端点持续输出全局连续的数据流，主机想读多长就能读多长。
    // 注意：dummy_hcd 对“主机读的长度不是 maxpacket 整数倍、而设备侧
    // 还有更多排队数据”的情况会返回 -EOVERFLOW（映射成 PIPE），这是
    // 该虚拟控制器模拟的固有限制，所以整包读取只测 512 的整数倍；
    // 非整包读取用第二个 alt 的短包行为覆盖。
    // 设备侧的 next_byte 跨 write 累加，所以每段数据的首字节必须接上
    // 上一段的末尾，这里用 stream 一起校验
    InStream stream;
    // 这是 main 端点的第一次读取，设备侧 next_byte 从 0 开始，首字节必须是 0
    stream.initialized = true;
    stream.expected = 0;
    for (const std::size_t length :
         {std::size_t {512}, std::size_t {1024}, std::size_t {1536}, std::size_t {4096},
          std::size_t {65536}, std::size_t {1024 * 1024}}) {
        std::vector<std::uint8_t> data(length);
        int transferred = 0;
        const int result = bulk_in(handle, main_pair.in, data, transferred, 10000);
        std::string detail;
        bool ok = result == 0 && transferred == static_cast<int>(length);
        if (ok) {
            ok = check_stream(data.data(), length, stream, detail);
        }
        if (!ok && detail.empty()) {
            detail = "rc=" + libusb_error_string(result) + " transferred=" +
                     std::to_string(transferred);
        }
        tests.check(ok, "bulk in len " + std::to_string(length), detail);
    }
}

void run_zlp_test(libusb_device_handle *handle, TestRunner &tests) {
    // 把回复长度设成 0，设备会持续发零长包
    bool saw_zero = false;
    if (!set_reply_len(handle, main_pair.in, 0)) {
        tests.check(false, "bulk zero length packet", "set_reply_len 失败");
    }
    else {
        std::vector<std::uint8_t> data(4096);
        for (int attempt = 0; attempt < 10 && !saw_zero; ++attempt) {
            int transferred = 0;
            const int result = bulk_in(handle, main_pair.in, data, transferred, 1000);
            if (result == 0 && transferred == 0) {
                saw_zero = true;
            }
        }
        tests.check(saw_zero, "bulk zero length packet");
    }

    // 恢复普通长度后，残留的零长请求可能还会回来，读到正长度为止；
    // 设备侧可能已经排了几个零长请求，上限留大一点避免环境敏感。
    // 前面的读取总量是 256 的整数倍，零长包不应推进设备侧 next_byte，
    // 所以恢复后第一包的首字节必须是 0——只查段内 +1 发现不了起点偏移
    set_reply_len(handle, main_pair.in, 4096);
    std::vector<std::uint8_t> data(4096);
    bool recovered = false;
    std::string zlp_detail;
    for (int attempt = 0; attempt < 50 && !recovered; ++attempt) {
        int transferred = 0;
        const int result = bulk_in(handle, main_pair.in, data, transferred, 1000);
        recovered = result == 0 && transferred == static_cast<int>(data.size()) &&
                    data[0] == 0 && verify_increment(data.data(), static_cast<std::size_t>(transferred));
        if (!recovered && result == 0 && transferred > 0) {
            zlp_detail = "first=" + std::to_string(data[0]) + " transferred=" +
                         std::to_string(transferred);
        }
    }
    tests.check(recovered, "bulk recovery after zlp", zlp_detail);
}

void run_out_size_tests(libusb_device_handle *handle, TestRunner &tests) {
    // 所有 OUT 传输共用一条全局连续的数据流，设备侧按“相邻字节 +1”
    // 校验，即使 usbfs 把一次传输拆成多个 URB 也不会误报
    std::uint8_t seed = 0;
    for (const std::size_t length :
         {std::size_t {0}, std::size_t {1}, std::size_t {511}, std::size_t {512},
          std::size_t {513}, std::size_t {1024}, std::size_t {4096}, std::size_t {65535},
          std::size_t {1024 * 1024}}) {
        proto::OutStats before {};
        proto::OutStats after {};
        const bool before_ok = get_out_stats(handle, main_pair.out, before);
        std::vector<std::uint8_t> data(length);
        fill_increment(data, seed);
        seed = static_cast<std::uint8_t>(seed + length);
        int transferred = 0;
        const int result = bulk_out(handle, main_pair.out, data, transferred, 10000);
        const std::uint64_t expected_total = main_out_total + length;
        // 设备侧按 maxpacket 分块读；零长度传输也会占一次 read，所以用
        // chunks 增量确认 ZLP 真的被 worker 读走（只比 bytes 时 0 长度恒成立）
        const std::uint32_t expected_chunks =
                length == 0 ? 1u : static_cast<std::uint32_t>((length + 511) / 512);
        const std::uint32_t total_chunks = before.chunks + expected_chunks;
        const bool after_ok =
                before_ok && wait_out_total(handle, main_pair.out, expected_total, after,
                                            total_chunks);
        const bool ok = after_ok && result == 0 &&
                        transferred == static_cast<int>(length) &&
                        after.bytes == expected_total && after.errors == 0;
        if (ok) {
            main_out_total = expected_total;
        }
        tests.check(ok, "bulk out len " + std::to_string(length),
                    "rc=" + libusb_error_string(result) + " transferred=" +
                            std::to_string(transferred) + " bytes=" +
                            std::to_string(after.bytes) + "/" +
                            std::to_string(expected_total) + " chunks=" +
                            std::to_string(after.chunks) + "/" +
                            std::to_string(total_chunks) + " errors=" +
                            std::to_string(after.errors));
    }
}

void run_alt_pair_tests(libusb_context *context, libusb_device_handle *handle, TestRunner &tests,
                        InStream &stream) {
    // 第二个 alt 的 IN 端点固定发 100 字节，用来验证“设备回短包”的行为
    {
        std::vector<std::uint8_t> data(4096);
        int transferred = 0;
        const int result = bulk_in(handle, alt_pair.in, data, transferred, 5000);
        std::string detail;
        const bool ok = result == 0 && transferred == 100 &&
                        check_stream(data.data(), 100, stream, detail);
        if (!ok && detail.empty()) {
            detail = "in=" + hex_byte(alt_pair.in) + " rc=" + libusb_error_string(result) +
                     " transferred=" + std::to_string(transferred);
        }
        tests.check(ok, "bulk in alt setting short packet", detail);
    }
    // 非 maxpacket 整数倍的请求：设备只有 100 字节数据，短包结束传输
    {
        std::vector<std::uint8_t> data(511);
        int transferred = 0;
        const int result = bulk_in(handle, alt_pair.in, data, transferred, 5000);
        std::string detail;
        const bool ok = result == 0 && transferred == 100 &&
                        check_stream(data.data(), 100, stream, detail);
        if (!ok && detail.empty()) {
            detail = "in=" + hex_byte(alt_pair.in) + " rc=" + libusb_error_string(result) +
                     " transferred=" + std::to_string(transferred);
        }
        tests.check(ok, "bulk in alt setting short packet 511", detail);
    }
    {
        std::string detail;
        const bool ok = write_and_verify(handle, alt_pair, 2048, alt_out_total, detail);
        tests.check(ok, "bulk out alt setting", detail);
    }
    // SHORT_NOT_OK：短包必须报错而不是静默成功。这条覆盖
    // get_libusb_transfer_flags 把 URB_SHORT_NOT_OK 传下去，以及
    // transfer_callback 在 flag 已设时不把 ERROR 改回 COMPLETED
    {
        std::vector<std::uint8_t> data(4096);
        const auto result = bulk_in_with_flags(context, handle, alt_pair.in, data, 5000,
                                               LIBUSB_TRANSFER_SHORT_NOT_OK);
        std::string detail;
        bool ok = !result.submit_failed && result.done && result.transferred == 100 &&
                  result.status != LIBUSB_TRANSFER_COMPLETED;
        if (ok) {
            ok = check_stream(data.data(), 100, stream, detail);
        }
        if (!ok && detail.empty()) {
            detail = "status=" + std::to_string(static_cast<int>(result.status)) +
                     " transferred=" + std::to_string(result.transferred) +
                     (result.submit_failed ? " 提交失败" : (result.done ? "" : " 未完成"));
        }
        tests.check(ok, "bulk in SHORT_NOT_OK 短包报错", detail);
    }
}

void run_concurrency(libusb_context *context, libusb_device_handle *handle, TestRunner &tests) {
    // 集中在同一个 IN 端点提交 16 条异步传输
    constexpr int total = 16;
    constexpr int transfer_length = 4096;
    std::vector<AsyncResult> results(total);
    std::vector<std::pair<libusb_transfer *, std::size_t>> pending;
    const bool configured = set_reply_len(handle, main_pair.in, transfer_length);

    for (int i = 0; i < total; ++i) {
        libusb_transfer *transfer = submit_async(handle, main_pair.in, transfer_length, 10000,
                                                 false, &results[i]);
        if (transfer != nullptr) {
            pending.emplace_back(transfer, static_cast<std::size_t>(i));
        }
    }

    bool all_done = pending.size() == static_cast<std::size_t>(total);
    for (auto &[transfer, index] : pending) {
        (void) transfer;
        all_done = wait_async(context, results[index], 10000) && all_done;
    }

    bool ok = all_done && configured;
    std::string detail = "configured=" + std::to_string(configured) +
                         " submitted=" + std::to_string(pending.size());
    // 每条传输内部校验逐字节 +1，且首字节必须是 0：4096 是 256 的整数倍，
    // 设备侧每个 4096 chunk 的首字节恒为 0，并发完成顺序又由下层调度决定，
    // 跨 chunk 的先后不对应任何可断言的契约。只查段内 +1 的话，设备侧
    // 起点整体偏移也能通过
    for (auto &[transfer, index] : pending) {
        (void) transfer;
        const bool single_ok = results[index].status == LIBUSB_TRANSFER_COMPLETED &&
                               results[index].actual_length == transfer_length &&
                               results[index].data[0] == 0 &&
                               verify_increment(results[index].data.data(),
                                                results[index].data.size());
        if (!single_ok) {
            detail += " [" + std::to_string(index) + ": status=" +
                      libusb_error_string(results[index].status) + " actual=" +
                      std::to_string(results[index].actual_length) + "]";
        }
        ok = ok && single_ok;
    }
    for (auto &[transfer, index] : pending) {
        cancel_and_free_transfer(context, transfer, &results[index], 1000);
    }
    tests.check(ok, "bulk concurrent in", detail);
}

void run_cancel(libusb_context *context, libusb_device_handle *handle, TestRunner &tests) {
    set_in_enabled(handle, main_pair.in, false);
    // 先把已经排队的 chunk 读空，否则取消时传输可能已经完成
    const bool drained = drain_until_timeout(handle, main_pair.in, 200);
    AsyncResult async;
    libusb_transfer *transfer = submit_async(handle, main_pair.in, 4096, 10000, false, &async);
    bool ok = drained && transfer != nullptr;
    if (transfer != nullptr) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        libusb_cancel_transfer(transfer);
        ok = ok && wait_async(context, async, 3000) && async.status == LIBUSB_TRANSFER_CANCELLED;
        cancel_and_free_transfer(context, transfer, &async, 1000);
    }
    set_in_enabled(handle, main_pair.in, true);
    tests.check(ok, "bulk cancel in flight");

    // 取消之后端点还能用，且数据流仍然正确
    std::vector<std::uint8_t> data(1024);
    int transferred = 0;
    const int result = bulk_in(handle, main_pair.in, data, transferred, 5000);
    tests.check(result == 0 && transferred == 1024 &&
                        verify_increment(data.data(), 1024),
                "bulk recovery after cancel",
                "rc=" + libusb_error_string(result) + " transferred=" +
                        std::to_string(transferred));
}

void run_timeout(libusb_device_handle *handle, TestRunner &tests) {
    set_in_enabled(handle, main_pair.in, false);
    const bool drained = drain_until_timeout(handle, main_pair.in, 200);
    std::vector<std::uint8_t> data(64);
    int transferred = 0;
    const int result = bulk_in(handle, main_pair.in, data, transferred, 100);
    set_in_enabled(handle, main_pair.in, true);
    tests.check(drained && result == LIBUSB_ERROR_TIMEOUT, "bulk in timeout",
                "rc=" + libusb_error_string(result));
}

void run_halt_tests(libusb_device_handle *handle, TestRunner &tests) {
    // ---- IN 端点 halt ----
    set_in_enabled(handle, main_pair.in, false);
    const bool drained = drain_until_timeout(handle, main_pair.in, 200);
    int result = libusb_control_transfer(handle, 0x02, USB_REQ_SET_FEATURE, USB_ENDPOINT_HALT,
                                         main_pair.in, nullptr, 0, 1000);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    {
        std::vector<std::uint8_t> data(64);
        int transferred = 0;
        const int wait_result = bulk_in(handle, main_pair.in, data, transferred, 1000);
        tests.check(result == 0 && drained && wait_result == LIBUSB_ERROR_PIPE,
                    "bulk halted in endpoint",
                    "drained=" + std::to_string(drained) +
                            " rc=" + libusb_error_string(wait_result) +
                            " set_feature=" + std::to_string(result));
    }
    result = libusb_clear_halt(handle, main_pair.in);
    set_in_enabled(handle, main_pair.in, true);
    {
        std::vector<std::uint8_t> data(4096);
        int transferred = 0;
        const int in_result = bulk_in(handle, main_pair.in, data, transferred, 5000);
        const bool data_ok = in_result == 0 && transferred == 4096 &&
                             verify_increment(data.data(), 4096);
        tests.check(result == 0 && data_ok, "bulk recovery after clear_halt",
                    "rc=" + libusb_error_string(in_result) + " transferred=" +
                            std::to_string(transferred));
    }

    // ---- OUT 端点 halt ----
    // OUT worker 平时阻塞在 read 上，只有收到一次主机数据后才会回到循环
    // 顶部处理 halt。这时的端点已经置 halt，触发用的 OUT 通常会被拒绝
    // （PIPE/超时），但不同实现下也可能在 halt 生效前被接收，所以发完
    // 读一次统计，把真正到达设备的字节数补进累计基准。
    result = libusb_control_transfer(handle, 0x02, USB_REQ_SET_FEATURE, USB_ENDPOINT_HALT,
                                     main_pair.out, nullptr, 0, 1000);
    proto::OutStats trigger_before {};
    proto::OutStats trigger_after {};
    const bool trigger_before_ok = get_out_stats(handle, main_pair.out, trigger_before);
    {
        std::vector<std::uint8_t> trigger(8);
        // 和 OUT 数据流用同一个种子：真机上这次写若在 halt 生效前被接收，
        // 字节序列也不会断
        fill_increment(trigger, static_cast<std::uint8_t>(main_out_total & 0xff));
        int transferred = 0;
        bulk_out(handle, main_pair.out, trigger, transferred, 300);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const bool trigger_after_ok = get_out_stats(handle, main_pair.out, trigger_after);
    if (trigger_before_ok && trigger_after_ok) {
        main_out_total += trigger_after.bytes - trigger_before.bytes;
    }
    {
        std::vector<std::uint8_t> data(8);
        int transferred = 0;
        const int wait_result = bulk_out(handle, main_pair.out, data, transferred, 1000);
        tests.check(result == 0 && trigger_before_ok && trigger_after_ok &&
                            wait_result == LIBUSB_ERROR_PIPE,
                    "bulk halted out endpoint",
                    "trigger_stats=" + std::to_string(trigger_before_ok) + "/" +
                            std::to_string(trigger_after_ok) + " rc=" +
                            libusb_error_string(wait_result) +
                            " set_feature=" + std::to_string(result));
    }
    result = libusb_clear_halt(handle, main_pair.out);
    {
        // 清除 halt 会让设备侧挂起的 read 被内核取消，worker 重新提交请求
        // 需要一点时间；这段时间内主机发来的数据会被 dummy_hcd 丢掉
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::string detail;
        const bool out_ok = write_and_verify(handle, main_pair, 8, main_out_total, detail);
        tests.check(result == 0 && out_ok, "bulk recovery after clear_halt out", detail);
    }
}

/**
 * @brief 多接口 / 多 alt setting 的切换测试
 *
 * 先把每个 alt 都切一遍，确认 GET_INTERFACE 回读和对应端点的收发；
 * 然后在两个接口上反复来回切换，每次切换后都立刻用当前 alt 的端点
 * 收发一次，验证端点被禁用/重新启用后 workers 能恢复。
 */
void run_alt_switching_tests(libusb_context *context, libusb_device_handle *handle,
                             TestRunner &tests) {
    // 三个 IN 端点各自维护流跟踪：alt setting 切换只禁用/重启用端点，
    // 设备侧 next_byte 不会重置，切换前后读到的数据必须连得上
    InStream main_stream;
    InStream alt_stream;
    InStream second_stream;
    {
        std::string detail;
        bool ok = set_interface_alt(handle, 0, 1);
        ok = ok && get_interface_alt(handle, 0) == 1;
        if (!ok) {
            detail = "切到接口0 alt1 后 GET_INTERFACE=" +
                     std::to_string(get_interface_alt(handle, 0));
        }
        tests.check(ok, "switch interface 0 to alt 1", detail);
    }
    run_alt_pair_tests(context, handle, tests, alt_stream);
    {
        std::string detail;
        bool ok = set_interface_alt(handle, 1, 1);
        ok = ok && get_interface_alt(handle, 1) == 1;
        if (!ok) {
            detail = "切到接口1 alt1 后 GET_INTERFACE=" +
                     std::to_string(get_interface_alt(handle, 1));
        }
        tests.check(ok, "switch interface 1 to alt 1", detail);
    }
    {
        std::string detail;
        bool ok = read_and_verify(handle, second_interface_pair.in, 4096, &second_stream,
                                  detail);
        ok = write_and_verify(handle, second_interface_pair, 512, second_out_total, detail) &&
             ok;
        tests.check(ok, "second interface alt 1 transfers", detail);
    }

    // 接口 1 切回 alt 0（没有端点），再切回 alt 1，确认无端点 alt 也能正常切换
    {
        std::string detail;
        bool ok = set_interface_alt(handle, 1, 0);
        ok = ok && get_interface_alt(handle, 1) == 0;
        ok = set_interface_alt(handle, 1, 1) && ok;
        ok = ok && read_and_verify(handle, second_interface_pair.in, 4096, &second_stream,
                                   detail);
        tests.check(ok, "switch interface 1 empty alt 0 and back", detail);
    }

    // 两个接口交替切换，每轮都验证当前 alt 的端点能正常收发
    for (int round = 0; round < 10; ++round) {
        std::string detail;
        const int target0 = round % 2;
        const int target1 = (round + 1) % 2;
        bool ok = set_interface_alt(handle, 0, target0);
        ok = ok && get_interface_alt(handle, 0) == target0;
        ok = set_interface_alt(handle, 1, target1) && ok;
        ok = ok && get_interface_alt(handle, 1) == target1;

        InStream &active_stream = target0 == 0 ? main_stream : alt_stream;
        const Pair &active = target0 == 0 ? main_pair : alt_pair;
        ok = read_and_verify(handle, active.in, target0 == 0 ? 4096 : 100, &active_stream,
                             detail) &&
             ok;
        ok = write_and_verify(handle, active, 64, target0 == 0 ? main_out_total : alt_out_total,
                              detail) &&
             ok;
        if (target1 == 1) {
            ok = read_and_verify(handle, second_interface_pair.in, 4096, &second_stream,
                                 detail) &&
                 ok;
            ok = write_and_verify(handle, second_interface_pair, 64, second_out_total, detail) &&
                 ok;
        }
        tests.check(ok, "alt switch round " + std::to_string(round), detail);
    }

    // 最后切回默认配置，确认主端点仍然可用
    {
        std::string detail;
        bool ok = set_interface_alt(handle, 0, 0);
        ok = set_interface_alt(handle, 1, 0) && ok;
        ok = ok && get_interface_alt(handle, 0) == 0 && get_interface_alt(handle, 1) == 0;
        ok = read_and_verify(handle, main_pair.in, 4096, &main_stream, detail) && ok;
        ok = write_and_verify(handle, main_pair, 4096, main_out_total, detail) && ok;
        tests.check(ok, "restore default alt settings", detail);
    }
}

}  // namespace

int main(int argc, char **argv) {
    using namespace usbipdcpp::e2e;
    Args args = parse_args(argc, argv, proto::vid, proto::pid_bulk);

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
    const int claim_result1 = libusb_claim_interface(handle, 1);
    if (claim_result != 0 || claim_result1 != 0) {
        std::fprintf(stderr, "claim interface 失败: %s / %s\n",
                     libusb_error_string(claim_result).c_str(),
                     libusb_error_string(claim_result1).c_str());
        libusb_close(handle);
        libusb_exit(context);
        return 2;
    }

    TestRunner tests;
    if (!load_endpoints(handle)) {
        std::fprintf(stderr, "端点地址不符合预期\n");
        libusb_release_interface(handle, 1);
        libusb_release_interface(handle, 0);
        libusb_close(handle);
        libusb_exit(context);
        return 2;
    }

    std::vector<std::uint8_t> info(4);
    const int info_result = vendor_in(handle, proto::Request::get_info, 0, 0, info, 1000);
    tests.check(info_result == 4, "bulk device get_info", "rc=" + libusb_error_string(info_result));

    run_in_size_tests(handle, tests);
    run_zlp_test(handle, tests);
    run_out_size_tests(handle, tests);
    run_concurrency(context, handle, tests);
    run_cancel(context, handle, tests);
    run_timeout(handle, tests);
    run_halt_tests(handle, tests);
    run_alt_switching_tests(context, handle, tests);

    libusb_release_interface(handle, 1);
    libusb_release_interface(handle, 0);
    libusb_close(handle);
    libusb_exit(context);
    return tests.summary();
}
