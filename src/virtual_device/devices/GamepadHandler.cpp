#include "virtual_device/devices/GamepadHandler.h"

#include <chrono>

#include <asio.hpp>
#include <spdlog/spdlog.h>

namespace usbipdcpp {

GamepadHandler::GamepadHandler(UsbInterface &handle_interface, StringPool &string_pool) :
    HidVirtualInterfaceHandler(handle_interface, string_pool) {
    report_descriptor_ = {
            // Usage Page (Generic Desktop)
            0x05,
            0x01,
            // Usage (Game Pad)
            0x09,
            0x05,
            // Collection (Application)
            0xA1,
            0x01,
            // --- 按钮 1-16 ---
            // Usage Page (Button)
            0x05,
            0x09,
            // Usage Min (1)
            0x19,
            0x01,
            // Usage Max (16)
            0x29,
            0x10,
            // Logical Min (0)
            0x15,
            0x00,
            // Logical Max (1)
            0x25,
            0x01,
            // Report Size (1)
            0x75,
            0x01,
            // Report Count (16)
            0x95,
            0x10,
            // Input (Data,Var,Abs)
            0x81,
            0x02,
            // --- D-pad (Hat Switch) ---
            // Usage Page (Generic Desktop)
            0x05,
            0x01,
            // Usage (Hat Switch)
            0x09,
            0x39,
            // Logical Min (0)
            0x15,
            0x00,
            // Logical Max (7)
            0x25,
            0x07,
            // Report Size (8)
            0x75,
            0x08,
            // Report Count (1)
            0x95,
            0x01,
            // Input (Data,Var,Null State)
            0x81,
            0x42,
            // --- 模拟轴 X/Y/Z/Rz ---
            // Usage Page (Generic Desktop)
            0x05,
            0x01,
            // Usage (X), (Y), (Z), (Rz)
            0x09,
            0x30,
            0x09,
            0x31,
            0x09,
            0x32,
            0x09,
            0x35,
            // Logical Min (-32768)
            0x16,
            0x00,
            0x80,
            // Logical Max (32767)
            0x26,
            0xFF,
            0x7F,
            // Report Size (16)
            0x75,
            0x10,
            // Report Count (4)
            0x95,
            0x04,
            // Input (Data,Var,Abs)
            0x81,
            0x02,
            // End Collection
            0xC0,
    };
}

void GamepadHandler::on_new_connection(Session &current_session, error_code &ec) {
    HidVirtualInterfaceHandler::on_new_connection(current_session, ec);

    client_connected_ = true;
    client_connected_.notify_all();
    client_connect_cv_.notify_all();

    should_stop_ = false;
    {
        std::lock_guard lock(state_mutex_);
        current_state_ = GamepadState{};
        last_state_ = GamepadState{};
    }

    send_thread_ = std::thread([this]() {
        while (!should_stop_) {
            std::unique_lock lock(state_mutex_);
            state_cv_.wait(lock, [this]() { return current_state_ != last_state_ || should_stop_; });
            if (should_stop_)
                break;

            std::array<uint8_t, REPORT_SIZE> report{};
            // 按钮位掩码（LE）
            report[0] = current_state_.buttons & 0xFF;
            report[1] = (current_state_.buttons >> 8) & 0xFF;
            // D-pad
            report[2] = current_state_.hat;
            // 轴（LE）
            for (uint8_t i = 0; i < NUM_AXES; ++i) {
                uint16_t val = static_cast<uint16_t>(static_cast<int16_t>(current_state_.axes[i]));
                report[3 + i * 2] = val & 0xFF;
                report[4 + i * 2] = (val >> 8) & 0xFF;
            }

            send_input_report(asio::buffer(report));
            last_state_ = current_state_;
        }
    });
}

void GamepadHandler::on_disconnection(error_code &ec) {
    should_stop_ = true;
    state_cv_.notify_all();
    if (send_thread_.joinable())
        send_thread_.join();
    client_connected_ = false;
    HidVirtualInterfaceHandler::on_disconnection(ec);
}

std::uint16_t GamepadHandler::get_report_descriptor_size() {
    return static_cast<std::uint16_t>(report_descriptor_.size());
}

data_type GamepadHandler::get_report_descriptor() {
    return report_descriptor_;
}

data_type GamepadHandler::request_get_report(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                                             std::uint32_t *p_status) {
    if (static_cast<HIDReportType>(type) == HIDReportType::Input) {
        std::lock_guard lock(state_mutex_);
        data_type result(REPORT_SIZE, 0);
        result[0] = current_state_.buttons & 0xFF;
        result[1] = (current_state_.buttons >> 8) & 0xFF;
        result[2] = current_state_.hat;
        for (uint8_t i = 0; i < NUM_AXES; ++i) {
            uint16_t val = static_cast<uint16_t>(static_cast<int16_t>(current_state_.axes[i]));
            result[3 + i * 2] = val & 0xFF;
            result[4 + i * 2] = (val >> 8) & 0xFF;
        }
        return result;
    }
    *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
    return {};
}

// ========== 按钮 API ==========

void GamepadHandler::set_button(uint8_t index, bool pressed) {
    if (index >= NUM_BUTTONS)
        return;
    std::lock_guard lock(state_mutex_);
    uint16_t old = current_state_.buttons;
    if (pressed)
        current_state_.buttons |= (1u << index);
    else
        current_state_.buttons &= ~(1u << index);
    if (current_state_.buttons != old)
        state_cv_.notify_one();
}

bool GamepadHandler::get_button(uint8_t index) const {
    if (index >= NUM_BUTTONS)
        return false;
    std::lock_guard lock(state_mutex_);
    return (current_state_.buttons >> index) & 1;
}

void GamepadHandler::press_buttons(std::initializer_list<uint8_t> indices) {
    std::lock_guard lock(state_mutex_);
    current_state_.buttons = 0;
    for (auto idx: indices) {
        if (idx < NUM_BUTTONS)
            current_state_.buttons |= (1u << idx);
    }
    state_cv_.notify_one();
}

void GamepadHandler::release_all_buttons() {
    std::lock_guard lock(state_mutex_);
    if (current_state_.buttons != 0) {
        current_state_.buttons = 0;
        state_cv_.notify_one();
    }
}

// ========== D-pad API ==========

void GamepadHandler::set_hat(HatDirection dir) {
    std::lock_guard lock(state_mutex_);
    uint8_t val = static_cast<uint8_t>(dir);
    if (current_state_.hat != val) {
        current_state_.hat = val;
        state_cv_.notify_one();
    }
}

GamepadHandler::HatDirection GamepadHandler::get_hat() const {
    std::lock_guard lock(state_mutex_);
    return static_cast<HatDirection>(current_state_.hat);
}

// ========== 模拟轴 API ==========

void GamepadHandler::set_axis(uint8_t index, int16_t value) {
    if (index >= NUM_AXES)
        return;
    std::lock_guard lock(state_mutex_);
    if (current_state_.axes[index] != value) {
        current_state_.axes[index] = value;
        state_cv_.notify_one();
    }
}

int16_t GamepadHandler::get_axis(uint8_t index) const {
    if (index >= NUM_AXES)
        return 0;
    std::lock_guard lock(state_mutex_);
    return current_state_.axes[index];
}

bool GamepadHandler::wait_for_client(int timeout_ms) {
    if (timeout_ms < 0) {
        client_connected_.wait(false);
        return true;
    }
    std::unique_lock lock(client_connect_mutex_);
    return client_connect_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                       [this] { return client_connected_.load(); });
}

} // namespace usbipdcpp
