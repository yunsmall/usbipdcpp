#include "virtual_device/devices/DigitizerHandler.h"

#include <chrono>

#include <asio.hpp>

namespace usbipdcpp {

DigitizerHandler::DigitizerHandler(UsbInterface &handle_interface, StringPool &string_pool, std::uint16_t x_max,
                                   std::uint16_t y_max) :
    HidVirtualInterfaceHandler(handle_interface, string_pool), x_max_(x_max), y_max_(y_max) {
    report_descriptor_ = {
            // Usage Page (Digitizer)
            0x05,
            0x0D,
            // Usage (Touch Screen)
            0x09,
            0x04,
            // Collection (Application)
            0xA1,
            0x01,
            // --- Tip Switch ---
            // Usage (Tip Switch)
            0x09,
            0x42,
            // Logical Min (0)
            0x15,
            0x00,
            // Logical Max (1)
            0x25,
            0x01,
            // Report Size (1)
            0x75,
            0x01,
            // Report Count (1)
            0x95,
            0x01,
            // Input (Data,Var,Abs)
            0x81,
            0x02,
            // --- In Range ---
            // Usage (In Range)
            0x09,
            0x32,
            // Report Size (1)
            0x75,
            0x01,
            // Report Count (1)
            0x95,
            0x01,
            // Input (Data,Var,Abs)
            0x81,
            0x02,
            // --- Padding (6 bits to byte) ---
            // Report Size (6)
            0x75,
            0x06,
            // Report Count (1)
            0x95,
            0x01,
            // Input (Const)
            0x81,
            0x01,
            // --- X ---
            // Usage Page (Generic Desktop)
            0x05,
            0x01,
            // Usage (X)
            0x09,
            0x30,
            // Logical Min (0)
            0x15,
            0x00,
            // Logical Max (16-bit LE inline)
            0x26,
            static_cast<std::uint8_t>(x_max_ & 0xFF),
            static_cast<std::uint8_t>((x_max_ >> 8) & 0xFF),
            // Report Size (16)
            0x75,
            0x10,
            // Report Count (1)
            0x95,
            0x01,
            // Input (Data,Var,Abs)
            0x81,
            0x02,
            // --- Y ---
            // Usage (Y)
            0x09,
            0x31,
            // Logical Min (0)
            0x15,
            0x00,
            // Logical Max (16-bit LE inline)
            0x26,
            static_cast<std::uint8_t>(y_max_ & 0xFF),
            static_cast<std::uint8_t>((y_max_ >> 8) & 0xFF),
            // Report Size (16)
            0x75,
            0x10,
            // Report Count (1)
            0x95,
            0x01,
            // Input (Data,Var,Abs)
            0x81,
            0x02,
            // --- Tip Pressure ---
            // Usage Page (Digitizer)
            0x05,
            0x0D,
            // Usage (Tip Pressure)
            0x09,
            0x30,
            // Logical Min (0)
            0x15,
            0x00,
            // Logical Max (255)
            0x25,
            0xFF,
            // Report Size (8)
            0x75,
            0x08,
            // Report Count (1)
            0x95,
            0x01,
            // Input (Data,Var,Abs)
            0x81,
            0x02,
            // End Collection
            0xC0,
    };
}

void DigitizerHandler::on_new_connection(Session &current_session, error_code &ec) {
    HidVirtualInterfaceHandler::on_new_connection(current_session, ec);

    client_connected_ = true;
    client_connected_.notify_all();
    client_connect_cv_.notify_all();

    should_stop_ = false;
    {
        std::lock_guard lock(state_mutex_);
        current_state_ = TouchState{};
        last_state_ = TouchState{};
    }

    send_thread_ = std::thread([this]() {
        while (!should_stop_) {
            std::unique_lock lock(state_mutex_);
            state_cv_.wait(lock, [this]() { return current_state_ != last_state_ || should_stop_; });
            if (should_stop_)
                break;

            std::array<std::uint8_t, REPORT_SIZE> report{};
            if (current_state_.touching) {
                report[0] = 0x03; // Tip Switch + In Range
                report[1] = current_state_.x & 0xFF;
                report[2] = (current_state_.x >> 8) & 0xFF;
                report[3] = current_state_.y & 0xFF;
                report[4] = (current_state_.y >> 8) & 0xFF;
                report[5] = current_state_.pressure;
            }
            // 不触摸时发送全零报告（tip=0, in_range=0）

            send_input_report(asio::buffer(report));
            last_state_ = current_state_;
        }
    });
}

void DigitizerHandler::on_disconnection(error_code &ec) {
    should_stop_ = true;
    state_cv_.notify_all();
    if (send_thread_.joinable())
        send_thread_.join();
    client_connected_ = false;
    HidVirtualInterfaceHandler::on_disconnection(ec);
}

std::uint16_t DigitizerHandler::get_report_descriptor_size() {
    return static_cast<std::uint16_t>(report_descriptor_.size());
}

data_type DigitizerHandler::get_report_descriptor() {
    return report_descriptor_;
}

void DigitizerHandler::touch(std::uint16_t x, std::uint16_t y, std::uint8_t pressure) {
    std::lock_guard lock(state_mutex_);
    bool changed = !current_state_.touching || current_state_.x != x || current_state_.y != y ||
                   current_state_.pressure != pressure;
    current_state_.touching = true;
    current_state_.x = x;
    current_state_.y = y;
    current_state_.pressure = pressure;
    if (changed)
        state_cv_.notify_one();
}

void DigitizerHandler::release() {
    std::lock_guard lock(state_mutex_);
    if (current_state_.touching) {
        current_state_.touching = false;
        state_cv_.notify_one();
    }
}

bool DigitizerHandler::is_touching() const {
    std::lock_guard lock(state_mutex_);
    return current_state_.touching;
}

std::pair<std::uint16_t, std::uint16_t> DigitizerHandler::get_position() const {
    std::lock_guard lock(state_mutex_);
    return {current_state_.x, current_state_.y};
}

bool DigitizerHandler::wait_for_client(int timeout_ms) {
    if (timeout_ms < 0) {
        client_connected_.wait(false);
        return true;
    }
    std::unique_lock lock(client_connect_mutex_);
    return client_connect_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                       [this] { return client_connected_.load(); });
}

} // namespace usbipdcpp
