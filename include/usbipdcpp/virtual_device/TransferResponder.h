#pragma once

#include "usbipdcpp/Export.h"
#include "usbipdcpp/protocol.h"
#include "usbipdcpp/utils/LatencyTracker.h"

namespace usbipdcpp {

/**
 * @brief 传输应答接口：设备侧（DeviceHandler/InterfaceHandler/通道/TransferScheduler）
 * 提交传输结果（RET_SUBMIT / RET_UNLINK）与传输停止控制的唯一出口
 *
 * 背景：这套提交应答的工作原本全部由 Session 承担，设备侧直接持有 Session 指针调用。
 * 但 Session 与 usbip 协议、网络线程、Server 生命周期强绑定，导致两个问题：
 * ① 难测试——单测设备行为必须起真实 Server + TCP 连接 + import 握手；
 * ② 难扩展——虚拟设备侧实际是一套用户态 USB 设备栈，将来要接其他后端
 * （如 qemu 的 usbredir）时，设备侧代码与 Session 纠缠无法复用。
 * 因此把「提交应答」这一职责抽成本接口：设备侧只持本接口指针，不依赖 Session。
 *
 * 使用方式：Session 内部持有一个本接口的实现（SessionResponder），经
 * Session::responder() 暴露给设备侧；on_new_connection 等生命周期回调也统一
 * 传本接口。测试时用桩实现替换即可，无需构造真实 Session/Server。
 * 传输停止走 stop_transfer（AbstDeviceHandler::trigger_session_stop 委托给本接口）。
 */
class USBIPDCPP_API TransferResponder {
public:
    virtual ~TransferResponder() = default;

    /// 提交一条 RET_SUBMIT 响应（异步，实现方入队/发出）
    virtual void submit_ret_submit(UsbIpResponse::UsbIpRetSubmit &&submit) = 0;

    /// 提交一条 RET_UNLINK 响应（异步，实现方入队/发出）
    virtual void submit_ret_unlink(UsbIpResponse::UsbIpRetUnlink &&unlink) = 0;

    /// 只入队不唤醒（连续入队多条再统一 wakeup_sender 的场景）
    virtual void enqueue_ret_submit(UsbIpResponse::UsbIpRetSubmit &&submit) = 0;
    virtual void enqueue_ret_unlink(UsbIpResponse::UsbIpRetUnlink &&unlink) = 0;

    /// 唤醒发送线程（与 enqueue_ret_* 配合，最后一次入队后调用）
    virtual void wakeup_sender() = 0;

    /// 中断当前传输（设备拔出/服务器停止时由 trigger_session_stop 调用）
    virtual void stop_transfer() = 0;

    /**
     * @brief 延迟统计对象。埋点宏（LATENCY_TRACK 等）以本指针解引用后传入。
     * 默认返回 nullptr（编译期未开统计或实现方不关心时无需 override）；
     * 编译期未开统计（USBIPDCPP_TRACK_PACKAGE/FORCE_TRACK_PACKAGE 均未定义）时
     * 埋点宏不展开，不会解引用空指针
     */
    virtual LatencyTracker *latency_tracker() { return nullptr; }
};

} // namespace usbipdcpp
