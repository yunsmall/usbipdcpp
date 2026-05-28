#include "virtual_device/devices/KeyboardHandler.h"

#include <chrono>
#include <thread>

#include <asio.hpp>
#include <spdlog/spdlog.h>

namespace usbipdcpp {

KeyboardHandler::KeyboardHandler(UsbInterface &handle_interface, StringPool &string_pool) :
    HidVirtualInterfaceHandler(handle_interface, string_pool) {
    // 两个独立报告，通过 Report ID 区分（兼容 Boot Protocol）
    report_descriptor_ = {
            // ===== Report ID 1: 标准键盘 =====
            // Usage Page (Generic Desktop)
            0x05,
            0x01,
            // Usage (Keyboard)
            0x09,
            0x06,
            // Collection (Application)
            0xA1,
            0x01,
            // Report ID (1)
            0x85,
            0x01,
            // Usage Page (Key Codes)
            0x05,
            0x07,
            // Usage Minimum (224 = Left Ctrl)
            0x19,
            0xE0,
            // Usage Maximum (231 = Right GUI)
            0x29,
            0xE7,
            // Logical Minimum (0)
            0x15,
            0x00,
            // Logical Maximum (1)
            0x25,
            0x01,
            // Report Size (1)
            0x75,
            0x01,
            // Report Count (8)
            0x95,
            0x08,
            // Input (Data,Var,Abs) — 修饰键字节
            0x81,
            0x02,
            // Report Size (8)
            0x75,
            0x08,
            // Report Count (1)
            0x95,
            0x01,
            // Input (Const) — 保留字节
            0x81,
            0x01,
            // Usage Page (Key Codes)
            0x05,
            0x07,
            // Usage Minimum (0)
            0x19,
            0x00,
            // Usage Maximum (101)
            0x29,
            0x65,
            // Logical Minimum (0)
            0x15,
            0x00,
            // Logical Maximum (101)
            0x25,
            0x65,
            // Report Size (8)
            0x75,
            0x08,
            // Report Count (6)
            0x95,
            0x06,
            // Input (Data,Var,Abs) — 按键码
            0x81,
            0x00,
            // End Collection (Keyboard)
            0xC0,
            // ===== Report ID 2: Consumer Control =====
            // Usage Page (Consumer)
            0x05,
            0x0C,
            // Usage (Consumer Control)
            0x09,
            0x01,
            // Collection (Application)
            0xA1,
            0x01,
            // Report ID (2)
            0x85,
            0x02,
            // Usage Min (0)
            0x19,
            0x00,
            // Usage Max (0x03FF)
            0x2A,
            0xFF,
            0x03,
            // Logical Min (0)
            0x15,
            0x00,
            // Logical Max (0x03FF)
            0x26,
            0xFF,
            0x03,
            // Report Size (16)
            0x75,
            0x10,
            // Report Count (1)
            0x95,
            0x01,
            // Input (Data,Arr,Abs)
            0x81,
            0x00,
            // End Collection (Consumer Control)
            0xC0,
    };
}

void KeyboardHandler::on_new_connection(Session &current_session, error_code &ec) {
    HidVirtualInterfaceHandler::on_new_connection(current_session, ec);

    client_connected_ = true;
    client_connected_.notify_all();
    client_connect_cv_.notify_all();

    should_stop_ = false;
    {
        std::lock_guard lock(state_mutex_);
        current_state_ = KeyboardState{};
        last_state_ = KeyboardState{};
    }
    idle_speed_ = 1;

    send_thread_ = std::thread([this]() {
        while (!should_stop_) {
            std::unique_lock lock(state_mutex_);
            state_cv_.wait(lock, [this]() { return current_state_ != last_state_ || should_stop_; });
            if (should_stop_)
                break;

            bool kb_changed =
                    (current_state_.modifier != last_state_.modifier) || (current_state_.keys != last_state_.keys);
            bool consumer_set = (current_state_.consumer_usage != 0);

            // Report ID 1: 标准键盘（9 字节含 Report ID）
            if (kb_changed) {
                std::array<std::uint8_t, 9> report{};
                report[0] = REPORT_ID_KEYBOARD;
                report[1] = current_state_.modifier;
                report[2] = 0; // 保留字节
                for (std::size_t i = 0; i < MAX_KEYS; ++i) {
                    report[3 + i] = current_state_.keys[i];
                }
                send_input_report(asio::buffer(report));
            }
            // Report ID 2: Consumer Control（3 字节含 Report ID），单次触发后自动清零
            if (consumer_set) {
                std::array<std::uint8_t, 3> report{};
                report[0] = REPORT_ID_CONSUMER;
                uint16_t usage = current_state_.consumer_usage;
                report[1] = usage & 0xFF;
                report[2] = (usage >> 8) & 0xFF;
                send_input_report(asio::buffer(report));
                current_state_.consumer_usage = 0;
            }

            last_state_ = current_state_;
        }
    });
}

void KeyboardHandler::on_disconnection(error_code &ec) {
    should_stop_ = true;
    state_cv_.notify_all();
    if (send_thread_.joinable())
        send_thread_.join();
    client_connected_ = false;
    HidVirtualInterfaceHandler::on_disconnection(ec);
}

std::uint16_t KeyboardHandler::get_report_descriptor_size() {
    return static_cast<std::uint16_t>(report_descriptor_.size());
}

data_type KeyboardHandler::get_report_descriptor() {
    return report_descriptor_;
}

data_type KeyboardHandler::request_get_report(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                                              std::uint32_t *p_status) {
    if (static_cast<HIDReportType>(type) == HIDReportType::Input) {
        std::lock_guard lock(state_mutex_);
        if (report_id == REPORT_ID_CONSUMER) {
            // Consumer Control 报告（3 字节含 Report ID）
            data_type result(3, 0);
            result[0] = REPORT_ID_CONSUMER;
            uint16_t consumer = current_state_.consumer_usage;
            result[1] = consumer & 0xFF;
            result[2] = (consumer >> 8) & 0xFF;
            return result;
        }
        // 默认返回键盘报告（9 字节含 Report ID）
        data_type result(9, 0);
        result[0] = REPORT_ID_KEYBOARD;
        result[1] = current_state_.modifier;
        result[2] = 0;
        for (std::size_t i = 0; i < MAX_KEYS; ++i) {
            result[3 + i] = current_state_.keys[i];
        }
        return result;
    }
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
    return {};
}

void KeyboardHandler::request_set_report(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                                         const data_type &data, std::uint32_t *p_status) {
    // 主机发送的输出报告：LED 状态。data 可能含 Report ID 前缀
    if (static_cast<HIDReportType>(type) == HIDReportType::Output && !data.empty()) {
        led_status_ = (length > 1 && data[0] <= 0x02) ? data[1] : data[0];
        *p_status = 0;
        return;
    }
    HidVirtualInterfaceHandler::request_set_report(type, report_id, length, data, p_status);
}

data_type KeyboardHandler::request_get_idle(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                                            std::uint32_t *p_status) {
    data_type result;
    vector_append_to_net(result, static_cast<std::uint16_t>(idle_speed_.load()));
    return result;
}

void KeyboardHandler::request_set_idle(std::uint8_t speed, std::uint32_t *p_status) {
    idle_speed_ = speed;
    *p_status = 0;
}

// ========== 按键 API ==========

void KeyboardHandler::press_key(std::uint8_t keycode) {
    if (keycode == 0)
        return;

    std::lock_guard lock(state_mutex_);
    // 检查是否已按下
    for (std::size_t i = 0; i < MAX_KEYS; ++i) {
        if (current_state_.keys[i] == keycode)
            return;
    }
    // 插入到第一个空闲槽位
    for (std::size_t i = 0; i < MAX_KEYS; ++i) {
        if (current_state_.keys[i] == 0) {
            current_state_.keys[i] = keycode;
            state_cv_.notify_one();
            return;
        }
    }
    SPDLOG_WARN("键盘按键已满（{} 键），忽略 0x{:02X}", MAX_KEYS, keycode);
}

void KeyboardHandler::release_key(std::uint8_t keycode) {
    if (keycode == 0)
        return;

    std::lock_guard lock(state_mutex_);
    bool found = false;
    for (std::size_t i = 0; i < MAX_KEYS; ++i) {
        if (current_state_.keys[i] == keycode) {
            current_state_.keys[i] = 0;
            found = true;
            break;
        }
    }
    if (!found)
        return;

    // 压缩：移除空洞
    std::size_t write = 0;
    for (std::size_t read = 0; read < MAX_KEYS; ++read) {
        if (current_state_.keys[read] != 0) {
            current_state_.keys[write++] = current_state_.keys[read];
        }
    }
    for (std::size_t i = write; i < MAX_KEYS; ++i) {
        current_state_.keys[i] = 0;
    }
    state_cv_.notify_one();
}

void KeyboardHandler::release_all() {
    std::lock_guard lock(state_mutex_);
    bool changed = current_state_.modifier != 0;
    for (std::size_t i = 0; i < MAX_KEYS; ++i) {
        if (current_state_.keys[i] != 0) {
            changed = true;
            current_state_.keys[i] = 0;
        }
    }
    current_state_.modifier = 0;
    if (changed)
        state_cv_.notify_one();
}

void KeyboardHandler::press_keys(std::initializer_list<std::uint8_t> keycodes) {
    std::lock_guard lock(state_mutex_);
    // 先清零
    for (std::size_t i = 0; i < MAX_KEYS; ++i) {
        current_state_.keys[i] = 0;
    }
    std::size_t idx = 0;
    for (auto kc: keycodes) {
        if (kc != 0 && idx < MAX_KEYS) {
            current_state_.keys[idx++] = kc;
        }
    }
    state_cv_.notify_one();
}

bool KeyboardHandler::is_key_pressed(std::uint8_t keycode) const {
    std::lock_guard lock(state_mutex_);
    for (std::size_t i = 0; i < MAX_KEYS; ++i) {
        if (current_state_.keys[i] == keycode)
            return true;
    }
    return false;
}

// ========== 修饰键 API ==========

void KeyboardHandler::set_modifier(std::uint8_t mask) {
    std::lock_guard lock(state_mutex_);
    auto old = current_state_.modifier;
    current_state_.modifier |= mask;
    if (current_state_.modifier != old)
        state_cv_.notify_one();
}

void KeyboardHandler::clear_modifier(std::uint8_t mask) {
    std::lock_guard lock(state_mutex_);
    auto old = current_state_.modifier;
    current_state_.modifier &= ~mask;
    if (current_state_.modifier != old)
        state_cv_.notify_one();
}

std::uint8_t KeyboardHandler::get_modifier() const {
    std::lock_guard lock(state_mutex_);
    return current_state_.modifier;
}

// ========== 媒体键 API ==========

void KeyboardHandler::press_media_key(std::uint16_t usage) {
    std::lock_guard lock(state_mutex_);
    current_state_.consumer_usage = usage;
    state_cv_.notify_one();
}

// ========== LED 状态 ==========

std::uint8_t KeyboardHandler::get_led_status() const {
    return led_status_.load();
}

bool KeyboardHandler::wait_for_client(int timeout_ms) {
    if (timeout_ms < 0) {
        client_connected_.wait(false);
        return true;
    }
    std::unique_lock lock(client_connect_mutex_);
    return client_connect_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                       [this] { return client_connected_.load(); });
}

} // namespace usbipdcpp
