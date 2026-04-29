#pragma once

#include <thread>
#include <mutex>
#include <condition_variable>
#include <array>
#include <atomic>
#include <functional>
#include <utility>

#include "virtual_device/HidVirtualInterfaceHandler.h"

namespace usbipdcpp {

/**
 * @brief 高级鼠标虚拟设备处理器
 *
 * 提供完整的鼠标操作API。默认使用屏幕坐标（像素），也可使用 HID 原始坐标。
 *
 * HID 原始坐标范围：[0, 32767]
 */
class AdvancedMouseHandler : public HidVirtualInterfaceHandler {
public:
    /// HID 坐标最大值
    static constexpr std::int16_t HID_MAX = 32767;

    /**
     * @brief 坐标模式
     */
    enum class CoordinateMode {
        Absolute,   ///< 绝对坐标 (0-32767)
        Relative    ///< 相对坐标 (-127 到 127)
    };

    /**
     * @brief 构造函数
     * @param handle_interface USB接口
     * @param string_pool 字符串池
     * @param mode 坐标模式（默认绝对坐标）
     * @param screen_width 屏幕宽度（像素，默认1920）
     * @param screen_height 屏幕高度（像素，默认1080）
     */
    AdvancedMouseHandler(UsbInterface &handle_interface, StringPool &string_pool,
                         CoordinateMode mode = CoordinateMode::Absolute,
                         int screen_width = 1920, int screen_height = 1080);

    ~AdvancedMouseHandler() override = default;

    // ========== HidVirtualInterfaceHandler 接口实现 ==========

    void on_new_connection(Session &current_session, error_code &ec) override;
    void on_disconnection(error_code &ec) override;

    std::uint16_t get_report_descriptor_size() override;
    data_type get_report_descriptor() override;

    // ========== 屏幕配置 ==========

    /**
     * @brief 设置屏幕尺寸
     * @param width 屏幕宽度（像素）
     * @param height 屏幕高度（像素）
     */
    void set_screen_size(int width, int height);

    /**
     * @brief 设置屏幕坐标范围
     * @param x1 左上角X坐标
     * @param y1 左上角Y坐标
     * @param x2 右下角X坐标
     * @param y2 右下角Y坐标
     */
    void set_screen_bounds(int x1, int y1, int x2, int y2);

    int get_screen_width() const;
    int get_screen_height() const;
    int get_screen_x1() const;
    int get_screen_y1() const;
    int get_screen_x2() const;
    int get_screen_y2() const;

    // ========== 鼠标操作 API（屏幕坐标，像素） ==========

    /**
     * @brief 设置鼠标位置（屏幕坐标）
     * @param x X坐标（像素）
     * @param y Y坐标（像素）
     */
    void set_position(int x, int y);

    /**
     * @brief 相对移动（屏幕像素）
     * @param dx X方向偏移（像素）
     * @param dy Y方向偏移（像素）
     */
    void move_relative(int dx, int dy);

    /**
     * @brief 设置左键状态
     * @param pressed true=按下, false=释放
     */
    void set_left_button(bool pressed);

    /**
     * @brief 设置右键状态
     * @param pressed true=按下, false=释放
     */
    void set_right_button(bool pressed);

    /**
     * @brief 设置中键状态
     * @param pressed true=按下, false=释放
     */
    void set_middle_button(bool pressed);

    /**
     * @brief 设置滚轮
     * @param delta 滚动量 (-127 到 127)
     */
    void set_wheel(std::int8_t delta);

    /**
     * @brief 点击左键
     * @param delay_ms 按下和释放之间的延迟（毫秒）
     */
    void left_click(int delay_ms = 50);

    /**
     * @brief 点击右键
     * @param delay_ms 按下和释放之间的延迟（毫秒）
     */
    void right_click(int delay_ms = 50);

    /**
     * @brief 点击中键
     * @param delay_ms 按下和释放之间的延迟（毫秒）
     */
    void middle_click(int delay_ms = 50);

    /**
     * @brief 双击左键
     * @param delay_ms 两次点击之间的延迟（毫秒）
     */
    void double_click(int delay_ms = 100);

    /**
     * @brief 平滑移动到指定位置（屏幕坐标）
     * @param target_x 目标X坐标（像素）
     * @param target_y 目标Y坐标（像素）
     * @param duration_ms 移动总时间（毫秒）
     * @param callback 可选的每帧回调（当前屏幕坐标）
     */
    void smooth_move_to(int target_x, int target_y, int duration_ms,
                        std::function<void(int, int)> callback = nullptr);

    // ========== 鼠标操作 API（HID 原始坐标，raw 后缀） ==========

    /**
     * @brief 设置鼠标位置（HID 原始坐标）
     * @param x HID X坐标 (0-32767)
     * @param y HID Y坐标 (0-32767)
     */
    void set_position_raw(std::int16_t x, std::int16_t y);

    /**
     * @brief 相对移动（HID 原始值）
     * @param dx X方向偏移
     * @param dy Y方向偏移
     */
    void move_relative_raw(std::int16_t dx, std::int16_t dy);

    /**
     * @brief 平滑移动到指定位置（HID 原始坐标）
     * @param target_x 目标HID X坐标 (0-32767)
     * @param target_y 目标HID Y坐标 (0-32767)
     * @param duration_ms 移动总时间（毫秒）
     * @param callback 可选的每帧回调（当前HID坐标）
     */
    void smooth_move_to_raw(std::int16_t target_x, std::int16_t target_y, int duration_ms,
                            std::function<void(std::int16_t, std::int16_t)> callback = nullptr);

    // ========== 坐标转换 ==========

    /**
     * @brief 将屏幕坐标转换为 HID 坐标
     */
    std::pair<std::int16_t, std::int16_t> screen_to_hid(int screen_x, int screen_y) const;

    /**
     * @brief 将 HID 坐标转换为屏幕坐标
     */
    std::pair<int, int> hid_to_screen(std::int16_t hid_x, std::int16_t hid_y) const;

    // ========== 状态查询 ==========

    /**
     * @brief 获取当前状态（屏幕坐标）
     */
    struct State {
        bool left_button = false;
        bool right_button = false;
        bool middle_button = false;
        int x = 0;              ///< X坐标（像素）
        int y = 0;              ///< Y坐标（像素）
        std::int8_t wheel = 0;

        std::int16_t hid_x = 0; ///< HID X坐标 (0-32767)
        std::int16_t hid_y = 0; ///< HID Y坐标 (0-32767)
    };

    State get_current_state() const;

    /**
     * @brief 重置状态（释放所有按键，归零坐标）
     */
    void reset_state();

private:
    CoordinateMode mode_;
    int screen_x1_ = 0;      ///< 屏幕左上角X坐标
    int screen_y1_ = 0;      ///< 屏幕左上角Y坐标
    int screen_x2_ = 1920;   ///< 屏幕右下角X坐标
    int screen_y2_ = 1080;   ///< 屏幕右下角Y坐标
    int screen_width_;       ///< 屏幕宽度 (x2 - x1)
    int screen_height_;      ///< 屏幕高度 (y2 - y1)

    std::int16_t hid_x_ = 0;
    std::int16_t hid_y_ = 0;
    bool left_button_ = false;
    bool right_button_ = false;
    bool middle_button_ = false;
    std::int8_t wheel_ = 0;

    mutable std::mutex state_mutex_;

    std::thread send_thread_;
    std::atomic_bool should_stop_{false};
    std::condition_variable state_cv_;
    bool state_changed_{false};

    // 报告描述符（根据模式动态生成）
    data_type report_descriptor_;

    void generate_report_descriptor();
    void send_current_state();
    void notify_state_change();
};

} // namespace usbipdcpp