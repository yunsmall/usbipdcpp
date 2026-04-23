#pragma once

#include "virtual_device/SimpleVirtualDeviceHandler.h"
#include "virtual_device/HidVirtualInterfaceHandler.h"
#include "protocol.h"

#include <mutex>
#include <thread>
#include <atomic>
#include <array>
#include <condition_variable>

namespace usbipdcpp {

class LibevdevMouseInterfaceHandler : public HidVirtualInterfaceHandler {
public:
    LibevdevMouseInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool);

    void on_new_connection(Session &current_session, error_code &ec) override;
    void on_disconnection(error_code &ec) override;

    // 重写：主机请求输入报告时返回当前状态
    data_type on_input_report_requested(std::uint16_t length) override;

    std::uint16_t get_report_descriptor_size() override;
    data_type get_report_descriptor() override;

    data_type request_get_report(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                                 std::uint32_t *p_status) override;
    data_type request_get_idle(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                               std::uint32_t *p_status) override;
    void request_set_idle(std::uint8_t speed, std::uint32_t *p_status) override;


    data_type report_descriptor{
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


    /**
     * @brief 没加锁，请自行加锁
     */
    void reset_relative_data();

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

    State current_state;
    State last_state;  // 用于检测状态变化
    std::mutex state_mutex;
    std::condition_variable state_cv;  // 等待状态变化
    std::thread send_thread;  // 发送线程

    std::atomic<std::int16_t> idle_speed = 1;
};

}
