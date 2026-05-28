#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include "virtual_device/HidVirtualInterfaceHandler.h"

namespace usbipdcpp {

/**
 * @brief USB HID 触摸屏虚拟设备处理器（单点触摸）
 *
 * 报告格式（6 字节）：
 *   [0]    bit0=Tip Switch, bit1=In Range，其余保留
 *   [1-2]  X 坐标（uint16 LE）
 *   [3-4]  Y 坐标（uint16 LE）
 *   [5]    按压力度（0~255）
 */
class USBIPDCPP_API DigitizerHandler : public HidVirtualInterfaceHandler {
public:
    static constexpr std::size_t REPORT_SIZE = 6;
    static constexpr std::uint16_t DEFAULT_MAX = 32767;

    /**
     * @param handle_interface USB 接口
     * @param string_pool 字符串池
     * @param x_max X 轴最大值（默认 32767）
     * @param y_max Y 轴最大值（默认 32767）
     */
    explicit DigitizerHandler(UsbInterface &handle_interface, StringPool &string_pool,
                              std::uint16_t x_max = DEFAULT_MAX, std::uint16_t y_max = DEFAULT_MAX);

    ~DigitizerHandler() override = default;

    // ========== HidVirtualInterfaceHandler 接口实现 ==========

    void on_new_connection(Session &current_session, error_code &ec) override;
    void on_disconnection(error_code &ec) override;

    std::uint16_t get_report_descriptor_size() override;
    data_type get_report_descriptor() override;

    // ========== 触摸 API ==========

    /// 触摸按下（或移动），坐标范围 [0, max]，压力范围 0~255（默认 128）
    void touch(std::uint16_t x, std::uint16_t y, std::uint8_t pressure = 128);

    /// 移动（同 touch）
    void move(std::uint16_t x, std::uint16_t y, std::uint8_t pressure = 128) {
        touch(x, y, pressure);
    }

    /// 抬起
    void release();

    /// 是否正在触摸
    [[nodiscard]] bool is_touching() const;

    /// 获取当前坐标
    [[nodiscard]] std::pair<std::uint16_t, std::uint16_t> get_position() const;

    /// 获取坐标最大值
    [[nodiscard]] std::uint16_t get_x_max() const {
        return x_max_;
    }
    [[nodiscard]] std::uint16_t get_y_max() const {
        return y_max_;
    }

    /// 等待客户端连接
    bool wait_for_client(int timeout_ms = -1);

private:
    struct TouchState {
        bool touching = false;
        std::uint16_t x = 0;
        std::uint16_t y = 0;
        std::uint8_t pressure = 0;

        bool operator==(const TouchState &) const = default;
    };

    std::uint16_t x_max_;
    std::uint16_t y_max_;
    data_type report_descriptor_;

    TouchState current_state_;
    TouchState last_state_;
    mutable std::mutex state_mutex_;
    std::condition_variable state_cv_;

    std::thread send_thread_;
    std::atomic_bool should_stop_{false};
    std::atomic_bool client_connected_{false};
    mutable std::mutex client_connect_mutex_;
    std::condition_variable client_connect_cv_;
};

} // namespace usbipdcpp
