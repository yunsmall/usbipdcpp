#pragma once

#include <map>
#include <shared_mutex>

#include <asio.hpp>
#include <libusb-1.0/libusb.h>

#include "DeviceHandler/DeviceHandler.h"
#include "SetupPacket.h"
#include "constant.h"
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
                const SetupPacket &setup_packet, const data_type &req, std::error_code &ec) override;
        void handle_bulk_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                  UsbInterface &interface, std::uint32_t transfer_flags,
                                  std::uint32_t transfer_buffer_length, const data_type &out_data,
                                  std::error_code &ec) override;
        void handle_interrupt_transfer(std::uint32_t seqnum, const UsbEndpoint &ep,
                                       UsbInterface &interface, std::uint32_t transfer_flags,
                                       std::uint32_t transfer_buffer_length, const data_type &out_data,
                                       std::error_code &ec) override;

        void handle_isochronous_transfer(std::uint32_t seqnum,
                                         const UsbEndpoint &ep, UsbInterface &interface,
                                         std::uint32_t transfer_flags,
                                         std::uint32_t transfer_buffer_length,
                                         const data_type &req,
                                         const std::vector<UsbIpIsoPacketDescriptor> &iso_packet_descriptors,
                                         std::error_code &ec) override;

        bool tweak_clear_halt_cmd(const SetupPacket &setup_packet);
        bool tweak_set_interface_cmd(const SetupPacket &setup_packet);
        bool tweak_set_configuration_cmd(const SetupPacket &setup_packet);
        bool tweak_reset_device_cmd(const SetupPacket &setup_packet);

        /**
         * @brief 返回是否做了特殊操作
         * @param setup_packet
         * @return
         */
        bool tweak_special_requests(const SetupPacket &setup_packet);

        static uint8_t get_libusb_transfer_flags(uint32_t in);

        static void masking_bogus_flags(bool is_out, struct libusb_transfer *trx);

        static int trxstat2error(enum libusb_transfer_status trxstat);
        static enum libusb_transfer_status error2trxstat(int e);

        struct libusb_callback_args {
            LibusbDeviceHandler &handler;
            std::uint32_t seqnum;
            bool is_out;
        };

        static void transfer_callback(libusb_transfer *trx);

        //这个标记一旦为true那么就应该立即停止通信，所有用来标记通信状态的变量都无效
        std::atomic_bool client_disconnection = false;
        std::atomic_bool device_removed = false;

        std::map<std::uint32_t, libusb_transfer *> transferring_data;
        std::shared_mutex transferring_data_mutex;

        libusb_device_handle *const native_handle;

        //不可以有timeout，因为timeout代表设备数据没准备好而不是错误，
        //发生timeout了那么依然会提交一个rep_submit，但设备此时没响应因此不能有提交
        int timeout_milliseconds = 0;
    };

}
