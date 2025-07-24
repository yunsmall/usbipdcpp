#pragma once

#include <map>
#include <shared_mutex>

#include <asio.hpp>
#include <libusb-1.0/libusb.h>

#include "DeviceHandler/DeviceHandler.h"
#include "SetupPacket.h"
#include "constant.h"
#include "libusb_handler/tools.h"

namespace usbipdcpp {
    class LibusbDeviceHandler : public DeviceHandlerBase {
        friend class LibusbServer;

    public:
        explicit LibusbDeviceHandler(UsbDevice &handle_device, libusb_device_handle *native_handle);

        ~LibusbDeviceHandler() override;


        void handle_unlink_seqnum(std::uint32_t seqnum) override;

    protected:
        void handle_control_urb(Session &session,
                                std::uint32_t seqnum, const UsbEndpoint &ep,
                                std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
                                const SetupPacket &setup_packet, const data_type &req, std::error_code &ec) override;
        void handle_bulk_transfer(Session &session, std::uint32_t seqnum, const UsbEndpoint &ep,
                                  UsbInterface &interface, std::uint32_t transfer_flags,
                                  std::uint32_t transfer_buffer_length, const data_type &out_data,
                                  std::error_code &ec) override;
        void handle_interrupt_transfer(Session &session, std::uint32_t seqnum, const UsbEndpoint &ep,
                                       UsbInterface &interface, std::uint32_t transfer_flags,
                                       std::uint32_t transfer_buffer_length, const data_type &out_data,
                                       std::error_code &ec) override;

        void handle_isochronous_transfer(Session &session, std::uint32_t seqnum,
                                         const UsbEndpoint &ep, UsbInterface &interface,
                                         std::uint32_t transfer_flags,
                                         std::uint32_t transfer_buffer_length,
                                         const data_type &req,
                                         const std::vector<UsbIpIsoPacketDescriptor> &iso_packet_descriptors,
                                         std::error_code &ec) override;

        bool tweak_clear_halt_cmd(const SetupPacket &setup_packet) {
            auto target_endp = setup_packet.index;
            spdlog::info("tweak_clear_halt_cmd");

            auto ret = libusb_clear_halt(native_handle, target_endp);

            if (ret) {
                SPDLOG_ERROR("libusb_clear_halt() error: endp {} returned {}", target_endp, ret);
            }
            else {
                SPDLOG_DEBUG("libusb_clear_halt() done: endp {}", target_endp);
            }
            // return ret;
            return true;
        }

        bool tweak_set_interface_cmd(const SetupPacket &setup_packet) {
            uint16_t alternate = setup_packet.value;
            uint16_t interface = setup_packet.index;

            SPDLOG_DEBUG("set_interface: inf {} alt {}",
                         interface, alternate);

            int ret = libusb_set_interface_alt_setting(native_handle, interface, alternate);
            if (ret) {
                SPDLOG_ERROR(
                        "{}: usb_set_interface error: inf {} alt {} ret {}",
                        get_device_busid(libusb_get_device(native_handle)),
                        interface, alternate, ret);
            }
            else {
                SPDLOG_DEBUG(
                        "{}: usb_set_interface done: inf {} alt {}",
                        get_device_busid(libusb_get_device(native_handle)),
                        interface, alternate);
            }
            // return ret;
            return true;
        }

        bool tweak_set_configuration_cmd(const SetupPacket &setup_packet) {

            uint16_t config = libusb_le16_to_cpu(setup_packet.value);

            auto ret = libusb_set_configuration(native_handle, config);
            if (ret) {
                SPDLOG_ERROR(
                        "{}: libusb_set_configuration error: config {} ret {}",
                        get_device_busid(libusb_get_device(native_handle)), config, ret);
            }
            else {
                SPDLOG_DEBUG(
                        "{}: libusb_set_configuration done: config {}",
                        get_device_busid(libusb_get_device(native_handle)), config);
            }

            // return 0;
            // return -1;
            return true;
        }


        int tweak_reset_device_cmd(const SetupPacket &setup_packet) {

            SPDLOG_INFO("{}: usb_queue_reset_device",
                        get_device_busid(libusb_get_device(native_handle)));

            // libusb_reset_device(native_handle);
            return 0;
        }

        /**
         * @brief 返回是否做了特殊操作
         * @param setup_packet
         * @return
         */
        bool tweak_special_requests(const SetupPacket &setup_packet) {
            if (setup_packet.is_clear_halt_cmd()) {
                return tweak_clear_halt_cmd(setup_packet);
            }
            else if (setup_packet.is_set_interface_cmd()) {
                return tweak_set_interface_cmd(setup_packet);
            }
            else if (setup_packet.is_set_configuration_cmd()) {
                return tweak_set_configuration_cmd(setup_packet);
            }
            // else if (setup_packet.is_reset_device_cmd()) {
            //     return tweak_reset_device_cmd(setup_packet);
            // }
            SPDLOG_DEBUG("不需要调整包");
            return false;
        }

        static uint8_t get_libusb_transfer_flags(uint32_t in) {
            uint8_t flags = 0;

            if (in & static_cast<std::uint32_t>(TransferFlag::URB_SHORT_NOT_OK))
                flags |= LIBUSB_TRANSFER_SHORT_NOT_OK;
            if (in & static_cast<std::uint32_t>(TransferFlag::URB_ZERO_PACKET))
                flags |= LIBUSB_TRANSFER_ADD_ZERO_PACKET;

            return flags;
        }

        static int trxstat2error(enum libusb_transfer_status trxstat) {
            //具体数值抄的linux的
            switch (trxstat) {
                case LIBUSB_TRANSFER_COMPLETED:
                    return static_cast<int>(UrbStatusType::StatusOK);
                case LIBUSB_TRANSFER_CANCELLED:
                    return static_cast<int>(UrbStatusType::StatusECONNRESET);
                case LIBUSB_TRANSFER_ERROR:
                case LIBUSB_TRANSFER_STALL:
                case LIBUSB_TRANSFER_TIMED_OUT:
                case LIBUSB_TRANSFER_OVERFLOW:
                    return static_cast<int>(UrbStatusType::StatusEPIPE);
                case LIBUSB_TRANSFER_NO_DEVICE:
                    return static_cast<int>(UrbStatusType::StatusESHUTDOWN);
            }
            return static_cast<int>(UrbStatusType::StatusENOENT);
        }

        struct libusb_callback_args {
            LibusbDeviceHandler &handler;
            Session &session;
            std::uint32_t seqnum;
            bool is_out;
        };

        static enum libusb_transfer_status error2trxstat(int e) {
            switch (e) {
                case static_cast<int>(UrbStatusType::StatusOK):
                    return LIBUSB_TRANSFER_COMPLETED;
                case static_cast<int>(UrbStatusType::StatusENOENT):
                    return LIBUSB_TRANSFER_ERROR;
                case static_cast<int>(UrbStatusType::StatusECONNRESET):
                    return LIBUSB_TRANSFER_CANCELLED;
                case static_cast<int>(UrbStatusType::StatusETIMEDOUT):
                    return LIBUSB_TRANSFER_TIMED_OUT;
                case static_cast<int>(UrbStatusType::StatusEPIPE):
                    return LIBUSB_TRANSFER_STALL;
                case static_cast<int>(UrbStatusType::StatusESHUTDOWN):
                    return LIBUSB_TRANSFER_NO_DEVICE;
                case static_cast<int>(UrbStatusType::StatusEEOVERFLOW): //EOVERFLOW
                    return LIBUSB_TRANSFER_OVERFLOW;
            }
            return LIBUSB_TRANSFER_ERROR;
        }

        static void transfer_callback(libusb_transfer *trx);

        std::map<std::uint32_t, libusb_transfer *> transferring_data;
        std::shared_mutex transferring_data_mutex;

        libusb_device_handle *native_handle;
        int timeout_milliseconds = 1000;
    };

}
