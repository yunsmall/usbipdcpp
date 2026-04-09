#pragma once

#include "InterfaceHandler/VirtualInterfaceHandler.h"
#include "SetupPacket.h"
#include "constant.h"
#include "CdcAcmConstants.h"
#include <deque>
#include <shared_mutex>

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
                                   const data_type &out_data, std::error_code &ec) override;

    [[nodiscard]] data_type get_class_specific_descriptor() override;

    // 纯虚函数，子类必须实现
    void request_clear_feature(std::uint16_t feature_selector, std::uint32_t *p_status) override = 0;
    void request_endpoint_clear_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                        std::uint32_t *p_status) override = 0;
    std::uint8_t request_get_interface(std::uint32_t *p_status) override = 0;
    void request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) override = 0;
    std::uint16_t request_get_status(std::uint32_t *p_status) override = 0;
    std::uint16_t request_endpoint_get_status(std::uint8_t ep_address, std::uint32_t *p_status) override = 0;
    void request_set_feature(std::uint16_t feature_selector, std::uint32_t *p_status) override = 0;
    void request_endpoint_set_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                      std::uint32_t *p_status) override = 0;

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

protected:
    LineCoding line_coding_;
    ControlSignalState control_signal_state_;

    // 中断传输队列
    std::deque<std::uint32_t> interrupt_req_queue_;
    std::shared_mutex interrupt_req_queue_mutex_;

    // 待发送的状态通知数据
    std::vector<std::uint8_t> pending_notification_;
    std::mutex notification_mutex_;
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
                              const data_type &out_data, std::error_code &ec) override;

    void handle_non_standard_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                       std::uint32_t transfer_flags,
                                                       std::uint32_t transfer_buffer_length,
                                                       const SetupPacket &setup_packet,
                                                       const data_type &out_data, std::error_code &ec) override;

    [[nodiscard]] data_type get_class_specific_descriptor() override;

    // 纯虚函数
    void request_clear_feature(std::uint16_t feature_selector, std::uint32_t *p_status) override = 0;
    void request_endpoint_clear_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                        std::uint32_t *p_status) override = 0;
    std::uint8_t request_get_interface(std::uint32_t *p_status) override = 0;
    void request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) override = 0;
    std::uint16_t request_get_status(std::uint32_t *p_status) override = 0;
    std::uint16_t request_endpoint_get_status(std::uint8_t ep_address, std::uint32_t *p_status) override = 0;
    void request_set_feature(std::uint16_t feature_selector, std::uint32_t *p_status) override = 0;
    void request_endpoint_set_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                      std::uint32_t *p_status) override = 0;

    // 数据收发回调，子类可重写
    virtual void on_data_received(const data_type &data);
    virtual data_type on_data_requested(std::uint16_t length);

    // 发送数据到主机
    void send_data(const data_type &data);
    void send_data(data_type &&data);

protected:
    // 批量 IN 传输队列
    std::deque<std::uint32_t> bulk_in_req_queue_;
    std::shared_mutex bulk_in_req_queue_mutex_;

    // 待发送的数据
    std::deque<data_type> pending_tx_data_;
    std::mutex tx_data_mutex_;
};

}
