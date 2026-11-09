#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <libusb-1.0/libusb.h>

namespace usbipdcpp::e2e {

/**
 * @brief 命令行参数
 */
struct Args {
    int bus = -1;
    int address = -1;
    std::uint16_t vid = 0;
    std::uint16_t pid = 0;
};

/**
 * @brief 解析 --bus/--addr/--vid/--pid
 * @param argc 参数个数
 * @param argv 参数
 * @param default_vid 默认 VID
 * @param default_pid 默认 PID
 * @return 解析结果
 */
Args parse_args(int argc, char **argv, std::uint16_t default_vid, std::uint16_t default_pid);

/**
 * @brief 把 libusb 错误码转成可读字符串
 * @param code libusb 错误码
 * @return 错误名
 */
std::string libusb_error_string(int code);

/**
 * @brief 把端点地址格式化成 0xNN
 * @param address 端点地址
 * @return 形如 "0x81" 的字符串
 */
std::string hex_byte(std::uint8_t address);

/**
 * @brief 按总线号/设备地址打开设备
 * @param context libusb 上下文
 * @param args 命令行参数
 * @param handle 打开的句柄
 * @return 0 成功，非 0 失败
 */
int open_by_address(libusb_context *context, const Args &args, libusb_device_handle **handle);

/**
 * @brief 测试用例计数器
 */
class TestRunner {
public:
    /**
     * @brief 记录一个用例结果
     * @param ok 是否通过
     * @param name 用例名
     * @param detail 失败时的细节
     */
    void check(bool ok, const std::string &name, const std::string &detail = {});

    /**
     * @brief 打印汇总并返回退出码
     * @return 有失败返回 1，否则 0
     */
    int summary() const;

private:
    int passed = 0;
    int failed = 0;
};

/**
 * @brief 按 (seed + i) 填充递增模式
 * @param buffer 缓冲区
 * @param seed 种子
 */
void fill_increment(std::vector<std::uint8_t> &buffer, std::uint8_t seed);

/**
 * @brief 校验逐字节 +1 的连续性
 * @param data 数据
 * @param length 长度
 * @return 连续返回 true
 */
bool verify_increment(const std::uint8_t *data, std::size_t length);

/**
 * @brief 读小端 16 位
 */
std::uint16_t read_le16(const std::uint8_t *data);

/**
 * @brief 读小端 32 位
 */
std::uint32_t read_le32(const std::uint8_t *data);

/**
 * @brief 按配置描述符里指定接口/alt setting 的端点顺序取端点地址
 *
 * UDC 自动分配端点时可能改写端点地址，客户端不能假设就是设备侧
 * 描述符里请求的地址，必须以实际枚举出来的配置描述符为准。
 * @param handle 设备句柄
 * @param interface_number 接口号
 * @param alternate_setting alt setting 编号（bAlternateSetting）
 * @param addresses 输出端点地址（按描述符顺序）
 * @return 成功返回 true
 */
bool get_endpoint_addresses(libusb_device_handle *handle, int interface_number,
                            int alternate_setting, std::vector<std::uint8_t> &addresses);

/**
 * @brief 校验接口/alt setting 下所有端点的传输类型与最大包长
 *
 * 只查地址和数量发现不了描述符被写坏（例如 bulk 写成 interrupt、
 * wMaxPacketSize 错误），这类错误会让后续传输失败但不指向根因。
 * @param handle 设备句柄
 * @param interface_number 接口号
 * @param alternate_setting alt setting 编号
 * @param expected_type 期望的传输类型（LIBUSB_TRANSFER_TYPE_BULK / INTERRUPT）
 * @param expected_max_packet 期望的 wMaxPacketSize（低 11 位）
 * @param detail 失败时写入实际端点字段，供调用方打印
 * @return 全部端点匹配返回 true
 */
bool check_endpoint_descriptors(libusb_device_handle *handle, int interface_number,
                                int alternate_setting, int expected_type,
                                std::uint16_t expected_max_packet, std::string &detail);

/**
 * @brief 异步传输结果
 */
struct AsyncResult {
    std::vector<std::uint8_t> data;
    std::vector<std::uint8_t> setup;
    int status = LIBUSB_TRANSFER_ERROR;
    int actual_length = 0;
    bool done = false;
};

/**
 * @brief 提交一条异步 bulk/interrupt 传输
 * @param handle 设备句柄
 * @param endpoint 端点地址
 * @param length 传输长度
 * @param timeout 超时（毫秒）
 * @param interrupt 是否 interrupt 端点
 * @param result 结果对象（调用方负责在完成后释放 transfer）
 * @return libusb_transfer，失败返回 nullptr
 */
libusb_transfer *submit_async(libusb_device_handle *handle, std::uint8_t endpoint,
                              std::size_t length, int timeout, bool interrupt,
                              AsyncResult *result);

/**
 * @brief 提交一条异步控制传输
 * @param handle 设备句柄
 * @param request_type bmRequestType
 * @param request bRequest
 * @param value wValue
 * @param index wIndex
 * @param length wLength
 * @param timeout 超时（毫秒）
 * @param result 结果对象
 * @return libusb_transfer，失败返回 nullptr
 */
libusb_transfer *submit_async_control(libusb_device_handle *handle, std::uint8_t request_type,
                                      std::uint8_t request, std::uint16_t value,
                                      std::uint16_t index, std::size_t length, int timeout,
                                      AsyncResult *result);

/**
 * @brief 驱动事件循环直到异步传输完成或超时
 * @param context libusb 上下文
 * @param result 结果对象
 * @param timeout_ms 最长等待毫秒数
 * @return 实际完成返回 true
 */
bool wait_async(libusb_context *context, AsyncResult &result, int timeout_ms);

/**
 * @brief 释放一个可能仍在传输中的 transfer
 *
 * libusb 不允许直接 free 已提交且未完成的 transfer；这里先取消并等回调
 * 落地，等不到就放弃释放（进程即将退出，避免未定义行为）。
 * @param context libusb 上下文
 * @param transfer 要释放的 transfer（可为 nullptr）
 * @param result 对应的结果对象（可为 nullptr）
 * @param timeout_ms 每次等待毫秒数
 */
void cancel_and_free_transfer(libusb_context *context, libusb_transfer *transfer,
                              AsyncResult *result, int timeout_ms);

}  // namespace usbipdcpp::e2e
