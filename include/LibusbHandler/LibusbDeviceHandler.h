#pragma once

#include <atomic>

#include <asio.hpp>
#include <libusb-1.0/libusb.h>

#include "DeviceHandler/DeviceHandler.h"
#include "SetupPacket.h"
#include "constant.h"
#include "protocol.h"
#include "utils/ConcurrentTransferTracker.h"
#include "utils/ObjectPool.h"
#include "LibusbHandler/tools.h"

namespace usbipdcpp {
class LibusbDeviceHandler : public AbstDeviceHandler {
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
    void on_new_connection(Session &current_session, error_code &ec) override;
    void on_disconnection(error_code &ec) override;
    void handle_unlink_seqnum(std::uint32_t unlink_seqnum, std::uint32_t cmd_seqnum) override;

# ifdef USBIPDCPP_ENABLE_BUSY_WAIT
    bool has_pending_transfers() const override {
        return transfer_tracker_.concurrent_count() > 0;
    }
# endif

    bool is_device_removed() const override {
        return device_removed;
    }

    void on_device_removed() override {
        device_removed = true;
    }

    // ========== transfer_handle 操作覆盖实现 ==========
    void* alloc_transfer_handle(std::size_t buffer_length, int num_iso_packets, const UsbIpHeaderBasic& header, const SetupPacket& setup_packet) override;
    void* get_transfer_buffer(void* transfer_handle) override;
    std::size_t get_actual_length(void* transfer_handle) override;
    std::size_t get_read_data_offset(void* transfer_handle) override;
    std::size_t get_write_data_offset(const UsbIpHeaderBasic& header) override;
    UsbIpIsoPacketDescriptor get_iso_descriptor(void* transfer_handle, int index) override;
    void set_iso_descriptor(void* transfer_handle, int index, const UsbIpIsoPacketDescriptor& desc) override;
    void free_transfer_handle(void* transfer_handle) override;

protected:
    void handle_control_urb(
            std::uint32_t seqnum, const UsbEndpoint &ep,
            std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
            const SetupPacket &setup_packet, TransferHandle transfer, std::error_code &ec) override;
    void handle_bulk_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                              UsbInterface &interface, std::uint32_t transfer_flags,
                              std::uint32_t transfer_buffer_length, TransferHandle transfer,
                              std::error_code &ec) override;
    void handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                   UsbInterface &interface, std::uint32_t transfer_flags,
                                   std::uint32_t transfer_buffer_length, TransferHandle transfer,
                                   std::error_code &ec) override;

    void handle_isochronous_transfer(std::uint32_t seqnum,
                                     const UsbEndpoint &ep, UsbInterface &interface,
                                     std::uint32_t transfer_flags,
                                     std::uint32_t transfer_buffer_length,
                                     TransferHandle transfer,
                                     int num_iso_packets,
                                     std::error_code &ec) override;

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
        std::uint32_t seqnum;                          // CMD_SUBMIT 的 seqnum
        bool is_out;
        TransferHandle transfer;                       // 拥有 libusb_transfer* 的所有权
    };

    static void transfer_callback(libusb_transfer *trx);

    // 对象池：64个
    using CallbackArgsPool = ObjectPool<libusb_callback_args, 256, true>;
    CallbackArgsPool callback_args_pool_;

    // 用于等待所有传输完成
    std::mutex transfer_complete_mutex_;
    std::condition_variable transfer_complete_cv_;

    //这个标记一旦为true那么就应该立即停止通信，所有用来标记通信状态的变量都无效
    std::atomic_bool client_disconnection = false;
    std::atomic_bool device_removed = false;

    // 分段锁传输追踪器
    ConcurrentTransferTracker<libusb_transfer *> transfer_tracker_;

    // 设备句柄
    // - 普通模式：在 on_new_connection 时赋值
    // - Android 模式：在 on_new_connection 时通过 libusb_wrap_sys_device 创建
    libusb_device_handle *native_handle = nullptr;

    // 设备引用（仅普通模式使用）
    // 通过判断 native_device_ != nullptr 来区分普通模式和 Android 模式
    libusb_device *native_device_ = nullptr;

    // Android 模式：系统设备文件描述符
    intptr_t wrapped_fd_ = -1;

    bool interfaces_claimed_ = false;  // 接口是否已声明

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

    //不可以有timeout，因为timeout代表设备数据没准备好而不是错误，
    //发生timeout了那么依然会提交一个rep_submit，但设备此时没响应因此不能有提交
    static constexpr int timeout_milliseconds = 0;
};

}
