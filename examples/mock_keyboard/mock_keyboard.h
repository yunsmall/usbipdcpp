#pragma once

#include <thread>
#include <mutex>
#include <condition_variable>
#include <array>

#include "virtual_device/SimpleVirtualDeviceHandler.h"
#include "virtual_device/HidVirtualInterfaceHandler.h"
#include "Server.h"
#include "Session.h"
#include "protocol.h"


class MockKeyboardInterfaceHandler : public usbipdcpp::HidVirtualInterfaceHandler {
public:
    MockKeyboardInterfaceHandler(usbipdcpp::UsbInterface &handle_interface, usbipdcpp::StringPool &string_pool);

    void on_new_connection(usbipdcpp::Session &current_session, usbipdcpp::error_code &ec) override;
    void on_disconnection(usbipdcpp::error_code &ec) override;

    std::uint16_t get_report_descriptor_size() override;
    usbipdcpp::data_type get_report_descriptor() override;

    // 重写：主机请求输入报告时返回当前状态
    usbipdcpp::data_type on_input_report_requested(std::uint16_t length) override;

    usbipdcpp::data_type request_get_report(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                                            std::uint32_t *p_status) override;
    usbipdcpp::data_type request_get_idle(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                                          std::uint32_t *p_status) override;
    void request_set_idle(std::uint8_t speed, std::uint32_t *p_status) override;


    // 标准键盘HID报告描述符
    usbipdcpp::data_type report_descriptor{
            // Usage Page (Generic Desktop)
            0x05, 0x01,
            // Usage (Keyboard)
            0x09, 0x06,
            // Collection (Application)
            0xA1, 0x01,
            // Usage Page (Key Codes)
            0x05, 0x07,
            // Usage Minimum (224)
            0x19, 0xE0,
            // Usage Maximum (231)
            0x29, 0xE7,
            // Logical Minimum (0)
            0x15, 0x00,
            // Logical Maximum (1)
            0x25, 0x01,
            // Report Size (1)
            0x75, 0x01,
            // Report Count (8)
            0x95, 0x08,
            // Input (Data,Var,Abs) - 修饰键字节
            0x81, 0x02,
            // Report Size (8)
            0x75, 0x08,
            // Report Count (1)
            0x95, 0x01,
            // Input (Const) - 保留字节
            0x81, 0x01,
            // Usage Page (Key Codes)
            0x05, 0x07,
            // Usage Minimum (0)
            0x19, 0x00,
            // Usage Maximum (101)
            0x29, 0x65,
            // Logical Minimum (0)
            0x15, 0x00,
            // Logical Maximum (101)
            0x25, 0x65,
            // Report Size (8)
            0x75, 0x08,
            // Report Count (6)
            0x95, 0x06,
            // Input (Data,Var,Abs) - 按键码
            0x81, 0x00,
            // End Collection
            0xC0,
    };
    /*
    报告描述符说明：
    报告格式（8字节）：
    [字节0] | 修饰键
              bit0: 左Ctrl
              bit1: 左Shift
              bit2: 左Alt
              bit3: 左GUI (Win/Cmd)
              bit4: 右Ctrl
              bit5: 右Shift
              bit6: 右Alt
              bit7: 右GUI
    [字节1] | 保留 (0x00)
    [字节2-7] | 按键码（最多6个同时按下的键）

    常用按键码：
    0x04 = A
    0x05 = B
    ...
    0x1E = 1
    0x1F = 2
    ...
    0x28 = Enter
    0x29 = Escape
    0x2C = Space
    */

    struct State {
        std::uint8_t modifier = 0;          // 修饰键
        std::array<std::uint8_t, 6> keys{}; // 按键码数组

        bool operator==(const State &) const = default;
    };

    std::atomic_bool should_immediately_stop = false;

    State current_state;
    State last_state;  // 用于检测状态变化
    std::mutex state_mutex;
    std::condition_variable state_cv;  // 等待状态变化

    std::thread send_thread;
    std::atomic<std::int16_t> idle_speed = 1;
};
