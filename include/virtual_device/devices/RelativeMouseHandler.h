#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include "virtual_device/HidVirtualInterfaceHandler.h"

namespace usbipdcpp {

/**
 * @brief 相对坐标鼠标虚拟设备处理器
 *
 * 提供基于相对偏移的鼠标操作 API。移动量累积到主机读取为止，
 * 多次 move() 调用会自动叠加，报告发送后归零。
 *
 * HID 报告格式（4 字节）：
 *   [0] 按钮 (bit0:左, bit1:右, bit2:中, bit3:侧, bit4:额外) + 3位填充
 *   [1] X 轴相对移动 (-127~127)
 *   [2] Y 轴相对移动 (-127~127)
 *   [3] 滚轮 (-127~127)
 */
class USBIPDCPP_API RelativeMouseHandler : public HidVirtualInterfaceHandler {
public:
    RelativeMouseHandler(UsbInterface &handle_interface, StringPool &string_pool);
    ~RelativeMouseHandler() override = default;

    void on_new_connection(Session &current_session, error_code &ec) override;
    void on_disconnection(error_code &ec) override;

    std::uint16_t get_report_descriptor_size() override;
    data_type get_report_descriptor() override;

    /// 累积相对移动偏移量，clamp 到 [-127, 127]，主机取走前持续累积
    void move(std::int8_t dx, std::int8_t dy);
    /// 累积滚轮偏移量，clamp 到 [-127, 127]
    void set_wheel(std::int8_t delta);

    void set_left_button(bool pressed);
    void set_right_button(bool pressed);
    void set_middle_button(bool pressed);
    void set_side_button(bool pressed);
    void set_extra_button(bool pressed);

    /// 按下 → 延迟 → 释放
    void left_click(int delay_ms = 50);
    void right_click(int delay_ms = 50);
    void middle_click(int delay_ms = 50);
    void double_click(int delay_ms = 100);

    struct ButtonState {
        bool left = false;
        bool right = false;
        bool middle = false;
        bool side = false;
        bool extra = false;
    };
    ButtonState get_button_state() const;

    /// 等待客户端连接
    bool wait_for_client(int timeout_ms = -1);

private:
    struct State {
        bool left = false;
        bool right = false;
        bool middle = false;
        bool side = false;
        bool extra = false;
        std::int8_t dx = 0;
        std::int8_t dy = 0;
        std::int8_t wheel = 0;

        bool operator==(const State &) const = default;
    };

    void notify();
    void send_report();

    data_type report_descriptor;

    State current;
    State last;

    mutable std::mutex state_mutex;
    std::condition_variable state_cv;
    std::thread send_thread;
    std::atomic_bool should_stop{false};

    std::atomic_bool client_connected{false};
    std::mutex connect_mutex;
    std::condition_variable connect_cv;
};

} // namespace usbipdcpp
