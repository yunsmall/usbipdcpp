#pragma once

#include "virtual_device/VirtualInterfaceHandler.h"
#include "SetupPacket.h"
#include "constant.h"
#include "CdcAcmConstants.h"
#include <deque>
#include <shared_mutex>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <string_view>

namespace usbipdcpp {

/**
 * @brief CDC ACM 线路编码结构
 */
struct LineCoding {
    std::uint32_t dwDTERate = 115200;  // 波特率
    std::uint8_t bCharFormat = 0;      // 停止位: 0=1位, 1=1.5位, 2=2位
    std::uint8_t bParityType = 0;      // 校验: 0=无, 1=奇, 2=偶, 3=标记, 4=空格
    std::uint8_t bDataBits = 8;        // 数据位: 5, 6, 7, 8, 16

    [[nodiscard]] std::vector<std::uint8_t> to_bytes() const {
        std::vector<std::uint8_t> result;
        result.push_back(dwDTERate & 0xFF);
        result.push_back((dwDTERate >> 8) & 0xFF);
        result.push_back((dwDTERate >> 16) & 0xFF);
        result.push_back((dwDTERate >> 24) & 0xFF);
        result.push_back(bCharFormat);
        result.push_back(bParityType);
        result.push_back(bDataBits);
        return result;
    }

    static LineCoding from_bytes(const std::vector<std::uint8_t> &data) {
        LineCoding coding{};
        if (data.size() >= 7) {
            coding.dwDTERate = data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
            coding.bCharFormat = data[4];
            coding.bParityType = data[5];
            coding.bDataBits = data[6];
        }
        return coding;
    }
};

/**
 * @brief CDC ACM 控制信号状态
 */
struct ControlSignalState {
    bool dtr = false;  // Data Terminal Ready
    bool rts = false;  // Request To Send

    [[nodiscard]] std::uint16_t to_uint16() const {
        std::uint16_t value = 0;
        if (dtr) value |= static_cast<std::uint16_t>(CdcAcmControlSignal::DTR);
        if (rts) value |= static_cast<std::uint16_t>(CdcAcmControlSignal::RTS);
        return value;
    }

    static ControlSignalState from_uint16(std::uint16_t value) {
        ControlSignalState state;
        state.dtr = (value & static_cast<std::uint16_t>(CdcAcmControlSignal::DTR)) != 0;
        state.rts = (value & static_cast<std::uint16_t>(CdcAcmControlSignal::RTS)) != 0;
        return state;
    }
};

/**
 * @brief CDC ACM 串口状态通知
 */
struct SerialStateNotification {
    std::uint8_t bmRequestType = 0xA1;  // 类特定、接口、IN
    std::uint8_t bNotification = 0x20;  // SERIAL_STATE
    std::uint16_t wValue = 0;
    std::uint16_t wIndex = 0;            // 接口号
    std::uint16_t wLength = 2;
    std::uint16_t data = 0;              // 状态位

    [[nodiscard]] std::vector<std::uint8_t> to_bytes() const {
        std::vector<std::uint8_t> result;
        result.push_back(bmRequestType);
        result.push_back(bNotification);
        result.push_back(wValue & 0xFF);
        result.push_back((wValue >> 8) & 0xFF);
        result.push_back(wIndex & 0xFF);
        result.push_back((wIndex >> 8) & 0xFF);
        result.push_back(wLength & 0xFF);
        result.push_back((wLength >> 8) & 0xFF);
        result.push_back(data & 0xFF);
        result.push_back((data >> 8) & 0xFF);
        return result;
    }
};

// 前向声明
class CdcAcmDataInterfaceHandler;

/**
 * @brief CDC ACM 通信接口处理器（处理控制请求和状态通知）
 *
 * 用于处理 CDC ACM 设备的通信接口，响应控制请求并发送状态通知。
 */
class CdcAcmCommunicationInterfaceHandler : public VirtualInterfaceHandler {
public:
    CdcAcmCommunicationInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool);

    void handle_non_standard_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                       std::uint32_t transfer_flags,
                                                       std::uint32_t transfer_buffer_length,
                                                       const SetupPacket &setup_packet,
                                                       const data_type &out_data, std::error_code &ec) override;

    void handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                   std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
                                   data_type &&out_data, std::error_code &ec) override;

    [[nodiscard]] data_type get_class_specific_descriptor() override;

    // 标准请求处理（已提供默认实现，子类可选择性重写）
    void request_clear_feature(std::uint16_t feature_selector, std::uint32_t *p_status) override;
    void request_endpoint_clear_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                        std::uint32_t *p_status) override;
    std::uint8_t request_get_interface(std::uint32_t *p_status) override;
    void request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) override;
    std::uint16_t request_get_status(std::uint32_t *p_status) override;
    std::uint16_t request_endpoint_get_status(std::uint8_t ep_address, std::uint32_t *p_status) override;
    void request_set_feature(std::uint16_t feature_selector, std::uint32_t *p_status) override;
    void request_endpoint_set_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                      std::uint32_t *p_status) override;

    // CDC ACM 特有的虚拟函数，子类可重写
    virtual void on_set_line_coding(const LineCoding &coding);
    virtual void on_set_control_line_state(const ControlSignalState &state);
    virtual void on_send_break(std::uint16_t duration);

    // 处理非 CDC ACM 类请求的控制传输，子类可重写以扩展功能
    virtual void handle_non_cdc_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                          std::uint32_t transfer_flags,
                                                          std::uint32_t transfer_buffer_length,
                                                          const SetupPacket &setup_packet,
                                                          const data_type &out_data, std::error_code &ec);

    // 获取当前线路编码
    [[nodiscard]] const LineCoding& get_line_coding() const { return line_coding_; }
    [[nodiscard]] const ControlSignalState& get_control_signal_state() const { return control_signal_state_; }

    // 发送串口状态通知
    void send_serial_state_notification(std::uint16_t state_bits);

    // 关联数据接口处理器
    void set_data_handler(CdcAcmDataInterfaceHandler *handler) { data_handler_ = handler; }
    CdcAcmDataInterfaceHandler* get_data_handler() const { return data_handler_; }

protected:
    LineCoding line_coding_;
    ControlSignalState control_signal_state_;

    /**
     * @brief 中断传输队列
     */
    std::deque<std::uint32_t> interrupt_req_queue_;
    std::shared_mutex interrupt_req_queue_mutex_;

    /**
     * @brief 待发送的状态通知数据
     */
    std::vector<std::uint8_t> pending_notification_;
    std::mutex notification_mutex_;

    /**
     * @brief 关联的数据接口处理器
     */
    CdcAcmDataInterfaceHandler *data_handler_ = nullptr;
};

/**
 * @brief 环形缓冲区，预分配内存避免频繁动态分配
 */
class RingBuffer {
public:
    explicit RingBuffer(std::size_t capacity = 64 * 1024);

    /**
     * @brief 写入数据
     * @param data 数据指针
     * @param size 数据大小
     * @return 实际写入的字节数
     */
    std::size_t write(const std::uint8_t *data, std::size_t size);

    /**
     * @brief 读取数据
     * @param data 数据缓冲区
     * @param max_size 最大读取字节数
     * @return 实际读取的字节数
     */
    std::size_t read(std::uint8_t *data, std::size_t max_size);

    /**
     * @brief 查看数据（不移除）
     * @param data 数据缓冲区
     * @param max_size 最大查看字节数
     * @return 实际查看的字节数
     */
    std::size_t peek(std::uint8_t *data, std::size_t max_size) const;

    /**
     * @brief 获取当前数据量
     * @return 缓冲区中已使用字节数
     */
    [[nodiscard]] std::size_t size() const;

    /**
     * @brief 获取缓冲区容量
     * @return 缓冲区总容量
     */
    [[nodiscard]] std::size_t capacity() const;

    /**
     * @brief 获取缓冲区剩余空间
     * @return 缓冲区可用字节数
     */
    [[nodiscard]] std::size_t available() const;

    /**
     * @brief 检查缓冲区是否为空
     * @return true 表示为空
     */
    [[nodiscard]] bool empty() const;

    /**
     * @brief 检查缓冲区是否已满
     * @return true 表示已满
     */
    [[nodiscard]] bool full() const;

    /**
     * @brief 清空缓冲区
     */
    void clear();

    /**
     * @brief 调整缓冲区容量
     * @param new_capacity 新容量
     */
    void resize(std::size_t new_capacity);

private:
    std::vector<std::uint8_t> buffer_;
    std::size_t capacity_;
    std::size_t head_ = 0;
    std::size_t tail_ = 0;
    std::size_t count_ = 0;
};

/**
 * @brief CDC ACM 数据接口处理器（处理数据传输）
 *
 * 用于处理 CDC ACM 设备的数据接口，处理批量数据传输。
 */
class CdcAcmDataInterfaceHandler : public VirtualInterfaceHandler {
public:
    CdcAcmDataInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool);

    void handle_bulk_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                              std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
                              data_type &&out_data, std::error_code &ec) override;

    void handle_non_standard_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                       std::uint32_t transfer_flags,
                                                       std::uint32_t transfer_buffer_length,
                                                       const SetupPacket &setup_packet,
                                                       const data_type &out_data, std::error_code &ec) override;

    [[nodiscard]] data_type get_class_specific_descriptor() override;

    // 标准请求处理（已提供默认实现，子类可选择性重写）
    void request_clear_feature(std::uint16_t feature_selector, std::uint32_t *p_status) override;
    void request_endpoint_clear_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                        std::uint32_t *p_status) override;
    std::uint8_t request_get_interface(std::uint32_t *p_status) override;
    void request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) override;
    std::uint16_t request_get_status(std::uint32_t *p_status) override;
    std::uint16_t request_endpoint_get_status(std::uint8_t ep_address, std::uint32_t *p_status) override;
    void request_set_feature(std::uint16_t feature_selector, std::uint32_t *p_status) override;
    void request_endpoint_set_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                      std::uint32_t *p_status) override;

    // ===== 数据收发回调，子类可重写 =====

    /**
     * @brief 收到主机发送的数据时回调
     * @param data 接收到的数据
     */
    virtual void on_data_received(data_type &&data);

    /**
     * @brief 主机请求数据时回调，用于按需生成数据
     * @param length 主机请求的数据长度
     * @return 返回要发送的数据，如果返回空则等待 send_data 调用
     */
    virtual data_type on_data_requested(std::uint16_t length);

    /**
     * @brief 主机 RTS 状态变化时回调
     * @param rts RTS 状态，true=主机愿意接收数据
     */
    virtual void on_rts_changed(bool rts);

    // ===== 发送数据到主机 =====

    /**
     * @brief 非阻塞发送数据到主机
     * @param data 数据指针
     * @param size 数据大小
     * @return 实际写入缓冲区的字节数，缓冲区满时可能小于请求值
     */
    std::size_t send_data(const std::uint8_t *data, std::size_t size);
    std::size_t send_data(const data_type &data);
    std::size_t send_data(data_type &&data);
    std::size_t send_data(std::string_view data);

    /**
     * @brief 阻塞发送数据到主机，等待缓冲区有空间
     * @param data 数据指针
     * @param size 数据大小
     * @param timeout_ms 超时时间（毫秒），0 表示无限等待
     * @return 实际写入缓冲区的字节数，超时时可能小于请求值
     */
    std::size_t send_data_blocking(const std::uint8_t *data, std::size_t size,
                                   std::uint32_t timeout_ms = 0);
    std::size_t send_data_blocking(const data_type &data, std::uint32_t timeout_ms = 0);
    std::size_t send_data_blocking(data_type &&data, std::uint32_t timeout_ms = 0);
    std::size_t send_data_blocking(std::string_view data, std::uint32_t timeout_ms = 0);

    // ===== 缓冲区配置 =====

    /**
     * @brief 设置 TX 缓冲区容量
     * @param capacity 缓冲区大小（字节）
     */
    void set_tx_buffer_capacity(std::size_t capacity);

    /**
     * @brief 设置 TX 水位线
     * @param high 高水位线，缓冲区超过此值时建议触发流控
     * @param low 低水位线，缓冲区低于此值时建议恢复发送
     */
    void set_tx_watermarks(std::size_t high, std::size_t low);

    /**
     * @brief 获取 TX 缓冲区当前数据量
     * @return 缓冲区中已使用字节数
     */
    [[nodiscard]] std::size_t get_tx_buffer_size() const;

    /**
     * @brief 获取 TX 缓冲区剩余空间
     * @return 缓冲区可用字节数
     */
    [[nodiscard]] std::size_t get_tx_buffer_available() const;

    // ===== 流控状态 =====

    /**
     * @brief 设置 CTS 状态通知主机
     * @param cts CTS 状态，true=设备可以接收数据
     */
    void set_cts(bool cts);

    /**
     * @brief 获取当前 RTS 状态（来自主机）
     * @return RTS 状态，true=主机愿意接收数据
     */
    [[nodiscard]] bool get_rts() const;

    /**
     * @brief 关联通信接口处理器
     * @param handler 通信接口处理器指针
     */
    void set_comm_handler(CdcAcmCommunicationInterfaceHandler *handler);

protected:
    /**
     * @brief TX 缓冲区（设备→主机）
     */
    RingBuffer tx_buffer_;

    std::size_t tx_high_watermark_ = 48 * 1024;
    std::size_t tx_low_watermark_ = 16 * 1024;

    /**
     * @brief 批量 IN 请求队列
     */
    struct BulkInRequest {
        std::uint32_t seqnum;
        std::uint32_t length;
    };
    std::deque<BulkInRequest> bulk_in_req_queue_;

    /**
     * @brief 保护 tx_buffer_ 和 bulk_in_req_queue_ 的互斥锁
     * @note 这两个资源需要协同操作，用同一个锁避免嵌套锁问题
     */
    mutable std::mutex tx_mutex_;

    /**
     * @brief 条件变量，用于阻塞发送时等待缓冲区有空间
     */
    std::condition_variable tx_cv_;

    /**
     * @brief 关联的通信接口处理器
     */
    CdcAcmCommunicationInterfaceHandler *comm_handler_ = nullptr;

    /**
     * @brief 从 TX 缓冲区读取数据并发送
     * @param seqnum 请求序号
     * @param max_length 最大发送长度
     * @return 是否发送了数据
     * @note 调用者必须已持有 tx_mutex_
     */
    bool send_from_tx_buffer_locked(std::uint32_t seqnum, std::uint32_t max_length);

    /**
     * @brief 尝试发送等待的数据
     * @note 调用者必须已持有 tx_mutex_
     */
    void try_send_pending_locked();
};

}
