#pragma once

#include "InterfaceHandler/InterfaceHandler.h"

namespace usbipdcpp {

    class VirtualInterfaceHandler : public InterfaceHandlerBase {
    public:
        explicit VirtualInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool) :
            InterfaceHandlerBase(handle_interface), string_pool(string_pool) {

            string_interface = string_pool.new_string(L"Usbipcpp Virtual Interface");
        }

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

        virtual void handle_non_standard_request_type_control_urb(Session &session, std::uint32_t seqnum,
                                                                  const UsbEndpoint &ep,
                                                                  std::uint32_t transfer_flags,
                                                                  std::uint32_t transfer_buffer_length,
                                                                  const SetupPacket &setup,
                                                                  const data_type &out_data, std::error_code &ec);
        virtual void handle_non_standard_request_type_control_urb_to_endpoint(Session &session, std::uint32_t seqnum,
                                                                              const UsbEndpoint &ep,
                                                                              std::uint32_t transfer_flags,
                                                                              std::uint32_t transfer_buffer_length,
                                                                              const SetupPacket &setup,
                                                                              const data_type &out_data,
                                                                              std::error_code &ec) ;

        virtual void request_clear_feature(std::uint16_t feature_selector, std::uint32_t* p_status) =0;
        virtual void request_endpoint_clear_feature(std::uint16_t feature_selector, std::uint8_t ep_address, std::uint32_t* p_status) =0;

        virtual std::uint8_t request_get_interface(std::uint32_t* p_status) =0;
        virtual void request_set_interface(std::uint16_t alternate_setting, std::uint32_t* p_status) =0;

        virtual std::uint16_t request_get_status(std::uint32_t* p_status) =0;
        virtual std::uint16_t request_endpoint_get_status(std::uint8_t ep_address, std::uint32_t* p_status) =0;

        /**
         * @brief this function is not necessary for all device,
         * HID device is required to implement this function
         * @param type
         * @param language_id
         * @param descriptor_length
         * @param p_status
         * @return
         */
        virtual data_type request_get_descriptor(std::uint8_t type, std::uint8_t language_id,
                                                 std::uint16_t descriptor_length, std::uint32_t* p_status);

        virtual void request_set_feature(std::uint16_t feature_selector, std::uint32_t* p_status) =0;
        virtual void request_endpoint_set_feature(std::uint16_t feature_selector, std::uint8_t ep_address, std::uint32_t* p_status) =0;

        /**
         * @brief Only use for isochronous transfer, so give a default empty implement.
         * @param ep_address
         * @param p_status
         * @return
         */
        virtual std::uint16_t request_endpoint_sync_frame(std::uint8_t ep_address, std::uint32_t* p_status) {
            return 0;
        }


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