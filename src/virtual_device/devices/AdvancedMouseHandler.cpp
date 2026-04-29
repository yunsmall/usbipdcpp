#include "virtual_device/devices/AdvancedMouseHandler.h"

#include <algorithm>
#include <cmath>
#include <chrono>

namespace usbipdcpp {

AdvancedMouseHandler::AdvancedMouseHandler(UsbInterface &handle_interface, StringPool &string_pool,
                                           CoordinateMode mode, int screen_width, int screen_height)
    : HidVirtualInterfaceHandler(handle_interface, string_pool),
      mode_(mode),
      screen_x2_(screen_width),
      screen_y2_(screen_height),
      screen_width_(screen_width),
      screen_height_(screen_height) {
    generate_report_descriptor();
}

void AdvancedMouseHandler::set_screen_size(int width, int height) {
    screen_x2_ = screen_x1_ + width;
    screen_y2_ = screen_y1_ + height;
    screen_width_ = width;
    screen_height_ = height;
}

void AdvancedMouseHandler::set_screen_bounds(int x1, int y1, int x2, int y2) {
    screen_x1_ = x1;
    screen_y1_ = y1;
    screen_x2_ = x2;
    screen_y2_ = y2;
    screen_width_ = x2 - x1;
    screen_height_ = y2 - y1;
}

int AdvancedMouseHandler::get_screen_width() const {
    return screen_width_;
}

int AdvancedMouseHandler::get_screen_height() const {
    return screen_height_;
}

int AdvancedMouseHandler::get_screen_x1() const {
    return screen_x1_;
}

int AdvancedMouseHandler::get_screen_y1() const {
    return screen_y1_;
}

int AdvancedMouseHandler::get_screen_x2() const {
    return screen_x2_;
}

int AdvancedMouseHandler::get_screen_y2() const {
    return screen_y2_;
}

std::pair<std::int16_t, std::int16_t> AdvancedMouseHandler::screen_to_hid(int screen_x, int screen_y) const {
    // 将屏幕坐标从 [x1, x2] 和 [y1, y2] 映射到 HID [0, 32767]
    double relative_x = static_cast<double>(screen_x - screen_x1_);
    double relative_y = static_cast<double>(screen_y - screen_y1_);
    double hid_x_double = relative_x * HID_MAX / screen_width_;
    double hid_y_double = relative_y * HID_MAX / screen_height_;
    int hid_x_int = std::clamp(static_cast<int>(std::round(hid_x_double)), 0, static_cast<int>(HID_MAX));
    int hid_y_int = std::clamp(static_cast<int>(std::round(hid_y_double)), 0, static_cast<int>(HID_MAX));
    return {static_cast<std::int16_t>(hid_x_int), static_cast<std::int16_t>(hid_y_int)};
}

std::pair<int, int> AdvancedMouseHandler::hid_to_screen(std::int16_t hid_x, std::int16_t hid_y) const {
    // 将 HID 坐标从 [0, 32767] 映射到屏幕 [x1, x2] 和 [y1, y2]
    double screen_x_double = static_cast<double>(hid_x) * screen_width_ / HID_MAX;
    double screen_y_double = static_cast<double>(hid_y) * screen_height_ / HID_MAX;
    int screen_x = static_cast<int>(std::round(screen_x_double)) + screen_x1_;
    int screen_y = static_cast<int>(std::round(screen_y_double)) + screen_y1_;
    return {screen_x, screen_y};
}

void AdvancedMouseHandler::generate_report_descriptor() {
    report_descriptor_.clear();

    if (mode_ == CoordinateMode::Absolute) {
        // 绝对坐标模式的报告描述符（6字节报告）
        report_descriptor_ = {
            // Usage Page (Generic Desktop)
            0x05, 0x01,
            // Usage (Mouse)
            0x09, 0x02,
            // Collection (Application)
            0xA1, 0x01,
            // Usage (Pointer)
            0x09, 0x01,
            // Collection (Physical)
            0xA1, 0x00,

            // 按钮 (3个按键)
            0x05, 0x09,       // Usage Page (Button)
            0x19, 0x01,       // Usage Minimum (Button 1)
            0x29, 0x03,       // Usage Maximum (Button 3)
            0x15, 0x00,       // Logical Minimum (0)
            0x25, 0x01,       // Logical Maximum (1)
            0x95, 0x03,       // Report Count (3)
            0x75, 0x01,       // Report Size (1)
            0x81, 0x02,       // Input (Data,Var,Abs)

            // 填充 (5 bits)
            0x95, 0x05,
            0x81, 0x03,

            // X/Y 绝对坐标
            0x05, 0x01,       // Usage Page (Generic Desktop)
            0x09, 0x30,       // Usage (X)
            0x09, 0x31,       // Usage (Y)
            0x16, 0x00, 0x00, // Logical Minimum (0)
            0x26, 0xFF, 0x7F, // Logical Maximum (32767)
            0x75, 0x10,       // Report Size (16)
            0x95, 0x02,       // Report Count (2)
            0x81, 0x02,       // Input (Data,Var,Abs)

            // 滚轮
            0x09, 0x38,       // Usage (Wheel)
            0x15, 0x81,       // Logical Minimum (-127)
            0x25, 0x7F,       // Logical Maximum (127)
            0x75, 0x08,       // Report Size (8)
            0x95, 0x01,       // Report Count (1)
            0x81, 0x06,       // Input (Data,Var,Rel)

            0xC0,             // End Collection (Physical)
            0xC0,             // End Collection (Application)
        };
    } else {
        // 相对坐标模式的报告描述符（4字节报告）
        report_descriptor_ = {
            0x05, 0x01,       // Usage Page (Generic Desktop)
            0x09, 0x02,       // Usage (Mouse)
            0xA1, 0x01,       // Collection (Application)
            0x09, 0x01,       // Usage (Pointer)
            0xA1, 0x00,       // Collection (Physical)

            0x05, 0x09,       // Usage Page (Button)
            0x19, 0x01,       // Usage Minimum (Button 1)
            0x29, 0x03,       // Usage Maximum (Button 3)
            0x15, 0x00,       // Logical Minimum (0)
            0x25, 0x01,       // Logical Maximum (1)
            0x95, 0x03,       // Report Count (3)
            0x75, 0x01,       // Report Size (1)
            0x81, 0x02,       // Input (Data,Var,Abs)

            0x95, 0x05,       // 填充 (5 bits)
            0x81, 0x03,

            0x05, 0x01,       // Usage Page (Generic Desktop)
            0x09, 0x30,       // Usage (X)
            0x09, 0x31,       // Usage (Y)
            0x15, 0x81,       // Logical Minimum (-127)
            0x25, 0x7F,       // Logical Maximum (127)
            0x75, 0x08,       // Report Size (8)
            0x95, 0x02,       // Report Count (2)
            0x81, 0x06,       // Input (Data,Var,Rel)

            0x09, 0x38,       // Usage (Wheel)
            0x95, 0x01,       // Report Count (1)
            0x81, 0x06,       // Input (Data,Var,Rel)

            0xC0,             // End Collection (Physical)
            0xC0,             // End Collection (Application)
        };
    }
}

void AdvancedMouseHandler::on_new_connection(Session &current_session, error_code &ec) {
    HidVirtualInterfaceHandler::on_new_connection(current_session, ec);
    should_stop_ = false;
    state_changed_ = true;

    send_thread_ = std::thread([this]() {
        while (!should_stop_) {
            std::unique_lock lock(state_mutex_);
            state_cv_.wait(lock, [this]() {
                return state_changed_ || should_stop_;
            });
            if (should_stop_) break;

            send_current_state();
            state_changed_ = false;
        }
    });
}

void AdvancedMouseHandler::on_disconnection(error_code &ec) {
    should_stop_ = true;
    state_cv_.notify_all();
    if (send_thread_.joinable()) {
        send_thread_.join();
    }
    HidVirtualInterfaceHandler::on_disconnection(ec);
}

std::uint16_t AdvancedMouseHandler::get_report_descriptor_size() {
    return static_cast<std::uint16_t>(report_descriptor_.size());
}

data_type AdvancedMouseHandler::get_report_descriptor() {
    return report_descriptor_;
}

void AdvancedMouseHandler::send_current_state() {
    if (mode_ == CoordinateMode::Absolute) {
        std::array<std::uint8_t, 6> report{};

        if (left_button_) report[0] |= 0x01;
        if (right_button_) report[0] |= 0x02;
        if (middle_button_) report[0] |= 0x04;

        report[1] = hid_x_ & 0xFF;
        report[2] = (hid_x_ >> 8) & 0xFF;
        report[3] = hid_y_ & 0xFF;
        report[4] = (hid_y_ >> 8) & 0xFF;
        report[5] = static_cast<std::uint8_t>(wheel_);

        send_input_report(asio::buffer(report));
        wheel_ = 0;  // 滚轮发送后归零
    } else {
        std::array<std::uint8_t, 4> report{};

        if (left_button_) report[0] |= 0x01;
        if (right_button_) report[0] |= 0x02;
        if (middle_button_) report[0] |= 0x04;

        report[1] = static_cast<std::uint8_t>(std::clamp(static_cast<int>(hid_x_), -127, 127));
        report[2] = static_cast<std::uint8_t>(std::clamp(static_cast<int>(hid_y_), -127, 127));
        report[3] = static_cast<std::uint8_t>(wheel_);

        send_input_report(asio::buffer(report));

        hid_x_ = 0;
        hid_y_ = 0;
        wheel_ = 0;
    }
}

void AdvancedMouseHandler::notify_state_change() {
    state_changed_ = true;
    state_cv_.notify_one();
}

// ========== 屏幕坐标 API ==========

void AdvancedMouseHandler::set_position(int x, int y) {
    auto [hid_x, hid_y] = screen_to_hid(x, y);
    set_position_raw(hid_x, hid_y);
}

void AdvancedMouseHandler::move_relative(int dx, int dy) {
    // 计算像素对应的 HID 增量
    std::int16_t hid_dx = static_cast<std::int16_t>(dx * HID_MAX / screen_width_);
    std::int16_t hid_dy = static_cast<std::int16_t>(dy * HID_MAX / screen_height_);
    move_relative_raw(hid_dx, hid_dy);
}

void AdvancedMouseHandler::smooth_move_to(int target_x, int target_y, int duration_ms,
                                          std::function<void(int, int)> callback) {
    auto [hid_x, hid_y] = screen_to_hid(target_x, target_y);
    smooth_move_to_raw(hid_x, hid_y, duration_ms, [this, callback](std::int16_t x, std::int16_t y) {
        if (callback) {
            auto [sx, sy] = hid_to_screen(x, y);
            callback(sx, sy);
        }
    });
}

// ========== HID 原始坐标 API (raw) ==========

void AdvancedMouseHandler::set_position_raw(std::int16_t x, std::int16_t y) {
    std::lock_guard lock(state_mutex_);
    hid_x_ = static_cast<std::int16_t>(std::clamp(static_cast<int>(x), 0, static_cast<int>(HID_MAX)));
    hid_y_ = static_cast<std::int16_t>(std::clamp(static_cast<int>(y), 0, static_cast<int>(HID_MAX)));
    notify_state_change();
}

void AdvancedMouseHandler::move_relative_raw(std::int16_t dx, std::int16_t dy) {
    std::lock_guard lock(state_mutex_);
    if (mode_ == CoordinateMode::Absolute) {
        hid_x_ = static_cast<std::int16_t>(std::clamp(static_cast<int>(hid_x_) + dx, 0, static_cast<int>(HID_MAX)));
        hid_y_ = static_cast<std::int16_t>(std::clamp(static_cast<int>(hid_y_) + dy, 0, static_cast<int>(HID_MAX)));
    } else {
        hid_x_ += dx;
        hid_y_ += dy;
    }
    notify_state_change();
}

void AdvancedMouseHandler::smooth_move_to_raw(std::int16_t target_x, std::int16_t target_y, int duration_ms,
                                               std::function<void(std::int16_t, std::int16_t)> callback) {
    std::int16_t start_x, start_y;
    {
        std::lock_guard lock(state_mutex_);
        start_x = hid_x_;
        start_y = hid_y_;
    }

    const int steps = std::max(1, duration_ms / 10);
    const double dx = static_cast<double>(target_x - start_x) / steps;
    const double dy = static_cast<double>(target_y - start_y) / steps;

    for (int i = 1; i <= steps; ++i) {
        if (should_stop_) break;

        std::int16_t x = static_cast<std::int16_t>(start_x + dx * i);
        std::int16_t y = static_cast<std::int16_t>(start_y + dy * i);

        set_position_raw(x, y);

        if (callback) {
            callback(x, y);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// ========== 按钮 API ==========

void AdvancedMouseHandler::set_left_button(bool pressed) {
    std::lock_guard lock(state_mutex_);
    left_button_ = pressed;
    notify_state_change();
}

void AdvancedMouseHandler::set_right_button(bool pressed) {
    std::lock_guard lock(state_mutex_);
    right_button_ = pressed;
    notify_state_change();
}

void AdvancedMouseHandler::set_middle_button(bool pressed) {
    std::lock_guard lock(state_mutex_);
    middle_button_ = pressed;
    notify_state_change();
}

void AdvancedMouseHandler::set_wheel(std::int8_t delta) {
    std::lock_guard lock(state_mutex_);
    wheel_ = delta;
    notify_state_change();
}

void AdvancedMouseHandler::left_click(int delay_ms) {
    set_left_button(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    set_left_button(false);
}

void AdvancedMouseHandler::right_click(int delay_ms) {
    set_right_button(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    set_right_button(false);
}

void AdvancedMouseHandler::middle_click(int delay_ms) {
    set_middle_button(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    set_middle_button(false);
}

void AdvancedMouseHandler::double_click(int delay_ms) {
    left_click(delay_ms / 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    left_click(delay_ms / 2);
}

// ========== 状态查询 ==========

AdvancedMouseHandler::State AdvancedMouseHandler::get_current_state() const {
    std::lock_guard lock(state_mutex_);
    auto [screen_x, screen_y] = hid_to_screen(hid_x_, hid_y_);
    return State{
        .left_button = left_button_,
        .right_button = right_button_,
        .middle_button = middle_button_,
        .x = screen_x,
        .y = screen_y,
        .wheel = wheel_,
        .hid_x = hid_x_,
        .hid_y = hid_y_,
    };
}

void AdvancedMouseHandler::reset_state() {
    std::lock_guard lock(state_mutex_);
    hid_x_ = 0;
    hid_y_ = 0;
    left_button_ = false;
    right_button_ = false;
    middle_button_ = false;
    wheel_ = 0;
    notify_state_change();
}

} // namespace usbipdcpp