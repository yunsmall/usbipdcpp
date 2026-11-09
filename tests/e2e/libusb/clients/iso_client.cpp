#include "client_common.hpp"
#include "iso_pattern.hpp"
#include "protocol.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <libusb-1.0/libusb.h>
#include <linux/usb/ch9.h>

namespace {

using namespace usbipdcpp::e2e;

constexpr int io_timeout_ms = 5000;
constexpr std::uint16_t max_packet_alt1 = 1024;
constexpr std::uint16_t max_packet_alt2 = 512;

/**
 * @brief 把 libusb 传输状态转成可读字符串
 *
 * libusb_error_string 只认 libusb_error 枚举，传输状态是另一套枚举，
 * 直接传进去只会得到 "unknown"。
 */
std::string transfer_status_string(int status) {
    switch (status) {
        case LIBUSB_TRANSFER_COMPLETED:
            return "COMPLETED";
        case LIBUSB_TRANSFER_ERROR:
            return "ERROR";
        case LIBUSB_TRANSFER_TIMED_OUT:
            return "TIMED_OUT";
        case LIBUSB_TRANSFER_CANCELLED:
            return "CANCELLED";
        case LIBUSB_TRANSFER_STALL:
            return "STALL";
        case LIBUSB_TRANSFER_NO_DEVICE:
            return "NO_DEVICE";
        case LIBUSB_TRANSFER_OVERFLOW:
            return "OVERFLOW";
        default:
            return "UNKNOWN(" + std::to_string(status) + ")";
    }
}

/**
 * @brief 一条 iso 传输的完成结果
 */
struct IsoResult {
    std::vector<std::uint8_t> buffer;
    std::vector<int> packet_status;
    std::vector<int> packet_actual;
    int status = LIBUSB_TRANSFER_ERROR;
    int actual_length = 0;
    bool done = false;
    bool submitted = false;
};

void LIBUSB_CALL iso_callback(libusb_transfer *transfer) {
    auto *result = static_cast<IsoResult *>(transfer->user_data);
    result->status = transfer->status;
    result->actual_length = transfer->actual_length;
    const int count = transfer->num_iso_packets;
    result->packet_status.resize(static_cast<std::size_t>(count));
    result->packet_actual.resize(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        result->packet_status[static_cast<std::size_t>(i)] = transfer->iso_packet_desc[i].status;
        result->packet_actual[static_cast<std::size_t>(i)] = transfer->iso_packet_desc[i].actual_length;
    }
    result->done = true;
}

/**
 * @brief 创建一条 iso 传输（缓冲区清零为 0xEE，OUT 方向由调用方随后填充）
 * @param handle 设备句柄
 * @param endpoint 端点地址
 * @param lengths 每个包的槽位长度
 * @param result 结果对象（必须在传输完成前保持地址稳定）
 * @param timeout_ms 传输超时（毫秒）
 * @return libusb_transfer，失败返回 nullptr
 */
libusb_transfer *make_iso_transfer(libusb_device_handle *handle, std::uint8_t endpoint,
                                   const std::vector<std::uint32_t> &lengths, IsoResult &result,
                                   int timeout_ms) {
    std::uint32_t total = 0;
    for (std::uint32_t length: lengths) {
        total += length;
    }
    // 全零长度时也给一个字节的缓冲，避免传 nullptr 给 libusb；长度仍按 0 上报
    result.buffer.assign(total > 0 ? total : 1, 0xee);
    result.packet_status.assign(lengths.size(), LIBUSB_TRANSFER_ERROR);
    result.packet_actual.assign(lengths.size(), 0);
    result.status = LIBUSB_TRANSFER_ERROR;
    result.actual_length = 0;
    result.done = false;
    result.submitted = false;

    auto *transfer = libusb_alloc_transfer(static_cast<int>(lengths.size()));
    if (transfer == nullptr) {
        return nullptr;
    }
    libusb_fill_iso_transfer(transfer, handle, endpoint, result.buffer.data(), static_cast<int>(total),
                             static_cast<int>(lengths.size()), iso_callback, &result, timeout_ms);
    // fill 默认把总长度平分到各包，必须逐个覆盖成测试想要的槽位长度
    for (std::size_t i = 0; i < lengths.size(); ++i) {
        transfer->iso_packet_desc[i].length = lengths[i];
    }
    return transfer;
}

/**
 * @brief 处理事件直到传输完成或超时
 * @param context libusb 上下文
 * @param result 结果对象
 * @param timeout_ms 最长等待毫秒数
 * @return 完成返回 true
 */
bool wait_result(libusb_context *context, IsoResult &result, int timeout_ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!result.done) {
        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 20000;
        const int rc = libusb_handle_events_timeout_completed(context, &timeout, nullptr);
        if (rc < 0 && rc != LIBUSB_ERROR_INTERRUPTED) {
            return false;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
    }
    return true;
}

/**
 * @brief 释放可能仍在传输中的 iso transfer
 *
 * libusb 不允许直接 free 已提交且未完成的 transfer：先取消并等回调落地，
 * 等不到就放弃释放（进程即将退出，避免未定义行为）。
 */
void cancel_and_free(libusb_context *context, libusb_transfer *transfer, IsoResult &result, int timeout_ms) {
    if (transfer == nullptr) {
        return;
    }
    if (!result.submitted) {
        libusb_free_transfer(transfer);
        return;
    }
    if (!result.done) {
        libusb_cancel_transfer(transfer);
        if (!wait_result(context, result, timeout_ms)) {
            return;
        }
    }
    libusb_free_transfer(transfer);
}

int vendor_in(libusb_device_handle *handle, std::uint8_t request, std::uint16_t value,
              std::vector<std::uint8_t> &data) {
    return libusb_control_transfer(handle, proto::iso_request_type_in, request, value, 0,
                                   data.empty() ? nullptr : data.data(),
                                   static_cast<std::uint16_t>(data.size()), 1000);
}

int vendor_out(libusb_device_handle *handle, std::uint8_t request, std::uint16_t value) {
    return libusb_control_transfer(handle, proto::iso_request_type_out, request, value, 0, nullptr, 0, 1000);
}

bool get_iso_stats(libusb_device_handle *handle, proto::IsoStats &stats) {
    std::vector<std::uint8_t> data(sizeof(proto::IsoStats));
    const int result = vendor_in(handle, static_cast<std::uint8_t>(proto::IsoRequest::get_iso_stats), 0, data);
    if (result != static_cast<int>(data.size())) {
        return false;
    }
    std::memcpy(&stats, data.data(), sizeof(stats));
    return true;
}

bool reset_iso_stats(libusb_device_handle *handle) {
    return vendor_out(handle, static_cast<std::uint8_t>(proto::IsoRequest::reset_iso_stats), 0) == 0;
}

bool set_in_delay(libusb_device_handle *handle, std::uint16_t milliseconds) {
    return vendor_out(handle, static_cast<std::uint8_t>(proto::IsoRequest::set_in_delay), milliseconds) == 0;
}

bool set_fail_mask(libusb_device_handle *handle, std::uint32_t mask) {
    return vendor_out(handle, static_cast<std::uint8_t>(proto::IsoRequest::set_fail_mask),
                      static_cast<std::uint16_t>(mask)) == 0;
}

int get_current_alt(libusb_device_handle *handle) {
    std::uint8_t value = 0xff;
    const auto request_type = static_cast<std::uint8_t>(
            static_cast<unsigned>(LIBUSB_ENDPOINT_IN) | static_cast<unsigned>(LIBUSB_REQUEST_TYPE_STANDARD) |
            static_cast<unsigned>(LIBUSB_RECIPIENT_INTERFACE));
    const int result = libusb_control_transfer(handle, request_type, LIBUSB_REQUEST_GET_INTERFACE, 0, 0, &value, 1,
                                               1000);
    return result == 1 ? value : -1;
}

std::string lengths_to_string(const std::vector<std::uint32_t> &lengths) {
    std::string result;
    for (std::size_t i = 0; i < lengths.size(); ++i) {
        if (i != 0) {
            result += ',';
        }
        result += std::to_string(lengths[i]);
    }
    return result;
}

/**
 * @brief 校验 IN 传输：整体状态、逐包状态/长度、包内容
 *
 * libusb 文档明确说明 libusb_transfer::actual_length 对等时传输没有意义，
 * 有效长度只能逐包累加 iso_packet_desc[i].actual_length（实测该字段恒为 0）。
 */
bool check_in_result(const IsoResult &result, const std::vector<std::uint32_t> &lengths,
                     std::string &detail) {
    if (result.status != LIBUSB_TRANSFER_COMPLETED) {
        detail = "传输状态 " + transfer_status_string(result.status) + "（等待完成失败）";
        return false;
    }
    std::uint32_t offset = 0;
    std::uint32_t actual_total = 0;
    for (std::size_t i = 0; i < lengths.size(); ++i) {
        if (result.packet_status[i] != LIBUSB_TRANSFER_COMPLETED) {
            detail = "包 " + std::to_string(i) + " 状态 " +
                     transfer_status_string(result.packet_status[i]);
            return false;
        }
        if (result.packet_actual[i] != static_cast<int>(lengths[i])) {
            detail = "包 " + std::to_string(i) + " 实际长度 " +
                     std::to_string(result.packet_actual[i]) + "，期望 " +
                     std::to_string(lengths[i]);
            return false;
        }
        if (!iso::verify_packet(result.buffer.data() + offset, lengths[i])) {
            detail = "包 " + std::to_string(i) + " 内容校验失败";
            return false;
        }
        actual_total += static_cast<std::uint32_t>(result.packet_actual[i]);
        offset += lengths[i];
    }
    if (actual_total != offset) {
        detail = "逐包实际长度之和 " + std::to_string(actual_total) + "，期望 " +
                 std::to_string(offset);
        return false;
    }
    return true;
}

/**
 * @brief 校验 OUT 传输：整体状态、逐包状态/长度（内容由设备侧校验）
 *
 * 与 IN 一样，libusb_transfer::actual_length 对等时传输无效，只看逐包长度。
 */
bool check_out_result(const IsoResult &result, const std::vector<std::uint32_t> &lengths,
                      std::string &detail) {
    if (result.status != LIBUSB_TRANSFER_COMPLETED) {
        detail = "传输状态 " + transfer_status_string(result.status) + "（等待完成失败）";
        return false;
    }
    std::uint32_t actual_total = 0;
    for (std::size_t i = 0; i < lengths.size(); ++i) {
        if (result.packet_status[i] != LIBUSB_TRANSFER_COMPLETED) {
            detail = "包 " + std::to_string(i) + " 状态 " +
                     transfer_status_string(result.packet_status[i]);
            return false;
        }
        if (result.packet_actual[i] != static_cast<int>(lengths[i])) {
            detail = "包 " + std::to_string(i) + " 实际长度 " +
                     std::to_string(result.packet_actual[i]) + "，期望 " +
                     std::to_string(lengths[i]);
            return false;
        }
        actual_total += static_cast<std::uint32_t>(result.packet_actual[i]);
    }
    std::uint32_t expected_total = 0;
    for (std::uint32_t length: lengths) {
        expected_total += length;
    }
    if (actual_total != expected_total) {
        detail = "逐包实际长度之和 " + std::to_string(actual_total) + "，期望 " +
                 std::to_string(expected_total);
        return false;
    }
    return true;
}

/**
 * @brief 跑一条 IN 用例：提交、等待、校验、释放
 */
void run_in_case(libusb_context *context, libusb_device_handle *handle, TestRunner &runner,
                 std::uint8_t endpoint, const std::vector<std::uint32_t> &lengths,
                 const std::string &name) {
    IsoResult result;
    auto *transfer = make_iso_transfer(handle, endpoint, lengths, result, io_timeout_ms);
    if (transfer == nullptr) {
        runner.check(false, name, "分配 transfer 失败");
        return;
    }
    const int rc = libusb_submit_transfer(transfer);
    std::string detail;
    bool ok = false;
    if (rc != 0) {
        detail = "提交失败: " + libusb_error_string(rc);
    }
    else {
        result.submitted = true;
        wait_result(context, result, io_timeout_ms);
        ok = check_in_result(result, lengths, detail);
    }
    runner.check(ok, name, detail);
    cancel_and_free(context, transfer, result, io_timeout_ms);
}

/**
 * @brief 跑一条 OUT 用例：按包里序号填充、提交、等待、校验
 *
 * 包序号用调用方给出的起点，返回下一个可用序号（失败时不推进，避免后续
 * 用例的包序号出现无法解释的跳跃）。
 */
std::uint32_t run_out_case(libusb_context *context, libusb_device_handle *handle, TestRunner &runner,
                           std::uint8_t endpoint, const std::vector<std::uint32_t> &lengths,
                           std::uint32_t first_index, const std::string &name) {
    IsoResult result;
    auto *transfer = make_iso_transfer(handle, endpoint, lengths, result, io_timeout_ms);
    if (transfer == nullptr) {
        runner.check(false, name, "分配 transfer 失败");
        return first_index;
    }
    // OUT 数据布局与主机 buffer 一致：包按槽位长度紧凑排列，槽位内写包序号
    std::uint32_t offset = 0;
    for (std::size_t i = 0; i < lengths.size(); ++i) {
        iso::fill_packet(result.buffer.data() + offset, lengths[i], first_index + static_cast<std::uint32_t>(i));
        offset += lengths[i];
    }

    const int rc = libusb_submit_transfer(transfer);
    std::string detail;
    bool ok = false;
    std::uint32_t next_index = first_index;
    if (rc != 0) {
        detail = "提交失败: " + libusb_error_string(rc);
    }
    else {
        result.submitted = true;
        wait_result(context, result, io_timeout_ms);
        ok = check_out_result(result, lengths, detail);
        if (ok) {
            next_index += static_cast<std::uint32_t>(lengths.size());
        }
    }
    runner.check(ok, name, detail);
    cancel_and_free(context, transfer, result, io_timeout_ms);
    return next_index;
}

struct EndpointSet {
    std::uint8_t in = 0;
    std::uint8_t out = 0;
};

/**
 * @brief 从配置描述符校验设备结构并取出各 alt 的端点
 */
bool load_layout(libusb_device_handle *handle, std::array<EndpointSet, 3> &endpoints,
                 std::string &detail) {
    libusb_config_descriptor *config = nullptr;
    if (libusb_get_active_config_descriptor(libusb_get_device(handle), &config) != 0) {
        detail = "读取活动配置描述符失败";
        return false;
    }
    auto fail = [&](std::string message) {
        detail = std::move(message);
        libusb_free_config_descriptor(config);
        return false;
    };

    if (config->bNumInterfaces != 1) {
        return fail("接口数量 " + std::to_string(config->bNumInterfaces) + "，期望 1");
    }
    const auto &interface = config->interface[0];
    if (interface.num_altsetting != 3) {
        return fail("alt 数量 " + std::to_string(interface.num_altsetting) + "，期望 3");
    }
    if (interface.altsetting[0].bNumEndpoints != 0) {
        return fail("alt 0 端点数量 " + std::to_string(interface.altsetting[0].bNumEndpoints) +
                    "，期望 0");
    }
    if (interface.altsetting[1].bNumEndpoints != 2) {
        return fail("alt 1 端点数量 " + std::to_string(interface.altsetting[1].bNumEndpoints) +
                    "，期望 2");
    }
    if (interface.altsetting[2].bNumEndpoints != 1) {
        return fail("alt 2 端点数量 " + std::to_string(interface.altsetting[2].bNumEndpoints) +
                    "，期望 1");
    }

    for (const auto &endpoint: {interface.altsetting[1].endpoint[0], interface.altsetting[1].endpoint[1]}) {
        if ((endpoint.bmAttributes & 0x03) != LIBUSB_TRANSFER_TYPE_ISOCHRONOUS) {
            return fail("alt 1 端点类型不是 iso");
        }
        if (endpoint.wMaxPacketSize != max_packet_alt1) {
            return fail("alt 1 端点包大小 " + std::to_string(endpoint.wMaxPacketSize) + "，期望 " +
                        std::to_string(max_packet_alt1));
        }
        if (endpoint.bInterval != 1) {
            return fail("alt 1 端点 bInterval " + std::to_string(endpoint.bInterval) + "，期望 1");
        }
        if ((endpoint.bEndpointAddress & LIBUSB_ENDPOINT_IN) != 0) {
            endpoints[1].in = endpoint.bEndpointAddress;
        }
        else {
            endpoints[1].out = endpoint.bEndpointAddress;
        }
    }
    if (endpoints[1].in == 0 || endpoints[1].out == 0) {
        return fail("alt 1 缺少 IN 或 OUT 端点");
    }

    const auto &alt2 = interface.altsetting[2].endpoint[0];
    if ((alt2.bmAttributes & 0x03) != LIBUSB_TRANSFER_TYPE_ISOCHRONOUS ||
        (alt2.bEndpointAddress & LIBUSB_ENDPOINT_IN) == 0) {
        return fail("alt 2 端点不是 iso IN");
    }
    if (alt2.wMaxPacketSize != max_packet_alt2) {
        return fail("alt 2 端点包大小 " + std::to_string(alt2.wMaxPacketSize) + "，期望 " +
                    std::to_string(max_packet_alt2));
    }
    if (alt2.bInterval != 2) {
        return fail("alt 2 端点 bInterval " + std::to_string(alt2.bInterval) + "，期望 2");
    }
    endpoints[2].in = alt2.bEndpointAddress;

    libusb_free_config_descriptor(config);
    return true;
}

void run_cancel_case(libusb_context *context, libusb_device_handle *handle, TestRunner &runner,
                     std::uint8_t endpoint) {
    if (!set_in_delay(handle, 500)) {
        runner.check(false, "iso IN 取消", "设置设备 IN 延迟失败");
        return;
    }

    IsoResult result;
    auto *transfer = make_iso_transfer(handle, endpoint, std::vector<std::uint32_t>(8, 1024), result,
                                       io_timeout_ms);
    if (transfer == nullptr || libusb_submit_transfer(transfer) != 0) {
        runner.check(false, "iso IN 取消", "提交传输失败");
        if (transfer != nullptr) {
            libusb_free_transfer(transfer);
        }
        set_in_delay(handle, 0);
        return;
    }
    result.submitted = true;
    // 设备延迟 500ms 才服务，这里稍等后取消，确保 URB 仍在设备侧挂起
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const int cancel_rc = libusb_cancel_transfer(transfer);
    const bool finished = wait_result(context, result, 3000);
    const bool ok = cancel_rc == 0 && finished && result.status == LIBUSB_TRANSFER_CANCELLED;
    std::string detail;
    if (!ok) {
        detail = "cancel=" + libusb_error_string(cancel_rc) + " 完成=" + (finished ? "是" : "否") +
                 " 状态=" + transfer_status_string(result.status);
    }
    runner.check(ok, "iso IN 取消（挂起中 UNLINK）", detail);
    cancel_and_free(context, transfer, result, 3000);
    set_in_delay(handle, 0);
}

/**
 * @brief 验证 alt 0（零带宽）上提交 iso 会被拒绝
 *
 * 端点不在当前 alt 时，错误可能在提交阶段直接返回，也可能提交后在完成
 * 回调里以错误状态出现；两种都算通过。真正要防的是“提交成功且数据正常
 * 完成”——那说明 alt 切换没有生效。
 */
void run_alt0_rejection_case(libusb_context *context, libusb_device_handle *handle, TestRunner &runner,
                             std::uint8_t endpoint) {
    IsoResult result;
    auto *transfer = make_iso_transfer(handle, endpoint, std::vector<std::uint32_t>{1024}, result, 1000);
    if (transfer == nullptr) {
        runner.check(false, "alt 0 上 iso 提交被拒绝", "分配 transfer 失败");
        return;
    }
    const int rc = libusb_submit_transfer(transfer);
    bool ok = false;
    std::string detail;
    if (rc != 0) {
        ok = true;
        libusb_free_transfer(transfer);
    }
    else {
        result.submitted = true;
        wait_result(context, result, 1000);
        ok = result.done && result.status != LIBUSB_TRANSFER_COMPLETED;
        if (!ok) {
            detail = result.done ? "传输意外成功完成" : "传输既未完成也未被拒绝";
        }
        cancel_and_free(context, transfer, result, 1000);
    }
    runner.check(ok, "alt 0 上 iso 提交被拒绝", detail);
}

/**
 * @brief 校验“部分包失败”的逐包状态与长度
 *
 * 设备让指定槽位返回 EPIPE、actual_length=0；其余包必须正常完成。
 * 整体传输状态仍应是 COMPLETED（ISO 的包级错误不改变 URB 状态）。
 * @param verify_data 为 true 时（IN 方向）还校验未失败包的内容
 */
bool check_partial_failure(const IsoResult &result, const std::vector<std::uint32_t> &lengths,
                           std::size_t failed_slot, bool verify_data, std::string &detail) {
    if (result.status != LIBUSB_TRANSFER_COMPLETED) {
        detail = "整体状态 " + transfer_status_string(result.status);
        return false;
    }
    std::uint32_t offset = 0;
    for (std::size_t i = 0; i < lengths.size(); ++i) {
        if (i == failed_slot) {
            if (result.packet_status[i] == LIBUSB_TRANSFER_COMPLETED) {
                detail = "第 " + std::to_string(i) + " 包被报成成功";
                return false;
            }
            if (result.packet_actual[i] != 0) {
                detail = "第 " + std::to_string(i) + " 包 actual_length=" +
                         std::to_string(result.packet_actual[i]) + "，期望 0";
                return false;
            }
        }
        else {
            if (result.packet_status[i] != LIBUSB_TRANSFER_COMPLETED) {
                detail = "第 " + std::to_string(i) + " 包状态 " +
                         transfer_status_string(result.packet_status[i]);
                return false;
            }
            if (result.packet_actual[i] != static_cast<int>(lengths[i])) {
                detail = "第 " + std::to_string(i) + " 包 actual_length=" +
                         std::to_string(result.packet_actual[i]) + "，期望 " +
                         std::to_string(lengths[i]);
                return false;
            }
            if (verify_data && !iso::verify_packet(result.buffer.data() + offset, lengths[i])) {
                detail = "第 " + std::to_string(i) + " 包内容不匹配";
                return false;
            }
        }
        offset += lengths[i];
    }
    return true;
}

/**
 * @brief 部分包失败：逐包状态/长度、header actual_length 自洽与设备侧统计
 *
 * 服务端 header 的 actual_length 必须等于各包 actual_length 之和，否则 vhci
 * 会断开整台设备；这里同时覆盖 IN（数据回传）和 OUT（设备侧统计）两个方向。
 */
void run_packet_failure_cases(libusb_context *context, libusb_device_handle *handle, TestRunner &runner,
                              const std::array<EndpointSet, 3> &endpoints) {
    const std::vector<std::uint32_t> lengths(4, 1024);
    constexpr std::size_t failed_slot = 1;

    // IN：第 1 包失败
    {
        IsoResult result;
        auto *transfer = make_iso_transfer(handle, endpoints[1].in, lengths, result, io_timeout_ms);
        bool ok = false;
        std::string detail;
        if (transfer == nullptr) {
            detail = "分配 transfer 失败";
        }
        else if (!set_fail_mask(handle, 1u << failed_slot)) {
            detail = "设置失败掩码失败";
        }
        else if (libusb_submit_transfer(transfer) != 0) {
            detail = "提交失败";
        }
        else {
            result.submitted = true;
            if (!wait_result(context, result, io_timeout_ms)) {
                detail = "传输未完成";
            }
            else {
                ok = check_partial_failure(result, lengths, failed_slot, true, detail);
            }
        }
        runner.check(ok, "iso IN 部分包失败（逐包状态/长度）", detail);
        if (transfer != nullptr) {
            cancel_and_free(context, transfer, result, io_timeout_ms);
        }
        set_fail_mask(handle, 0);
    }

    // OUT：第 2 包失败，其余包的数据设备侧仍要收到
    {
        constexpr std::size_t out_failed_slot = 2;
        runner.check(reset_iso_stats(handle), "部分失败前清零设备侧统计");
        IsoResult result;
        auto *transfer = make_iso_transfer(handle, endpoints[1].out, lengths, result, io_timeout_ms);
        bool ok = false;
        std::string detail;
        if (transfer == nullptr) {
            detail = "分配 transfer 失败";
        }
        else {
            std::uint32_t offset = 0;
            for (std::size_t i = 0; i < lengths.size(); ++i) {
                iso::fill_packet(result.buffer.data() + offset, lengths[i],
                                 static_cast<std::uint32_t>(1000 + i));
                offset += lengths[i];
            }
            if (!set_fail_mask(handle, 1u << out_failed_slot)) {
                detail = "设置失败掩码失败";
            }
            else if (libusb_submit_transfer(transfer) != 0) {
                detail = "提交失败";
            }
            else {
                result.submitted = true;
                if (!wait_result(context, result, io_timeout_ms)) {
                    detail = "传输未完成";
                }
                else if (check_partial_failure(result, lengths, out_failed_slot, false, detail)) {
                    proto::IsoStats stats{};
                    const bool stats_ok = get_iso_stats(handle, stats);
                    if (!stats_ok) {
                        detail = "读取设备统计失败";
                    }
                    else if (stats.out_packets != 4 || stats.out_bytes != 3 * 1024 || stats.out_errors != 1) {
                        detail = "统计 packets=" + std::to_string(stats.out_packets) + " bytes=" +
                                 std::to_string(stats.out_bytes) + " errors=" +
                                 std::to_string(stats.out_errors) + "，期望 4/3072/1";
                    }
                    else {
                        ok = true;
                    }
                }
            }
        }
        runner.check(ok, "iso OUT 部分包失败（逐包状态 + 设备统计）", detail);
        if (transfer != nullptr) {
            cancel_and_free(context, transfer, result, io_timeout_ms);
        }
        set_fail_mask(handle, 0);
    }

    // 失败用例之后恢复正常传输（掩码已清零）
    run_in_case(context, handle, runner, endpoints[1].in,
                std::vector<std::uint32_t>(2, 1024), "部分失败后 IN 恢复正常");
}

void run_tests(libusb_context *context, libusb_device_handle *handle, TestRunner &runner) {
    libusb_device_descriptor descriptor{};
    libusb_get_device_descriptor(libusb_get_device(handle), &descriptor);
    runner.check(descriptor.idVendor == proto::vid && descriptor.idProduct == proto::pid_iso,
                 "设备描述符 VID:PID",
                 "实际 " + std::to_string(descriptor.idVendor) + ":" +
                         std::to_string(descriptor.idProduct));
    runner.check(descriptor.bcdUSB == 0x0200, "bcdUSB 为 2.00",
                 "实际 " + std::to_string(descriptor.bcdUSB));
    runner.check(libusb_get_device_speed(libusb_get_device(handle)) == LIBUSB_SPEED_HIGH,
                 "设备速度 high speed");

    std::array<EndpointSet, 3> endpoints{};
    std::string detail;
    const bool layout_ok = load_layout(handle, endpoints, detail);
    runner.check(layout_ok, "配置描述符布局（alt 0/1/2、iso 端点）", detail);

    if (!layout_ok) {
        // 端点信息不可用，后面的数据面用例没有意义
        return;
    }
    std::printf("端点：alt1 IN=0x%02x OUT=0x%02x，alt2 IN=0x%02x\n", endpoints[1].in,
                endpoints[1].out, endpoints[2].in);

    const int claim_rc = libusb_claim_interface(handle, 0);
    runner.check(claim_rc == 0, "claim 接口 0", libusb_error_string(claim_rc));
    if (claim_rc != 0) {
        return;
    }

    runner.check(get_current_alt(handle) == 0, "初始 alt 为 0");

    int rc = libusb_set_interface_alt_setting(handle, 0, 1);
    runner.check(rc == 0, "SET_INTERFACE alt 1", libusb_error_string(rc));
    runner.check(get_current_alt(handle) == 1, "GET_INTERFACE 返回 alt 1");
    if (rc != 0) {
        libusb_release_interface(handle, 0);
        return;
    }

    // ---- IN 方向 ----
    run_in_case(context, handle, runner, endpoints[1].in,
                std::vector<std::uint32_t>(8, 1024), "iso IN 8 包 × 1024");
    run_in_case(context, handle, runner, endpoints[1].in,
                {1024, 0, 512, 1, 100, 7, 1024, 33}, "iso IN 变长包（含零长度包）");
    run_in_case(context, handle, runner, endpoints[1].in,
                std::vector<std::uint32_t>(32, 1024), "iso IN 32 包 × 1024");
    run_in_case(context, handle, runner, endpoints[1].in,
                std::vector<std::uint32_t>(64, 256), "iso IN 64 包 × 256");

    // 并发 4 条不同包数/包长的 IN 传输
    {
        const std::vector<std::vector<std::uint32_t>> specs = {
                std::vector<std::uint32_t>(4, 512),
                std::vector<std::uint32_t>(8, 128),
                std::vector<std::uint32_t>(2, 1024),
                std::vector<std::uint32_t>(16, 64),
        };
        std::vector<std::unique_ptr<IsoResult>> results;
        std::vector<libusb_transfer *> transfers;
        bool submitted = true;
        for (const auto &spec: specs) {
            auto result = std::make_unique<IsoResult>();
            auto *transfer = make_iso_transfer(handle, endpoints[1].in, spec, *result, io_timeout_ms);
            if (transfer == nullptr || libusb_submit_transfer(transfer) != 0) {
                submitted = false;
                if (transfer != nullptr) {
                    libusb_free_transfer(transfer);
                }
                break;
            }
            result->submitted = true;
            results.push_back(std::move(result));
            transfers.push_back(transfer);
        }

        bool all_done = submitted;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(io_timeout_ms);
        while (all_done && std::chrono::steady_clock::now() < deadline) {
            bool pending = false;
            for (const auto &result: results) {
                pending = pending || !result->done;
            }
            if (!pending) {
                break;
            }
            timeval timeout{};
            timeout.tv_sec = 0;
            timeout.tv_usec = 20000;
            libusb_handle_events_timeout_completed(context, &timeout, nullptr);
        }
        for (const auto &result: results) {
            all_done = all_done && result->done;
        }

        bool all_ok = submitted && all_done;
        std::string concurrent_detail;
        if (!submitted) {
            concurrent_detail = "提交失败";
        }
        else if (!all_done) {
            concurrent_detail = "有传输未完成";
        }
        else {
            for (std::size_t i = 0; i < results.size(); ++i) {
                std::string item_detail;
                if (!check_in_result(*results[i], specs[i], item_detail)) {
                    all_ok = false;
                    concurrent_detail = "第 " + std::to_string(i) + " 条: " + item_detail;
                    break;
                }
            }
        }
        runner.check(all_ok, "iso IN 并发 4 条", concurrent_detail);
        for (std::size_t i = 0; i < transfers.size(); ++i) {
            cancel_and_free(context, transfers[i], *results[i], io_timeout_ms);
        }
    }

    // ---- OUT 方向 ----
    runner.check(reset_iso_stats(handle), "清零设备侧 iso 统计");

    std::uint32_t out_index = 0;
    out_index = run_out_case(context, handle, runner, endpoints[1].out,
                             std::vector<std::uint32_t>(8, 1024), out_index, "iso OUT 8 包 × 1024");
    out_index = run_out_case(context, handle, runner, endpoints[1].out,
                             {100, 0, 512, 1, 1024, 64}, out_index, "iso OUT 变长包（含零长度包）");
    out_index = run_out_case(context, handle, runner, endpoints[1].out,
                             std::vector<std::uint32_t>(32, 1024), out_index, "iso OUT 32 包 × 1024");

    proto::IsoStats stats{};
    const bool stats_ok = get_iso_stats(handle, stats);
    std::uint32_t expected_packets = 8 + 6 + 32;
    std::uint32_t expected_bytes = 8 * 1024 + (100 + 0 + 512 + 1 + 1024 + 64) + 32 * 1024;
    runner.check(stats_ok && stats.out_packets == expected_packets && stats.out_bytes == expected_bytes &&
                         stats.out_errors == 0,
                 "设备侧 OUT 统计（包数/字节/校验错误）",
                 stats_ok ? "packets=" + std::to_string(stats.out_packets) + " bytes=" +
                                    std::to_string(stats.out_bytes) + " errors=" +
                                    std::to_string(stats.out_errors)
                          : "读取统计失败");

    // OUT 之后 IN 仍正常（两个方向共用同一会话/端点表）
    run_in_case(context, handle, runner, endpoints[1].in,
                std::vector<std::uint32_t>(4, 1024), "iso OUT 之后 IN 仍正常");

    // 部分包失败：header actual_length 与逐包状态必须仍然自洽
    run_packet_failure_cases(context, handle, runner, endpoints);

    // ---- vendor 控制请求 ----
    {
        std::vector<std::uint8_t> info(4);
        const int result = vendor_in(handle, static_cast<std::uint8_t>(proto::IsoRequest::get_info), 0, info);
        bool ok = result == 4;
        std::uint32_t version = 0;
        if (ok) {
            std::memcpy(&version, info.data(), sizeof(version));
            ok = version == proto::version;
        }
        runner.check(ok, "vendor get_info", "返回长度 " + std::to_string(result));
    }
    {
        std::uint8_t byte = 0;
        const int result = libusb_control_transfer(handle, proto::iso_request_type_in, 0x7e, 0, 0, &byte,
                                                   sizeof(byte), 1000);
        runner.check(result == LIBUSB_ERROR_PIPE, "未知 vendor 请求被 STALL",
                     "返回 " + libusb_error_string(result));
    }

    // ---- alt 2：换一组端点 ----
    rc = libusb_set_interface_alt_setting(handle, 0, 2);
    runner.check(rc == 0, "SET_INTERFACE alt 2", libusb_error_string(rc));
    runner.check(get_current_alt(handle) == 2, "GET_INTERFACE 返回 alt 2");
    run_in_case(context, handle, runner, endpoints[2].in,
                std::vector<std::uint32_t>(4, 512), "alt 2 的 iso IN 4 包 × 512");

    rc = libusb_set_interface_alt_setting(handle, 0, 3);
    // 配置描述符里没有 alt 3 时 libusb 直接返回 NOT_FOUND（请求根本没发出去）；
    // 设备收到请求则回 STALL（PIPE）。两种都算正确拒绝
    runner.check(rc == LIBUSB_ERROR_PIPE || rc == LIBUSB_ERROR_NOT_FOUND,
                 "不存在的 alt 3 被拒绝", "返回 " + libusb_error_string(rc));
    runner.check(get_current_alt(handle) == 2, "失败的 SET_INTERFACE 不改变当前 alt");

    // ---- 回到 alt 1，反复切换 ----
    rc = libusb_set_interface_alt_setting(handle, 0, 1);
    runner.check(rc == 0, "切回 alt 1", libusb_error_string(rc));
    if (rc == 0) {
        run_in_case(context, handle, runner, endpoints[1].in,
                    std::vector<std::uint32_t>(8, 1024), "切回 alt 1 后 IN");
        out_index = run_out_case(context, handle, runner, endpoints[1].out,
                                 std::vector<std::uint32_t>(8, 1024), out_index, "切回 alt 1 后 OUT");
    }

    bool loop_ok = true;
    std::string loop_detail;
    for (int round = 0; round < 5 && loop_ok; ++round) {
        if (libusb_set_interface_alt_setting(handle, 0, 0) != 0) {
            loop_ok = false;
            loop_detail = "第 " + std::to_string(round) + " 轮切到 alt 0 失败";
            break;
        }
        if (get_current_alt(handle) != 0) {
            loop_ok = false;
            loop_detail = "第 " + std::to_string(round) + " 轮 alt 0 读取不一致";
            break;
        }
        // alt 0 没有端点：这里只验证切换本身，数据面提交的拒绝单独测
        if (libusb_set_interface_alt_setting(handle, 0, 1) != 0) {
            loop_ok = false;
            loop_detail = "第 " + std::to_string(round) + " 轮切回 alt 1 失败";
            break;
        }
        IsoResult result;
        auto *transfer = make_iso_transfer(handle, endpoints[1].in, std::vector<std::uint32_t>(2, 1024),
                                           result, io_timeout_ms);
        if (transfer == nullptr || libusb_submit_transfer(transfer) != 0) {
            loop_ok = false;
            loop_detail = "第 " + std::to_string(round) + " 轮提交失败";
            if (transfer != nullptr) {
                libusb_free_transfer(transfer);
            }
            break;
        }
        result.submitted = true;
        wait_result(context, result, io_timeout_ms);
        std::string item_detail;
        if (!check_in_result(result, std::vector<std::uint32_t>(2, 1024), item_detail)) {
            loop_ok = false;
            loop_detail = "第 " + std::to_string(round) + " 轮: " + item_detail;
        }
        cancel_and_free(context, transfer, result, io_timeout_ms);
    }
    runner.check(loop_ok, "alt 1↔alt 0 切换 5 轮", loop_detail);

    // alt 0 上提交 iso 应被拒绝（只做一次，单独一个用例）
    if (libusb_set_interface_alt_setting(handle, 0, 0) == 0) {
        run_alt0_rejection_case(context, handle, runner, endpoints[1].in);
    }
    else {
        runner.check(false, "alt 0 上 iso 提交被拒绝", "切到 alt 0 失败");
    }
    rc = libusb_set_interface_alt_setting(handle, 0, 1);
    runner.check(rc == 0, "取消用例前切回 alt 1", libusb_error_string(rc));

    // ---- 取消（UNLINK） ----
    run_cancel_case(context, handle, runner, endpoints[1].in);
    run_in_case(context, handle, runner, endpoints[1].in,
                std::vector<std::uint32_t>(4, 1024), "取消后 IN 恢复正常");

    libusb_release_interface(handle, 0);
}

}  // namespace

int main(int argc, char **argv) {
    const Args args = parse_args(argc, argv, proto::vid, proto::pid_iso);

    libusb_context *context = nullptr;
    if (libusb_init(&context) != 0) {
        std::fprintf(stderr, "libusb_init 失败\n");
        return 1;
    }

    libusb_device_handle *handle = nullptr;
    if (open_by_address(context, args, &handle) != 0) {
        libusb_exit(context);
        return 1;
    }

    TestRunner runner;
    run_tests(context, handle, runner);

    libusb_close(handle);
    libusb_exit(context);
    return runner.summary();
}
