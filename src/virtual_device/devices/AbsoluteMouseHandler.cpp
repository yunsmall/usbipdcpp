#include "virtual_device/devices/AbsoluteMouseHandler.h"

#include <algorithm>
#include <cmath>
#include <chrono>

namespace usbipdcpp {

AbsoluteMouseHandler::AbsoluteMouseHandler(UsbInterface &handle_interface, StringPool &string_pool,
                                           int screen_width, int screen_height)
    : HidVirtualInterfaceHandler(handle_interface, string_pool),
      screen_x2_(screen_width),
      screen_y2_(screen_height),
      screen_width_(screen_width),
      screen_height_(screen_height) {
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
}

void AbsoluteMouseHandler::set_screen_size(int width, int height) {
    screen_x2_ = screen_x1_ + width;
    screen_y2_ = screen_y1_ + height;
    screen_width_ = width;
    screen_height_ = height;
}

void AbsoluteMouseHandler::set_screen_bounds(int x1, int y1, int x2, int y2) {
    screen_x1_ = x1;
    screen_y1_ = y1;
    screen_x2_ = x2;
    screen_y2_ = y2;
    screen_width_ = x2 - x1;
    screen_height_ = y2 - y1;
}

int AbsoluteMouseHandler::get_screen_width() const { return screen_width_; }
int AbsoluteMouseHandler::get_screen_height() const { return screen_height_; }
int AbsoluteMouseHandler::get_screen_x1() const { return screen_x1_; }
int AbsoluteMouseHandler::get_screen_y1() const { return screen_y1_; }
int AbsoluteMouseHandler::get_screen_x2() const { return screen_x2_; }
int AbsoluteMouseHandler::get_screen_y2() const { return screen_y2_; }

std::pair<std::int16_t, std::int16_t> AbsoluteMouseHandler::screen_to_hid(int screen_x, int screen_y) const {
    double relative_x = static_cast<double>(screen_x - screen_x1_);
    double relative_y = static_cast<double>(screen_y - screen_y1_);
    double hid_x_double = relative_x * HID_MAX / screen_width_;
    double hid_y_double = relative_y * HID_MAX / screen_height_;
    int hid_x_int = std::clamp(static_cast<int>(std::round(hid_x_double)), 0, static_cast<int>(HID_MAX));
    int hid_y_int = std::clamp(static_cast<int>(std::round(hid_y_double)), 0, static_cast<int>(HID_MAX));
    return {static_cast<std::int16_t>(hid_x_int), static_cast<std::int16_t>(hid_y_int)};
}

std::pair<int, int> AbsoluteMouseHandler::hid_to_screen(std::int16_t hid_x, std::int16_t hid_y) const {
    double screen_x_double = static_cast<double>(hid_x) * screen_width_ / HID_MAX;
    double screen_y_double = static_cast<double>(hid_y) * screen_height_ / HID_MAX;
    int screen_x = static_cast<int>(std::round(screen_x_double)) + screen_x1_;
    int screen_y = static_cast<int>(std::round(screen_y_double)) + screen_y1_;
    return {screen_x, screen_y};
}

void AbsoluteMouseHandler::on_new_connection(Session &current_session, error_code &ec) {
    HidVirtualInterfaceHandler::on_new_connection(current_session, ec);

    client_connected_ = true;
    client_connected_.notify_all();

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

void AbsoluteMouseHandler::on_disconnection(error_code &ec) {
    should_stop_ = true;
    state_cv_.notify_all();
    if (send_thread_.joinable()) {
        send_thread_.join();
    }
    client_connected_ = false;
    client_connected_.notify_all();

    HidVirtualInterfaceHandler::on_disconnection(ec);
}

std::uint16_t AbsoluteMouseHandler::get_report_descriptor_size() {
    return static_cast<std::uint16_t>(report_descriptor_.size());
}

data_type AbsoluteMouseHandler::get_report_descriptor() {
    return report_descriptor_;
}

void AbsoluteMouseHandler::send_current_state() {
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
}

void AbsoluteMouseHandler::notify_state_change() {
    state_changed_ = true;
    state_cv_.notify_one();
}

// ========== 屏幕坐标 API ==========

void AbsoluteMouseHandler::set_position(int x, int y) {
    auto [hid_x, hid_y] = screen_to_hid(x, y);
    set_position_raw(hid_x, hid_y);
}

void AbsoluteMouseHandler::move(int from_x, int from_y, int to_x, int to_y, int duration_ms,
                                std::function<void(int, int)> callback) {
    auto [from_hid_x, from_hid_y] = screen_to_hid(from_x, from_y);
    auto [to_hid_x, to_hid_y] = screen_to_hid(to_x, to_y);
    move_raw(from_hid_x, from_hid_y, to_hid_x, to_hid_y, duration_ms, [this, callback](std::int16_t x, std::int16_t y) {
        if (callback) {
            auto [sx, sy] = hid_to_screen(x, y);
            callback(sx, sy);
        }
    });
}

void AbsoluteMouseHandler::humanized_move(int from_x, int from_y, int to_x, int to_y, int duration_ms,
                                          std::function<void(int, int)> callback) {
    auto [from_hid_x, from_hid_y] = screen_to_hid(from_x, from_y);
    auto [to_hid_x, to_hid_y] = screen_to_hid(to_x, to_y);

    // 混合 random_device 和时间戳确保随机性
    std::random_device rd;
    auto now = std::chrono::high_resolution_clock::now();
    auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    std::seed_seq seed{rd(), static_cast<std::uint32_t>(nanos), static_cast<std::uint32_t>(nanos >> 32)};
    std::mt19937 gen(seed);
    // 抖动幅度：约 3-8 像素的随机偏移
    std::uniform_real_distribution<double> jitter_dist(-300.0, 300.0);
    // 速度变化范围加大
    std::uniform_real_distribution<double> speed_dist(0.7, 1.3);
    // 停顿概率约 5%
    std::uniform_int_distribution<int> pause_dist(0, 100);
    std::uniform_int_distribution<int> pause_len_dist(15, 80);

    double mid_x = (from_hid_x + to_hid_x) / 2.0;
    double mid_y = (from_hid_y + to_hid_y) / 2.0;
    double distance = std::sqrt(std::pow(to_hid_x - from_hid_x, 2) + std::pow(to_hid_y - from_hid_y, 2));
    // 贝塞尔曲线控制点偏移：确保有明显的弯曲
    double offset_factor = std::min(distance * 0.25, 5000.0);
    // 控制点放在线段垂直方向偏移，保证弯曲而非只是微调
    double dx = to_hid_x - from_hid_x;
    double dy = to_hid_y - from_hid_y;
    double len = std::sqrt(dx * dx + dy * dy);
    // 垂直方向单位向量
    double perp_x = (len > 0) ? -dy / len : 0;
    double perp_y = (len > 0) ? dx / len : 0;
    std::uniform_real_distribution<double> perp_dist(-offset_factor, offset_factor);
    double perp1 = perp_dist(gen);
    double perp2 = perp_dist(gen);
    // 沿线段方向也加一些偏移
    std::uniform_real_distribution<double> along_dist(-distance * 0.1, distance * 0.1);
    double ctrl1_x = (from_hid_x + mid_x) / 2.0 + perp1 * perp_x + along_dist(gen) * dx / (len > 0 ? len : 1);
    double ctrl1_y = (from_hid_y + mid_y) / 2.0 + perp1 * perp_y + along_dist(gen) * dy / (len > 0 ? len : 1);
    double ctrl2_x = (mid_x + to_hid_x) / 2.0 + perp2 * perp_x + along_dist(gen) * dx / (len > 0 ? len : 1);
    double ctrl2_y = (mid_y + to_hid_y) / 2.0 + perp2 * perp_y + along_dist(gen) * dy / (len > 0 ? len : 1);

    // 三次贝塞尔曲线: B(t) = (1-t)^3*P0 + 3*(1-t)^2*t*P1 + 3*(1-t)*t^2*P2 + t^3*P3
    auto bezier = [&](double t) -> std::pair<double, double> {
        double u = 1.0 - t;
        double x = u*u*u*from_hid_x + 3*u*u*t*ctrl1_x + 3*u*t*t*ctrl2_x + t*t*t*to_hid_x;
        double y = u*u*u*from_hid_y + 3*u*u*t*ctrl1_y + 3*u*t*t*ctrl2_y + t*t*t*to_hid_y;
        return {x, y};
    };

    // 增加步数使运动更平滑
    const int base_steps = std::max(1, duration_ms / 5);
    double t = 0.0;
    double current_speed = 1.0;
    // 抖动用连续随机游走，避免逐帧独立跳变
    double jitter_x_current = 0.0;
    double jitter_y_current = 0.0;
    std::uniform_real_distribution<double> jitter_step(-50.0, 50.0);
    std::uniform_real_distribution<double> jitter_target(-200.0, 200.0);
    double jitter_x_target = jitter_target(gen);
    double jitter_y_target = jitter_target(gen);
    int jitter_retarget_counter = 0;

    while (t < 1.0 && !should_stop_) {
        // 速度变化更平滑
        double speed_target = speed_dist(gen);
        current_speed = current_speed * 0.7 + speed_target * 0.3;
        double dt = current_speed / base_steps;
        t = std::min(t + dt, 1.0);

        // 抖动：平滑游走向目标偏移，到达后重新生成目标
        jitter_x_current += (jitter_x_target - jitter_x_current) * 0.1;
        jitter_y_current += (jitter_y_target - jitter_y_current) * 0.1;
        jitter_retarget_counter++;
        if (jitter_retarget_counter > 10 + gen() % 20) {
            jitter_x_target = jitter_target(gen);
            jitter_y_target = jitter_target(gen);
            jitter_retarget_counter = 0;
        }

        auto [bezier_x, bezier_y] = bezier(t);
        double final_x = bezier_x + jitter_x_current;
        double final_y = bezier_y + jitter_y_current;

        std::int16_t x = static_cast<std::int16_t>(std::clamp(static_cast<int>(std::round(final_x)), 0, static_cast<int>(HID_MAX)));
        std::int16_t y = static_cast<std::int16_t>(std::clamp(static_cast<int>(std::round(final_y)), 0, static_cast<int>(HID_MAX)));

        set_position_raw(x, y);
        if (callback) {
            auto [sx, sy] = hid_to_screen(x, y);
            callback(sx, sy);
        }

        // 约5%概率停顿
        if (pause_dist(gen) < 5) {
            std::this_thread::sleep_for(std::chrono::milliseconds(pause_len_dist(gen)));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    set_position_raw(to_hid_x, to_hid_y);
    if (callback) {
        callback(to_x, to_y);
    }
}

// ========== HID 原始坐标 API ==========

void AbsoluteMouseHandler::set_position_raw(std::int16_t x, std::int16_t y) {
    std::lock_guard lock(state_mutex_);
    hid_x_ = static_cast<std::int16_t>(std::clamp(static_cast<int>(x), 0, static_cast<int>(HID_MAX)));
    hid_y_ = static_cast<std::int16_t>(std::clamp(static_cast<int>(y), 0, static_cast<int>(HID_MAX)));
    notify_state_change();
}

void AbsoluteMouseHandler::move_raw(std::int16_t from_x, std::int16_t from_y, std::int16_t to_x, std::int16_t to_y,
                                    int duration_ms, std::function<void(std::int16_t, std::int16_t)> callback) {
    const int steps = std::max(1, duration_ms / 10);
    const double dx = static_cast<double>(to_x - from_x) / steps;
    const double dy = static_cast<double>(to_y - from_y) / steps;

    for (int i = 1; i <= steps; ++i) {
        if (should_stop_) break;

        std::int16_t x = static_cast<std::int16_t>(from_x + dx * i);
        std::int16_t y = static_cast<std::int16_t>(from_y + dy * i);

        set_position_raw(x, y);
        if (callback) callback(x, y);

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// ========== 按钮 API ==========

void AbsoluteMouseHandler::set_left_button(bool pressed) {
    std::lock_guard lock(state_mutex_);
    left_button_ = pressed;
    notify_state_change();
}

void AbsoluteMouseHandler::set_right_button(bool pressed) {
    std::lock_guard lock(state_mutex_);
    right_button_ = pressed;
    notify_state_change();
}

void AbsoluteMouseHandler::set_middle_button(bool pressed) {
    std::lock_guard lock(state_mutex_);
    middle_button_ = pressed;
    notify_state_change();
}

void AbsoluteMouseHandler::set_wheel(std::int8_t delta) {
    std::lock_guard lock(state_mutex_);
    wheel_ = delta;
    notify_state_change();
}

void AbsoluteMouseHandler::left_click(int x, int y, int delay_ms) {
    set_position(x, y);
    set_left_button(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    set_left_button(false);
}

void AbsoluteMouseHandler::right_click(int x, int y, int delay_ms) {
    set_position(x, y);
    set_right_button(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    set_right_button(false);
}

void AbsoluteMouseHandler::middle_click(int x, int y, int delay_ms) {
    set_position(x, y);
    set_middle_button(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    set_middle_button(false);
}

void AbsoluteMouseHandler::double_click(int x, int y, int delay_ms) {
    left_click(x, y, delay_ms / 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    left_click(x, y, delay_ms / 2);
}

// ========== 拖动 API ==========

void AbsoluteMouseHandler::drag(int from_x, int from_y, int to_x, int to_y, int duration_ms,
                                std::function<void(int, int)> callback) {
    set_position(from_x, from_y);
    set_left_button(true);
    move(from_x, from_y, to_x, to_y, duration_ms, callback);
    set_left_button(false);
}

void AbsoluteMouseHandler::humanized_drag(int from_x, int from_y, int to_x, int to_y, int duration_ms,
                                          std::function<void(int, int)> callback) {
    set_position(from_x, from_y);
    set_left_button(true);
    humanized_move(from_x, from_y, to_x, to_y, duration_ms, callback);
    set_left_button(false);
}

// ========== 状态查询 ==========

AbsoluteMouseHandler::ButtonState AbsoluteMouseHandler::get_button_state() const {
    std::lock_guard lock(state_mutex_);
    return ButtonState{
        .left_button = left_button_,
        .right_button = right_button_,
        .middle_button = middle_button_,
        .wheel = wheel_,
    };
}

bool AbsoluteMouseHandler::wait_for_client(int timeout_ms) {
    if (timeout_ms < 0) {
        client_connected_.wait(false);
        return true;
    }
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (!client_connected_.load()) {
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return true;
}

} // namespace usbipdcpp