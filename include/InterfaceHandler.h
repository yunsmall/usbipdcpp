#pragma once

#include "type.h"
#include "StringPool.h"

namespace usbipcpp {
    struct UsbIpIsoPacketDescriptor;
    struct UsbEndpoint;
    struct SetupPacket;
    struct UsbInterface;

    class Session;


    /**
     * @brief 继承 InterfaceHandlerBase 类，不要继承这个类
     */
    class AbstInterfaceHandler {
    public:
        explicit AbstInterfaceHandler(UsbInterface &handle_interface):
            handle_interface(handle_interface) {
        }

        virtual void handle_control_urb(Session &session, std::uint32_t seqnum, const UsbEndpoint &ep,
                                        std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
                                        const SetupPacket &setup, const data_type &out_data, std::error_code &ec) =0;

        virtual void handle_bulk_transfer(Session &session, std::uint32_t seqnum, const UsbEndpoint &ep,
                                          std::uint32_t transfer_flags,
                                          std::uint32_t transfer_buffer_length, const data_type &out_data,
                                          std::error_code &ec) =0;
        virtual void handle_interrupt_transfer(Session &session, std::uint32_t seqnum, const UsbEndpoint &ep,
                                               std::uint32_t transfer_flags,
                                               std::uint32_t transfer_buffer_length, const data_type &out_data,
                                               std::error_code &ec) =0;

        virtual void handle_isochronous_transfer(Session &session, std::uint32_t seqnum,
                                                 const UsbEndpoint &ep,
                                                 std::uint32_t transfer_flags,
                                                 std::uint32_t transfer_buffer_length, const data_type &out_data,
                                                 const std::vector<UsbIpIsoPacketDescriptor> &
                                                 iso_packet_descriptors, std::error_code &ec) =0;

        virtual ~AbstInterfaceHandler() = default;

    protected:
        UsbInterface &handle_interface;
    };

    class InterfaceHandlerBase : public AbstInterfaceHandler {
    public:
        explicit InterfaceHandlerBase(UsbInterface &handle_interface) :
            AbstInterfaceHandler(handle_interface) {
        }
    };

    class VirtualInterfaceHandler : public InterfaceHandlerBase {
    public:
        explicit VirtualInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool) :
            InterfaceHandlerBase(handle_interface), string_pool(string_pool) {

            string_interface = string_pool.new_string(L"Usbipcpp Virtual Interface");
        }

        void handle_control_urb(Session &session, std::uint32_t seqnum, const UsbEndpoint &ep,
                                std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
                                const SetupPacket &setup, const data_type &out_data, std::error_code &ec) override;

        void handle_bulk_transfer(Session &session, std::uint32_t seqnum, const UsbEndpoint &ep,
                                  std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
                                  const data_type &out_data,
                                  std::error_code &ec) override;
        void handle_interrupt_transfer(Session &session, std::uint32_t seqnum, const UsbEndpoint &ep,
                                       std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
                                       const data_type &out_data,
                                       std::error_code &ec) override;
        void handle_isochronous_transfer(Session &session, std::uint32_t seqnum, const UsbEndpoint &ep,
                                         std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
                                         const data_type &out_data,
                                         const std::vector<UsbIpIsoPacketDescriptor> &iso_packet_descriptors,
                                         std::error_code &ec) override;

        virtual void handle_non_standard_control_urb(Session &session, std::uint32_t seqnum, const UsbEndpoint &ep,
                                                     std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
                                                     const SetupPacket &setup,
                                                     const data_type &out_data, std::error_code &ec) =0;
        virtual void handle_non_standard_control_urb_to_endpoint(Session &session, std::uint32_t seqnum,
                                                                 const UsbEndpoint &ep,
                                                                 std::uint32_t transfer_flags,
                                                                 std::uint32_t transfer_buffer_length,
                                                                 const SetupPacket &setup,
                                                                 const data_type &out_data, std::error_code &ec) =0;

        virtual void request_clear_feature(std::uint16_t feature_selector) =0;
        virtual void request_endpoint_clear_feature(std::uint16_t feature_selector, std::uint8_t ep_address) =0;

        virtual std::uint8_t request_get_interface() =0;
        virtual void request_set_interface(std::uint16_t alternate_setting) =0;

        virtual std::uint16_t request_endpoint_status(std::uint8_t ep_address) =0;
        virtual std::uint16_t request_endpoint_get_status(std::uint8_t ep_address) =0;

        virtual void request_set_feature(std::uint16_t feature_selector) =0;
        virtual void request_endpoint_set_feature(std::uint16_t feature_selector, std::uint8_t ep_address) =0;

        virtual std::uint16_t request_endpoint_sync_frame(std::uint8_t ep_address) =0;

        virtual std::uint16_t request_get_status() =0;

        [[nodiscard]] virtual data_type get_class_specific_descriptor() =0;

        [[nodiscard]] virtual std::uint8_t get_string_interface_value() const {
            return string_interface;
        }

        [[nodiscard]] virtual std::wstring get_string_interface() const {
            return string_pool.get_string(string_interface).value_or(L"");
        }

    protected:
        std::uint8_t string_interface;

        StringPool &string_pool;
    };

}
