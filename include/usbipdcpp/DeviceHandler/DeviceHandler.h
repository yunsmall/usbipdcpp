#pragma once

#include <cstdint>
#include <mutex>
#include <optional>

#include "usbipdcpp/Device.h"
#include "usbipdcpp/DeviceHandler/TransferOperator.h"
#include "usbipdcpp/type.h"


namespace usbipdcpp {
struct UsbEndpoint;
struct UsbInterface;
struct SetupPacket;

class TransferResponder;

/**
 * @brief USB 设备处理抽象基类。
 *
 * 每个导入的设备对应一个 DeviceHandler 实例。传输数据的读写通过
 * TransferOperator 完成，由 get_transfer_operator() 获取。
 * 默认使用 GenericTransferOperator，子类可替换。
 */
class USBIPDCPP_API AbstDeviceHandler {
public:
    explicit AbstDeviceHandler(UsbDevice &handle_device, std::unique_ptr<TransferOperator> op = nullptr) :
        handle_device(handle_device), transfer_op_(op ? std::move(op) : std::make_unique<GenericTransferOperator>()) {
    }

    // 禁止移动：handle_device 是引用成员无法重新绑定，transfer_op_ 与 session
    // 若按默认移动则丢失子类替换的 operator 和会话指针，静默产生错误行为。
    // 当前无任何调用方，直接删除以杜绝未来误用
    AbstDeviceHandler(AbstDeviceHandler &&other) = delete;

    /**
     * @brief 处理 URB 请求的统一入口
     * @param cmd 完整的 CMD_SUBMIT 命令
     * @param ep 端点信息
     * @param interface 可选的接口信息（非控制传输必须有）
     * @param ec 错误码
     */
    virtual void receive_urb(UsbIpCommand::UsbIpCmdSubmit cmd, UsbEndpoint ep, std::optional<UsbInterface> interface,
                             usbipdcpp::error_code &ec) = 0;

    /**
     * @brief 新的客户端连接时会调这个函数，可以阻塞。子类实现时请在函数开头调用这个函数
     * @param responder 请自行储存通信用的应答接口
     * @param ec 发生的ec
     *
     * @attention 失败回滚由本函数内部负责：返回错误（ec 置位）之前，本次调用
     * 已建立的状态必须由实现者自行撤销干净——撤销 responder 注册、停掉已启动
     * 的线程/调度器、断开已建连的接口（子类在基类调用之后才失败时同样如此）。
     * 调用方不会代调 on_disconnection 兜底：带着半开状态（仍有运行中的线程）
     * 的设备被释放析构时会因 joinable 线程直接 terminate。
     */
    virtual void on_new_connection(TransferResponder &responder, error_code &ec) {
        register_responder(responder);
    }

    /**
     * @brief 注册当前通信的应答接口（记录 responder 指针）。
     *
     * 内部基础设施：由基类的 on_new_connection 调用；子类在 on_new_connection
     * 后半段失败（如打开设备失败）时，必须手动调用 remove_responder() 撤销注册。
     * 残留指针并非无害：设备回 available 后若再次被导入，try_moving_device_to_using
     * 先把它移入 using，在本次 on_new_connection 重新注册之前的窗口内设备拔出，
     * trigger_session_stop 会通过上次连接残留的悬垂指针访问已析构的 Session
     */
    void register_responder(TransferResponder &current_responder) {
        std::lock_guard lock(session_mutex_);
        responder = &current_responder;
    }

    /**
     * @brief 移除已注册的 Session（清空 session 指针）。
     *
     * 内部基础设施：由基类的 on_disconnection 调用，也供子类在连接建立
     * 失败时手动调用。仅清指针，不做资源收尾（资源回滚由各自的失败路径负责）
     */
    void remove_responder() {
        std::lock_guard lock(session_mutex_);
        responder = nullptr;
    }

    /**
     * @brief 当发生错误等情况需要完全终止传输时会调用这个函数。被调用后禁止再提交消息和使用Session对象\n
     * 可以阻塞，处理所有需要处理的事务。子类实现时请在函数末尾调用这个函数
     */
    virtual void on_disconnection(error_code &ec) {
        remove_responder();
    }

    /**
     * @brief 检查设备是否已被移除
     * @return true 表示设备已物理拔出
     *
     * @note 返回 true 后 Server 会把设备当"已消失"处理：释放（会话断开 / stop）
     * 时直接从使用中列表丢弃、不再放回可用列表（后端只在拔出那一刻扫一遍列表，
     * 放回去就是没人再清得掉的僵尸），此后的导入尝试也必然失败。默认恒
     * false（虚拟设备不涉及物理拔出）；后端（如 libusb）在检测到拔出时覆盖
     *
     * @attention 禁止返回 true 之后再改回 false：已移除是单向终态——返回 true
     * 后设备随时可能被丢弃析构，不可能复活
     */
    virtual bool is_device_removed() const {
        return false; // 默认实现
    }

    /**
     * @brief 通知 handler"设备理论上已被移除"，请做相应处理（何时让
     *        is_device_removed() 返回 true 由实现自行决定，两者不强制绑定）
     *
     * 不由核心框架调用，一般用于其他后端收到系统拔出通知后回调（如 libusb
     * 后端的 hotplug / 传输错误路径）
     */
    virtual void on_device_removed() {
    }

    /**
     * @brief 线程安全地停止 Session
     */
    void trigger_session_stop();

    /**
     * @brief 处理 USBIP_CMD_UNLINK（客户端要求取消一个未完成的传输）。
     *
     * 协议约定：
     * - 每个 CMD_UNLINK 必须对应一个 RET_UNLINK，status 为 USBIP 错误码。
     * - 如果目标传输已经正常完成，RET_UNLINK 的 status 必须为 0（URB 已完成）。
     * - RET_SUBMIT 必须在 RET_UNLINK 之前发送。客户端先收到 URB 的实际结果，
     *   再收到 unlink 的确认，不能反过来。如果实现中同一个传输可能既发 RET_SUBMIT
     *   又发 RET_UNLINK，必须保证 RET_SUBMIT 先于 RET_UNLINK 到达客户端。
     *
     * @param unlink_seqnum 要取消的 CMD_SUBMIT 的 seqnum
     * @param cmd_seqnum    CMD_UNLINK 自己的 seqnum（用在 RET_UNLINK 里原样返回）
     *
     * 目标传输在 unlink 到达时可能处于以下几种状态，实现需要分别处理：
     *
     * 状态 1 —— 传输尚未开始或仍在进行中：
     *   取消该传输。如果取消成功，传输完成时发送 RET_UNLINK(cmd_seqnum, -ECONNRESET)
     *   （表示 URB 被取消），并且不再发送 RET_SUBMIT。
     *
     * 状态 2 —— 传输已经完成，但 RET_SUBMIT 还未发送：
     *   发送 RET_UNLINK(cmd_seqnum, 实际完成状态码)，如无错误则为 0。
     *   不再发送 RET_SUBMIT。
     *
     * 状态 3 —— RET_SUBMIT 已经发送：
     *   直接发送 RET_UNLINK(cmd_seqnum, 0)。RET_SUBMIT 已先行发送，顺序正确。
     *
     * 状态 4 —— 传输已完全处理完毕（RET_SUBMIT 及可能的 RET_UNLINK 均已发出）：
     *   直接发送 RET_UNLINK(cmd_seqnum, 0)。
     */
    virtual void handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) = 0;

    // ========== TransferOperator 接口 ==========

    /**
     * @brief 获取传输操作器（子类可重写以替换默认实现）
     */
    TransferOperator *get_transfer_operator() const {
        return transfer_op_.get();
    }

    /**
     * @brief 替换传输操作器（所有权转移）
     */
    void set_transfer_operator(std::unique_ptr<TransferOperator> op) {
        transfer_op_ = std::move(op);
    }

    virtual ~AbstDeviceHandler() = default;

protected:
    UsbDevice &handle_device;
    TransferResponder *responder = nullptr;
    mutable std::mutex session_mutex_;
    std::unique_ptr<TransferOperator> transfer_op_ = std::make_unique<GenericTransferOperator>();
};
} // namespace usbipdcpp
