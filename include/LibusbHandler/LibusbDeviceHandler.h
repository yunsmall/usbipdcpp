#pragma once

#include <atomic>

#include <asio.hpp>
#include <libusb-1.0/libusb.h>

#include "DeviceHandler/DeviceHandler.h"
#include "SetupPacket.h"
#include "constant.h"
#include "utils/ConcurrentTransferTracker.h"
#include "utils/ObjectPool.h"
#include "LibusbHandler/tools.h"

namespace usbipdcpp {
class LibusbDeviceHandler : public DeviceHandlerBase {
    friend class LibusbServer;

public:
    explicit LibusbDeviceHandler(UsbDevice &handle_device, libusb_device_handle *native_handle);

    ~LibusbDeviceHandler() override;
    void on_new_connection(Session &current_session, error_code &ec) override;
    void on_disconnection(error_code &ec) override;
    void handle_unlink_seqnum(std::uint32_t seqnum) override;

protected:
    void handle_control_urb(
            std::uint32_t seqnum, const UsbEndpoint &ep,
            std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
            const SetupPacket &setup_packet, data_type &&transfer_data, std::error_code &ec) override;
    void handle_bulk_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                              UsbInterface &interface, std::uint32_t transfer_flags,
                              std::uint32_t transfer_buffer_length, data_type &&transfer_data,
                              std::error_code &ec) override;
    void handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                   UsbInterface &interface, std::uint32_t transfer_flags,
                                   std::uint32_t transfer_buffer_length, data_type &&transfer_data,
                                   std::error_code &ec) override;

    void handle_isochronous_transfer(std::uint32_t seqnum,
                                     const UsbEndpoint &ep, UsbInterface &interface,
                                     std::uint32_t transfer_flags,
                                     std::uint32_t transfer_buffer_length,
                                     data_type &&transfer_data,
                                     const std::vector<UsbIpIsoPacketDescriptor> &iso_packet_descriptors,
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
        std::uint32_t seqnum;
        bool is_out;
        data_type transfer_buffer;
    };

    static void transfer_callback(libusb_transfer *trx);

    // 对象池：初始16个，最多64个
    using CallbackArgsPool = ObjectPool<libusb_callback_args, 16, 64>;
    CallbackArgsPool callback_args_pool_;

    // 用于等待所有传输完成
    std::mutex transfer_complete_mutex_;
    std::condition_variable transfer_complete_cv_;

    //这个标记一旦为true那么就应该立即停止通信，所有用来标记通信状态的变量都无效
    std::atomic_bool client_disconnection = false;
    std::atomic_bool device_removed = false;

    // 分段锁传输追踪器
    ConcurrentTransferTracker<libusb_transfer *> transfer_tracker_;

    libusb_device_handle *const native_handle;

    //不可以有timeout，因为timeout代表设备数据没准备好而不是错误，
    //发生timeout了那么依然会提交一个rep_submit，但设备此时没响应因此不能有提交
    static constexpr int timeout_milliseconds = 0;
};

}
