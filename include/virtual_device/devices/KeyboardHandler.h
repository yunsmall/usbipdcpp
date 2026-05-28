#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include "virtual_device/HidVirtualInterfaceHandler.h"

namespace usbipdcpp {

/// 键盘修饰键掩码
namespace KeyboardModifier {
    constexpr std::uint8_t LeftCtrl = 0x01;
    constexpr std::uint8_t LeftShift = 0x02;
    constexpr std::uint8_t LeftAlt = 0x04;
    constexpr std::uint8_t LeftGUI = 0x08;
    constexpr std::uint8_t RightCtrl = 0x10;
    constexpr std::uint8_t RightShift = 0x20;
    constexpr std::uint8_t RightAlt = 0x40;
    constexpr std::uint8_t RightGUI = 0x80;
} // namespace KeyboardModifier

/// 键盘 LED 状态掩码
namespace KeyboardLED {
    constexpr std::uint8_t NumLock = 0x01;
    constexpr std::uint8_t CapsLock = 0x02;
    constexpr std::uint8_t ScrollLock = 0x04;
    constexpr std::uint8_t Compose = 0x08;
    constexpr std::uint8_t Kana = 0x10;
} // namespace KeyboardLED

/**
 * @brief USB HID 键盘虚拟设备处理器（含 Consumer Control 媒体键）
 *
 * 实现标准 USB HID 引导键盘协议 + Consumer Page 媒体控制，使用 Report ID 分隔两个独立报告：
 * - Report ID 1：标准键盘 8 字节报告（兼容 Boot Protocol）
 * - Report ID 2：Consumer Control 2 字节报告（单次触发后自动清零）
 *
 * 支持：最多 6 键 + 8 修饰键、媒体键、LED 状态接收。
 *
 * @note 端点 max_packet_size 至少需 9 字节（Report ID 1 的完整报告）
 */
class USBIPDCPP_API KeyboardHandler : public HidVirtualInterfaceHandler {
public:
    /// 同时按下的最大按键数（不含修饰键）
    static constexpr std::size_t MAX_KEYS = 6;
    static constexpr std::uint8_t REPORT_ID_KEYBOARD = 1;
    static constexpr std::uint8_t REPORT_ID_CONSUMER = 2;

    KeyboardHandler(UsbInterface &handle_interface, StringPool &string_pool);
    ~KeyboardHandler() override = default;

    // ========== HidVirtualInterfaceHandler 接口实现 ==========

    void on_new_connection(Session &current_session, error_code &ec) override;
    void on_disconnection(error_code &ec) override;

    std::uint16_t get_report_descriptor_size() override;
    data_type get_report_descriptor() override;
    data_type request_get_report(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                                 std::uint32_t *p_status) override;
    void request_set_report(std::uint8_t type, std::uint8_t report_id, std::uint16_t length, const data_type &data,
                            std::uint32_t *p_status) override;
    data_type request_get_idle(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                               std::uint32_t *p_status) override;
    void request_set_idle(std::uint8_t speed, std::uint32_t *p_status) override;

    // ========== 按键 API ==========

    /**
     * @brief 按下一个键
     * @param keycode USB HID 按键码（如 0x04 = A, 0x28 = Enter）
     *
     * 若按键已按下或已满 6 键则忽略。
     */
    void press_key(std::uint8_t keycode);

    /**
     * @brief 释放一个键
     * @param keycode USB HID 按键码
     */
    void release_key(std::uint8_t keycode);

    /// 释放所有按键和修饰键
    void release_all();

    /**
     * @brief 同时按下多个键
     * @param keycodes 按键码列表
     *
     * 超过 MAX_KEYS 则只取前 6 个。
     */
    void press_keys(std::initializer_list<std::uint8_t> keycodes);

    /// 查询某键是否正在按下
    [[nodiscard]] bool is_key_pressed(std::uint8_t keycode) const;

    // ========== 修饰键 API ==========

    /// 设置修饰键（按位或 KeyboardModifier 常量）
    void set_modifier(std::uint8_t mask);

    /// 清除指定修饰键
    void clear_modifier(std::uint8_t mask);

    /// 获取当前修饰键状态
    [[nodiscard]] std::uint8_t get_modifier() const;

    // ========== 媒体键 API（Consumer Control） ==========

    /**
     * @brief 发送媒体键，单次触发后自动释放
     * @param usage USB Consumer Page Usage ID（如 HIDConsumer::PlayPause）
     */
    void press_media_key(std::uint16_t usage);

    // ========== LED 状态 ==========

    /// 获取主机设置的 LED 状态（按位与 KeyboardLED 常量）
    [[nodiscard]] std::uint8_t get_led_status() const;

    /// 等待客户端连接，timeout_ms 为负表示无限等待
    bool wait_for_client(int timeout_ms = -1);

private:
    struct KeyboardState {
        std::uint8_t modifier = 0;
        std::array<std::uint8_t, MAX_KEYS> keys{};
        std::uint16_t consumer_usage = 0; // Consumer Page Usage ID，0=无操作

        bool operator==(const KeyboardState &) const = default;
    };

    data_type report_descriptor_;
    std::atomic<std::int16_t> idle_speed_{1};
    std::atomic<std::uint8_t> led_status_{0};

    KeyboardState current_state_;
    KeyboardState last_state_;
    mutable std::mutex state_mutex_;
    std::condition_variable state_cv_;

    std::thread send_thread_;
    std::atomic_bool should_stop_{false};
    std::atomic_bool client_connected_{false};
    mutable std::mutex client_connect_mutex_;
    std::condition_variable client_connect_cv_;
};

} // namespace usbipdcpp
