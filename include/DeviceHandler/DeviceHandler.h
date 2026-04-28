#pragma once

#include <vector>
#include <cstdint>
#include <optional>
#include <system_error>
#include <mutex>
#include <spdlog/spdlog.h>

#include <asio.hpp>

#include "Device.h"
#include "type.h"
#include "protocol.h"


namespace usbipdcpp {
struct UsbEndpoint;
struct UsbInterface;
struct SetupPacket;

class Session;

class AbstDeviceHandler {
public:
    explicit AbstDeviceHandler(UsbDevice &handle_device) :
        handle_device(handle_device) {
    }

    AbstDeviceHandler(AbstDeviceHandler &&other) noexcept;

    /**
     * @brief 处理 URB 请求的统一入口
     * @param cmd 完整的 CMD_SUBMIT 命令
     * @param ep 端点信息
     * @param interface 可选的接口信息（非控制传输必须有）
     * @param ec 错误码
     */
    virtual void receive_urb(
            UsbIpCommand::UsbIpCmdSubmit cmd,
            UsbEndpoint ep,
            std::optional<UsbInterface> interface,
            usbipdcpp::error_code &ec
            ) = 0;

    /**
     * @brief 新的客户端连接时会调这个函数，可以阻塞。子类实现时请在函数开头调用这个函数
     * @param current_session 请自行储存通信用的session
     * @param ec 发生的ec
     */
    virtual void on_new_connection(Session &current_session, error_code &ec) {
        std::lock_guard lock(session_mutex_);
        session = &current_session;
    }

    /**
     * @brief 当发生错误等情况需要完全终止传输时会调用这个函数。被调用后禁止再提交消息和使用Session对象\n
     * 可以阻塞，处理所有需要处理的事务。子类实现时请在函数末尾调用这个函数
     */
    virtual void on_disconnection(error_code &ec) {
        std::lock_guard lock(session_mutex_);
        session = nullptr;
    }

# ifdef USBIPDCPP_ENABLE_BUSY_WAIT
    /**
     * @brief 检查是否还有传输在进行
     * @return true 表示还有传输未完成
     */
    virtual bool has_pending_transfers() const {
        return false;  // 默认实现
    }
# endif

    /**
     * @brief 检查设备是否已被移除
     * @return true 表示设备已物理拔出
     */
    virtual bool is_device_removed() const {
        return false;  // 默认实现
    }

    /**
     * @brief 设备被物理移除时调用
     */
    virtual void on_device_removed() {}

    /**
     * @brief 线程安全地停止 Session
     */
    void trigger_session_stop();

    /**
     * @brief 处理unlink。传入想要取消的序号和UNLINK命令的序号。
     * @param unlink_seqnum 想要取消的包序号
     * @param cmd_seqnum CMD_UNLINK 命令的序号（用于构造 RET_UNLINK）
     */
    virtual void handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) = 0;

    // ========== transfer_handle 操作接口 ==========

    /**
     * @brief 创建 transfer_handle
     * @param buffer_length 数据缓冲区长度
     * @param num_iso_packets 等时包数量
     * @param header
     * @param setup_packet
     * @return 创建的 transfer_handle
     */
    virtual void* alloc_transfer_handle(std::size_t buffer_length, int num_iso_packets, const UsbIpHeaderBasic& header, const SetupPacket& setup_packet);

    /**
     * @brief 获取 buffer 头指针
     * @param transfer_handle
     * @return buffer 头指针
     */
    virtual void* get_transfer_buffer(void* transfer_handle);

    /**
     * @brief 获取实际传输长度
     * @param transfer_handle
     * @return 实际长度
     */
    virtual std::size_t get_actual_length(void* transfer_handle);

    /**
     * @brief 获取读取数据时的偏移量
     * 用于 RET_SUBMIT 响应，控制传输需要跳过 setup 包（8字节）
     * @param transfer_handle
     * @return 偏移量
     */
    virtual std::size_t get_read_data_offset(void* transfer_handle);

    /**
     * @brief 获取写入数据时的偏移量
     * 用于 alloc_transfer_handle 时计算 buffer 位置，控制传输需要跳过 setup 包
     * @param header 用于判断是否为控制传输
     * @return 偏移量
     */
    virtual std::size_t get_write_data_offset(const UsbIpHeaderBasic& header);

    /**
     * @brief 获取等时包描述符
     * @param transfer_handle
     * @param index 索引
     * @return 描述符
     */
    virtual UsbIpIsoPacketDescriptor get_iso_descriptor(void* transfer_handle, int index);

    /**
     * @brief 设置等时包描述符
     * @param transfer_handle
     * @param index 索引
     * @param desc 描述符
     */
    virtual void set_iso_descriptor(void* transfer_handle, int index, const UsbIpIsoPacketDescriptor& desc);

    /**
     * @brief 释放 transfer_handle
     * @param transfer_handle
     */
    virtual void free_transfer_handle(void* transfer_handle);

    virtual ~AbstDeviceHandler() = default;

protected:
    UsbDevice &handle_device;
    Session *session = nullptr;
    mutable std::mutex session_mutex_;
};
}
