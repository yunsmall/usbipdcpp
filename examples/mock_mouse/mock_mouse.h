#pragma once

#include <thread>
#include <mutex>
#include <condition_variable>

#include "DeviceHandler/SimpleVirtualDeviceHandler.h"
#include "InterfaceHandler/HidVirtualInterfaceHandler.h"
#include "Server.h"
#include "Session.h"


class MockMouseInterfaceHandler : public usbipdcpp::HidVirtualInterfaceHandler {
public:
    MockMouseInterfaceHandler(usbipdcpp::UsbInterface &handle_interface, usbipdcpp::StringPool &string_pool);

    void handle_interrupt_transfer(std::uint32_t seqnum, const usbipdcpp::UsbEndpoint &ep,
                                   std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
                                   const usbipdcpp::data_type &out_data,
                                   std::error_code &ec) override;
    void on_new_connection(usbipdcpp::Session &current_session, usbipdcpp::error_code &ec) override;
    void on_disconnection(usbipdcpp::error_code &ec) override;
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

    std::uint16_t get_report_descriptor_size() override;

    usbipdcpp::data_type get_report_descriptor() override;


    void handle_non_hid_request_type_control_urb(std::uint32_t seqnum, const usbipdcpp::UsbEndpoint &ep,
                                                 std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
                                                 const usbipdcpp::SetupPacket &setup_packet,
                                                 const usbipdcpp::data_type &out_data, std::error_code &ec) override;
    usbipdcpp::data_type request_get_report(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                                 std::uint32_t *p_status) override;
    void request_set_report(std::uint8_t type, std::uint8_t report_id, std::uint16_t length, const usbipdcpp::data_type &data,
                            std::uint32_t *p_status) override;
    usbipdcpp::data_type request_get_idle(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                               std::uint32_t *p_status) override;
    void request_set_idle(std::uint8_t speed, std::uint32_t *p_status) override;


    usbipdcpp::data_type report_descriptor{
            // HID报告描述符 - 5按钮鼠标带滚轮
            0x05, 0x01, // Usage Page (Generic Desktop)
            0x09, 0x02, // Usage (Mouse)
            0xA1, 0x01, // Collection (Application)
            0x09, 0x01, //   Usage (Pointer)
            0xA1, 0x00, //   Collection (Physical)

            // 按钮区域 (5个按键 + 3位填充)
            0x05, 0x09, //   Usage Page (Button)
            0x19, 0x01, //   Usage Minimum (Button 1)
            0x29, 0x05, //   Usage Maximum (Button 5)
            0x15, 0x00, //   Logical Minimum (0)
            0x25, 0x01, //   Logical Maximum (1)
            0x95, 0x05, //   Report Count (5)  // 5个按钮
            0x75, 0x01, //   Report Size (1)   // 每个按钮1位
            0x81, 0x02, //   Input (Data,Var,Abs)

            0x95, 0x01, //   Report Count (1)  // 填充3位
            0x75, 0x03, //   Report Size (3)
            0x81, 0x03, //   Input (Const,Var,Abs) // 常量填充

            // 光标移动区域 (X/Y轴)
            0x05, 0x01, //   Usage Page (Generic Desktop)
            0x09, 0x30, //   Usage (X)
            0x09, 0x31, //   Usage (Y)
            0x15, 0x81, //   Logical Minimum (-127)
            0x25, 0x7F, //   Logical Maximum (127)
            0x75, 0x08, //   Report Size (8)   // 8位分辨率
            0x95, 0x02, //   Report Count (2)  // X和Y两个轴
            0x81, 0x06, //   Input (Data,Var,Rel) // 相对坐标

            // 滚轮区域
            0x09, 0x38, //   Usage (Wheel)
            0x15, 0x81, //   Logical Minimum (-127)
            0x25, 0x7F, //   Logical Maximum (127)
            0x75, 0x08, //   Report Size (8)
            0x95, 0x01, //   Report Count (1)
            0x81, 0x06, //   Input (Data,Var,Rel) // 相对滚动量

            0xC0, //   End Collection (Physical)
            0xC0, // End Collection (Application)

    };
    /*
报告描述符说明：
按钮部分:

5个独立按钮 (左键、右键、中键、侧键1、侧键2)
每个按钮占用1位 (0=释放, 1=按下)
用3位常量填充，使字节对齐
光标移动:

X/Y轴相对移动量
8位有符号整数 (-127到+127)
相对坐标模式 (REL)
滚轮:

垂直滚动量
8位有符号整数 (-127到+127)
中键按下时作为按钮，滚动时作为滚轮
报告格式：
[字节0] | 按钮状态 (bit0-4) + 填充 (bit5-7)
[字节1] | X轴移动量 (相对值)
[字节2] | Y轴移动量 (相对值)
[字节3] | 滚轮移动量 (相对值)
*/

    struct State {
        bool left_pressed = false;
        bool right_pressed = false;
        bool middle_pressed = false;
        bool side_pressed = false;
        bool extra_pressed = false;

        std::int8_t wheel_vertical = 0;

        std::int8_t move_horizontal = 0;
        std::int8_t move_vertical = 0;

        bool operator==(const State &) const = default;
    };

    std::atomic_bool should_immediately_stop = false;

    State last_state;
    State current_state;
    std::condition_variable state_cv;
    std::mutex state_mutex;


    std::deque<std::uint32_t> int_req_queue;
    std::shared_mutex int_req_queue_mutex;

    std::thread send_thread;
    std::atomic<std::int16_t> idle_speed = 1;
};