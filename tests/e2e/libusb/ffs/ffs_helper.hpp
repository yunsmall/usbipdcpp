#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <sys/types.h>
#include <type_traits>
#include <vector>

#include <linux/usb/ch9.h>
#include <linux/usb/functionfs.h>

namespace usbipdcpp::e2e {

/**
 * @brief 一串 USB 描述符（只含接口和端点，设备描述符由 configfs 提供）
 */
struct DescriptorList {
    std::vector<std::uint8_t> bytes;
    std::uint32_t count = 0;

    /**
     * @brief 追加一个 packed 描述符结构
     * @param descriptor 描述符结构
     */
    template <typename T>
    void add(const T &descriptor) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto *data = reinterpret_cast<const std::uint8_t *>(&descriptor);
        bytes.insert(bytes.end(), data, data + sizeof(T));
        ++count;
    }
};

/**
 * @brief FunctionFS 设备的全速/高速描述符
 */
struct FfsDescriptors {
    DescriptorList full_speed;
    DescriptorList high_speed;
};

/**
 * @brief FunctionFS 设备守护进程的公共底座
 *
 * 负责打开 ep0、写描述符和字符串、读事件循环，以及 ep0 的数据阶段与 stall。
 * 设备行为由子类通过 make_descriptors/on_enable/on_disable/handle_setup 提供。
 *
 * FunctionFS 的关键约定（内核 f_fs.c）：
 * - IN 方向的控制请求用 send_ep0 回数据，OUT 方向用 recv_ep0 收数据，
 *   wLength 为 0 的请求用 complete_ep0_no_data 完成状态阶段；
 * - stall 是通过对 ep0 做反方向 IO 触发的；
 * - 端点 halt 同样是对端点文件做反方向 IO 触发的，清除用 FUNCTIONFS_CLEAR_HALT。
 */
class FfsDevice {
public:
    /**
     * @brief 构造设备
     * @param mount_dir functionfs 挂载目录（里面有 ep0）
     */
    explicit FfsDevice(std::string mount_dir);
    virtual ~FfsDevice();

    FfsDevice(const FfsDevice &) = delete;
    FfsDevice &operator=(const FfsDevice &) = delete;

    /**
     * @brief 写描述符和字符串，然后进入 ep0 事件循环
     * @return 进程退出码
     */
    int run();

protected:
    /**
     * @brief 构造设备的接口和端点描述符
     * @return 全速/高速两套描述符
     */
    virtual FfsDescriptors make_descriptors() const = 0;

    /**
     * @brief 主机配置设备后调用（此时端点已经可用）
     */
    virtual void on_enable() {}

    /**
     * @brief 主机取消配置设备时调用
     */
    virtual void on_disable() {}

    /**
     * @brief 处理一个控制请求
     *
     * 实现必须自己完成数据阶段或状态阶段，不能直接返回。
     * @param setup 控制请求
     */
    virtual void handle_setup(const usb_ctrlrequest &setup) = 0;

    /**
     * @brief 拼接 functionfs 挂载目录下的端点文件名
     * @param name 端点文件名（ep0/ep1/...）
     * @return 完整路径
     */
    std::string endpoint_path(const char *name) const;

    /**
     * @brief 打开端点文件
     * @param name 端点文件名（ep1/...）
     * @return 文件描述符，失败返回 -1
     */
    int open_endpoint(const char *name) const;

    /**
     * @brief 读取端点当前实际使用的端点描述符
     *
     * UDC 自动分配端点时可能把描述符里的地址改写成物理端点地址，
     * 用户态必须用这个 ioctl 拿到真实地址，不能假设就是描述符里写的。
     * @param fd 端点文件
     * @param descriptor 输出描述符
     * @return 成功返回 true
     */
    static bool endpoint_descriptor(int fd, usb_endpoint_descriptor &descriptor);

    /**
     * @brief 当前 SETUP 是否是 IN 方向
     */
    bool setup_is_in() const;

    /**
     * @brief 当前 SETUP 的 wLength（主机序）
     */
    std::uint16_t setup_length() const;

    /**
     * @brief 取控制请求的 wValue（主机序）
     */
    static std::uint16_t setup_value(const usb_ctrlrequest &setup);

    /**
     * @brief 取控制请求的 wIndex（主机序）
     */
    static std::uint16_t setup_index(const usb_ctrlrequest &setup);

    /**
     * @brief 向 ep0 写数据，完成控制 IN 的数据阶段
     * @param data 数据
     * @param length 长度
     */
    void send_ep0(const void *data, std::size_t length);

    /**
     * @brief 从 ep0 读数据，完成控制 OUT 的数据阶段
     * @param data 接收缓冲区
     * @param length 期望长度
     * @return 实际字节数，失败返回 -1
     */
    ssize_t recv_ep0(void *data, std::size_t length);

    /**
     * @brief 完成无数据控制请求的状态阶段
     */
    void complete_ep0_no_data();

    /**
     * @brief 让当前控制请求 stall
     */
    void stall_ep0();

    /**
     * @brief 往 stderr 打日志
     */
    static void log(const char *format, ...) __attribute__((format(printf, 1, 2)));

private:
    void write_descriptors();
    void write_strings();
    void handle_event(const usb_functionfs_event &event);

    std::string mount_dir;
    int ep0_fd = -1;
    usb_ctrlrequest pending_setup {};
};

}  // namespace usbipdcpp::e2e
