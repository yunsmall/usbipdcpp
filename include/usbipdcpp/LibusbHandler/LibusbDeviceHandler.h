#pragma once

#include <atomic>
#include <shared_mutex>
#include <unordered_map>

#include <asio.hpp>
#include <libusb-1.0/libusb.h>

#include "usbipdcpp/DeviceHandler/DeviceHandler.h"
#include "usbipdcpp/LibusbHandler/tools.h"
#include "usbipdcpp/SetupPacket.h"
#include "usbipdcpp/constant.h"
#include "usbipdcpp/protocol.h"
#include "usbipdcpp/utils/ObjectPool.h"

namespace usbipdcpp {
class USBIPDCPP_API LibusbDeviceHandler : public AbstDeviceHandler {
    friend class LibusbServer;

public:
    /**
     * @brief 普通模式构造函数（延迟绑定）
     *
     * 设备在客户端连接时（on_new_connection）才打开。
     *
     * @param handle_device The UsbDevice this handler is attached to.
     * @param native_device The libusb device (not yet opened). The handler takes ownership of this reference.
     */
    explicit LibusbDeviceHandler(UsbDevice &handle_device, libusb_device *native_device);

    /**
     * @brief Android 模式构造函数
     *
     * 使用系统设备文件描述符。每次客户端连接时会调用 libusb_wrap_sys_device 包装 fd。
     * 断连时会关闭 handle，下次连接时重新 wrap，支持重连。
     *
     * @param handle_device The UsbDevice this handler is attached to.
     * @param fd A valid file descriptor opened on the device node.
     *           The fd must remain valid until the handler is destroyed.
     */
    explicit LibusbDeviceHandler(UsbDevice &handle_device, intptr_t fd);

    ~LibusbDeviceHandler() override;
    void on_new_connection(TransferResponder &responder, error_code &ec) override;
    void on_disconnection(error_code &ec) override;
    void handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) override;

    bool is_device_removed() const override {
        return device_removed;
    }

    void on_device_removed() override {
        device_removed = true;
    }

public:
    void receive_urb(UsbIpCommand::UsbIpCmdSubmit cmd, UsbEndpoint ep, std::optional<UsbInterface> interface,
                     usbipdcpp::error_code &ec) override;

    int tweak_clear_halt_cmd(const SetupPacket &setup_packet);
    int tweak_set_interface_cmd(const SetupPacket &setup_packet);
    int tweak_set_configuration_cmd(const SetupPacket &setup_packet);
    int tweak_reset_device_cmd(const SetupPacket &setup_packet);

    /**
     * @brief 处理特殊控制请求
     * @param setup_packet
     * @return -1: 不需要 tweak，应该提交 transfer
     *          0: tweak 成功，不需要提交 transfer
     *         >0: tweak 失败（libusb 错误码），不需要提交 transfer
     */
    int tweak_special_requests(const SetupPacket &setup_packet);

    static uint8_t get_libusb_transfer_flags(uint32_t in);

    static void masking_bogus_flags(bool is_out, struct libusb_transfer *trx);

    static int trxstat2error(enum libusb_transfer_status trxstat);
    static enum libusb_transfer_status error2trxstat(int e);

    struct libusb_callback_args {
        LibusbDeviceHandler *handler = nullptr;
        std::uint32_t seqnum; // CMD_SUBMIT 的 seqnum
        bool is_out;
        TransferHandle transfer; // 拥有 libusb_transfer* 的所有权
        bool unlinking = false; // unlink 正在取消中
        std::uint32_t unlink_cmd_seqnum = 0; // 对应的 CMD_UNLINK seqnum

        void reset() {
            handler = nullptr;
            seqnum = 0;
            is_out = false;
            transfer.reset();
            unlinking = false;
            unlink_cmd_seqnum = 0;
        }
    };

    struct CallbackArgsReset {
        static void reset(libusb_callback_args &args) {
            args.reset();
        }
    };

    static void LIBUSB_CALL transfer_callback(libusb_transfer *trx);

    // 对象池：256个，alloc 时自动调用 reset() 清理脏数据
    using CallbackArgsPool =
            ObjectPool<libusb_callback_args, 256, true, detail::DefaultLM<libusb_callback_args>, CallbackArgsReset>;
    CallbackArgsPool callback_args_pool_;

    // 用于等待所有传输完成
    std::mutex transfer_complete_mutex_;
    std::condition_variable transfer_complete_cv_;

    /**
     * @brief 递减 pending_count_ 并通知等待者。
     *
     * 所有递减点统一走本函数（transfer_callback 收尾、receive_urb 的 submit
     * 失败路径）。递减与通知必须放在 transfer_complete_mutex_ 锁内（标准
     * CV 模式），等待者（on_disconnection）在同一把锁内等待，两个并发
     * 正确性问题靠这把锁解决：
     * 1) 丢失唤醒：等待者的流程是「持锁检查谓词（pending_count_==0）→ 不满
     *    足则释放锁并睡眠」。若本函数在锁外递减+通知，递减可能落在等待者
     *    「检查谓词与释放锁入睡」之间：此时通知先于睡眠发生，等待者入睡后
     *    再也收不到通知，永久阻塞。锁内递减使 fetch_sub 必须等 on_disconnection
     *    释放锁（即已入睡）才能执行，随后的通知必然被已睡眠的等待者收到
     * 2) use-after-free：等待者的谓词满足（计数归零）后立即返回，receiver
     *    随后可能马上清理设备并析构本 handler（设备拔出路径无其他引用）。
     *    若递减与通知之间还访问本对象的成员（如 cv），即为悬垂访问。锁内
     *    完成 fetch_sub + notify 保证「计数归零」与「不再触碰 handler」同步
     *    发生：等待者拿到锁看到谓词满足时，本函数必然已全部执行完
     */
    void decrement_pending_and_notify() {
        std::lock_guard lock(transfer_complete_mutex_);
        pending_count_.fetch_sub(1, std::memory_order_release);
        transfer_complete_cv_.notify_one();
    }

    // 这个标记一旦为true那么就应该立即停止通信，所有用来标记通信状态的变量都无效
    std::atomic_bool client_disconnection = false;
    std::atomic_bool device_removed = false;

    // 正在进行的传输：seqnum → callback_args*
    std::shared_mutex transfers_mutex_;
    std::unordered_map<std::uint32_t, libusb_callback_args *> transfers_;
    std::atomic<std::size_t> pending_count_{0};

    // 设备句柄
    // - 普通模式：在 on_new_connection 时赋值
    // - Android 模式：在 on_new_connection 时通过 libusb_wrap_sys_device 创建
    libusb_device_handle *native_handle = nullptr;

    // 设备引用（仅普通模式使用）
    // 通过判断 native_device_ != nullptr 来区分普通模式和 Android 模式
    libusb_device *native_device_ = nullptr;

    // Android 模式：系统设备文件描述符
    intptr_t wrapped_fd_ = -1;

    bool interfaces_claimed_ = false; // 接口是否已声明

    /**
     * @brief Open device and claim interfaces (普通模式).
     * Called on client connection.
     * @return true on success, false on failure.
     */
    bool open_and_claim_device();

    /**
     * @brief Wrap fd and claim interfaces (Android 模式).
     * Called on client connection.
     * @return true on success, false on failure.
     */
    bool wrap_fd_and_claim_interfaces();

    /**
     * @brief Release interfaces and close device.
     * Called on client disconnection.
     */
    void release_and_close_device();

    // 不可以有timeout，因为timeout代表设备数据没准备好而不是错误，
    // 发生timeout了那么依然会提交一个rep_submit，但设备此时没响应因此不能有提交
    static constexpr int timeout_milliseconds = 0;
};

} // namespace usbipdcpp
