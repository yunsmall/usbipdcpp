#pragma once

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include "usbipdcpp/virtual_device/HidVirtualInterfaceHandler.h"

namespace usbipdcpp {

/**
 * @brief USB HID 游戏手柄虚拟设备处理器
 *
 * 标准手柄布局：16 按钮 + D-pad + 4 模拟轴（X/Y/Z/Rz）。
 * 报告格式（11 字节）：
 *   [0-1]  按钮位掩码（16 位，little-endian）
 *   [2]    D-pad 方向（0-7 八方向，0x0F 回中）
 *   [3-4]  X 轴（int16 LE）
 *   [5-6]  Y 轴（int16 LE）
 *   [7-8]  Z 轴（int16 LE）
 *   [9-10] Rz 轴（int16 LE）
 */
class USBIPDCPP_API GamepadHandler : public HidVirtualInterfaceHandler {
public:
    static constexpr uint8_t NUM_BUTTONS = 16;
    static constexpr uint8_t NUM_AXES = 4;
    static constexpr std::size_t REPORT_SIZE = 2 + 1 + NUM_AXES * 2; // 11 字节
    static constexpr int16_t AXIS_MIN = -32768;
    static constexpr int16_t AXIS_MAX = 32767;
    static constexpr int16_t AXIS_CENTER = 0;

    /// D-pad 方向
    enum class HatDirection : uint8_t {
        North = 0,
        NorthEast = 1,
        East = 2,
        SouthEast = 3,
        South = 4,
        SouthWest = 5,
        West = 6,
        NorthWest = 7,
        Center = 0x0F,
    };

    GamepadHandler(UsbInterface &handle_interface, StringPool &string_pool);
    ~GamepadHandler() override = default;

    /**
     * @brief 创建标准的 HID 游戏手柄接口（描述符模板，未绑定 handler）
     *
     * 接口定义：HID 类（03/00/00），一个中断 IN 端点。
     * @param in_ep 中断 IN 端点地址（设备→主机，手柄报告）
     * @return 未绑定 handler 的 UsbInterface
     */
    static UsbInterface make_interface(std::uint8_t in_ep) {
        UsbInterface i{
                .interface_class = static_cast<std::uint8_t>(ClassCode::HID),
                .interface_subclass = 0x00,
                .interface_protocol = 0x00,
                .endpoints = {{UsbEndpoint{.address = in_ep,
                                           .attributes = static_cast<std::uint8_t>(EndpointAttributes::Interrupt),
                                           .max_packet_size = 16, // 11 字节报告 + 对齐余量
                                           .interval = 8}}},
        };
        return i;
    }

    // ========== HidVirtualInterfaceHandler 接口实现 ==========

    void on_new_connection(Session &current_session, error_code &ec) override;
    void on_disconnection(error_code &ec) override;

    std::uint16_t get_report_descriptor_size() override;
    data_type get_report_descriptor() override;
    data_type request_get_report(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                                 std::uint32_t *p_status) override;

    // ========== 按钮 API ==========

    /// 设置按钮状态（0~15）
    void set_button(uint8_t index, bool pressed);
    bool get_button(uint8_t index) const;

    /// 按下一组按钮，其余全部释放
    void press_buttons(std::initializer_list<uint8_t> indices);

    /// 释放所有按钮
    void release_all_buttons();

    // ========== D-pad API ==========

    void set_hat(HatDirection dir);
    HatDirection get_hat() const;

    // ========== 模拟轴 API ==========

    /// 设置轴值（0=X, 1=Y, 2=Z, 3=Rz），范围 AXIS_MIN~AXIS_MAX，中心为 0
    void set_axis(uint8_t index, int16_t value);
    int16_t get_axis(uint8_t index) const;

    /// 等待客户端连接
    bool wait_for_client(int timeout_ms = -1);

private:
    struct GamepadState {
        uint16_t buttons = 0;
        uint8_t hat = static_cast<uint8_t>(HatDirection::Center);
        std::array<int16_t, NUM_AXES> axes{};

        bool operator==(const GamepadState &) const = default;
    };

    data_type report_descriptor_;

    GamepadState current_state_;
    GamepadState last_state_;
    mutable std::mutex state_mutex_;
    std::condition_variable state_cv_;

    std::thread send_thread_;
    std::atomic_bool should_stop_{false};
    std::atomic_bool client_connected_{false};
    mutable std::mutex client_connect_mutex_;
    std::condition_variable client_connect_cv_;
};

} // namespace usbipdcpp
