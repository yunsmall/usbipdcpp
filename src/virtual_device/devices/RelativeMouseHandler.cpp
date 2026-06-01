#include "virtual_device/devices/RelativeMouseHandler.h"

#include <algorithm>

namespace usbipdcpp {

RelativeMouseHandler::RelativeMouseHandler(UsbInterface &handle_interface, StringPool &string_pool) :
    HidVirtualInterfaceHandler(handle_interface, string_pool) {
    // 相对坐标鼠标: 6 字节报告，5 按钮 + X/Y 16位相对移动 + 滚轮
    // [0] 按钮 bit0-4 + 3 位填充, [1-2] X LE, [3-4] Y LE, [5] 滚轮
    report_descriptor = {
            0x05,
            0x01, // Usage Page (Generic Desktop)
            0x09,
            0x02, // Usage (Mouse)
            0xA1,
            0x01, // Collection (Application)
            0x09,
            0x01, //   Usage (Pointer)
            0xA1,
            0x00, //   Collection (Physical)

            // 5 按钮 + 3 位填充
            0x05,
            0x09, //   Usage Page (Button)
            0x19,
            0x01, //   Usage Minimum (1)
            0x29,
            0x05, //   Usage Maximum (5)
            0x15,
            0x00, //   Logical Minimum (0)
            0x25,
            0x01, //   Logical Maximum (1)
            0x95,
            0x05, //   Report Count (5)
            0x75,
            0x01, //   Report Size (1)
            0x81,
            0x02, //   Input (Data,Var,Abs)

            0x95,
            0x01, //   Report Count (1)
            0x75,
            0x03, //   Report Size (3)
            0x81,
            0x03, //   Input (Const)

            // X/Y 相对移动 16位 (-32767~32767)
            0x05,
            0x01, //   Usage Page (Generic Desktop)
            0x09,
            0x30, //   Usage (X)
            0x09,
            0x31, //   Usage (Y)
            0x16,
            0x01,
            0x80, //   Logical Minimum (-32767)
            0x26,
            0xFF,
            0x7F, //   Logical Maximum (32767)
            0x75,
            0x10, //   Report Size (16)
            0x95,
            0x02, //   Report Count (2)
            0x81,
            0x06, //   Input (Data,Var,Rel)

            // 滚轮 (-127~127)
            0x09,
            0x38, //   Usage (Wheel)
            0x15,
            0x81, //   Logical Minimum (-127)
            0x25,
            0x7F, //   Logical Maximum (127)
            0x75,
            0x08, //   Report Size (8)
            0x95,
            0x01, //   Report Count (1)
            0x81,
            0x06, //   Input (Data,Var,Rel)

            0xC0, //   End Collection (Physical)
            0xC0, // End Collection (Application)
    };
}

void RelativeMouseHandler::on_new_connection(Session &current_session, error_code &ec) {
    HidVirtualInterfaceHandler::on_new_connection(current_session, ec);

    client_connected = true;
    client_connected.notify_all();
    connect_cv.notify_all();

    should_stop = false;
    current = State{};
    last = State{};

    // 启动发送线程，等待状态变化后发送报告
    send_thread = std::thread([this]() {
        while (!should_stop) {
            std::unique_lock lock(state_mutex);
            state_cv.wait(lock, [this]() { return !(current == last) || should_stop; });
            if (should_stop)
                break;

            send_report();
            last = current;
        }
    });
}

void RelativeMouseHandler::on_disconnection(error_code &ec) {
    should_stop = true;
    state_cv.notify_all();
    if (send_thread.joinable())
        send_thread.join();
    client_connected = false;
    client_connected.notify_all();

    HidVirtualInterfaceHandler::on_disconnection(ec);
}

std::uint16_t RelativeMouseHandler::get_report_descriptor_size() {
    return static_cast<std::uint16_t>(report_descriptor.size());
}

data_type RelativeMouseHandler::get_report_descriptor() {
    return report_descriptor;
}

void RelativeMouseHandler::send_report() {
    std::array<std::uint8_t, 6> report{};

    if (current.left)  report[0] |= 0x01;
    if (current.right) report[0] |= 0x02;
    if (current.middle) report[0] |= 0x04;
    if (current.side)   report[0] |= 0x08;
    if (current.extra)  report[0] |= 0x10;

    auto x = static_cast<std::uint16_t>(current.dx);
    auto y = static_cast<std::uint16_t>(current.dy);
    report[1] = x & 0xFF;
    report[2] = (x >> 8) & 0xFF;
    report[3] = y & 0xFF;
    report[4] = (y >> 8) & 0xFF;
    report[5] = static_cast<std::uint8_t>(current.wheel);

    send_input_report(asio::buffer(report));

    // 相对数据发送后归零，避免重复移动
    current.dx = 0;
    current.dy = 0;
    current.wheel = 0;
}

void RelativeMouseHandler::notify() {
    state_cv.notify_one();
}

void RelativeMouseHandler::move(std::int16_t dx, std::int16_t dy) {
    std::lock_guard lock(state_mutex);
    // 累加钳位到 ±32767：int16 溢出会导致方向反向，
    // 钳位到 HID 描述符 16 位 Logical Min/Max 一致，保证饱和而非回绕
    int new_dx = std::clamp(static_cast<int>(current.dx) + dx, -32767, 32767);
    int new_dy = std::clamp(static_cast<int>(current.dy) + dy, -32767, 32767);
    current.dx = static_cast<std::int16_t>(new_dx);
    current.dy = static_cast<std::int16_t>(new_dy);
    notify();
}

void RelativeMouseHandler::set_wheel(std::int8_t delta) {
    std::lock_guard lock(state_mutex);
    // 同 move，累加钳位防止 int8 溢出回绕
    int new_wheel = std::clamp(static_cast<int>(current.wheel) + delta, -127, 127);
    current.wheel = static_cast<std::int8_t>(new_wheel);
    notify();
}

void RelativeMouseHandler::set_left_button(bool pressed) {
    std::lock_guard lock(state_mutex);
    current.left = pressed;
    notify();
}

void RelativeMouseHandler::set_right_button(bool pressed) {
    std::lock_guard lock(state_mutex);
    current.right = pressed;
    notify();
}

void RelativeMouseHandler::set_middle_button(bool pressed) {
    std::lock_guard lock(state_mutex);
    current.middle = pressed;
    notify();
}

void RelativeMouseHandler::set_side_button(bool pressed) {
    std::lock_guard lock(state_mutex);
    current.side = pressed;
    notify();
}

void RelativeMouseHandler::set_extra_button(bool pressed) {
    std::lock_guard lock(state_mutex);
    current.extra = pressed;
    notify();
}

void RelativeMouseHandler::left_click(int delay_ms) {
    set_left_button(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    set_left_button(false);
}

void RelativeMouseHandler::right_click(int delay_ms) {
    set_right_button(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    set_right_button(false);
}

void RelativeMouseHandler::middle_click(int delay_ms) {
    set_middle_button(true);
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    set_middle_button(false);
}

void RelativeMouseHandler::double_click(int delay_ms) {
    left_click(delay_ms / 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    left_click(delay_ms / 2);
}

RelativeMouseHandler::ButtonState RelativeMouseHandler::get_button_state() const {
    std::lock_guard lock(state_mutex);
    return ButtonState{.left = current.left,
                       .right = current.right,
                       .middle = current.middle,
                       .side = current.side,
                       .extra = current.extra};
}

bool RelativeMouseHandler::wait_for_client(int timeout_ms) {
    if (timeout_ms < 0) {
        client_connected.wait(false);
        return true;
    }
    std::unique_lock lock(connect_mutex);
    return connect_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return client_connected.load(); });
}

} // namespace usbipdcpp
