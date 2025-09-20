#pragma once


#include "protocol.h"
#include "InterfaceHandler/VirtualInterfaceHandler.h"
#include "SetupPacket.h"
#include "constant.h"


namespace usbipdcpp {
    /**
     * @brief Notice: handle get_descriptor request in handle_non_standard_control_urb,
     * and return descriptor by calling get_report_descriptor.
     */
    class HidVirtualInterfaceHandler : public VirtualInterfaceHandler {
    public:
        HidVirtualInterfaceHandler(UsbInterface &handle_interface, StringPool &string_pool) :
            VirtualInterfaceHandler(handle_interface, string_pool) {
        }

        void handle_non_standard_request_type_control_urb(std::uint32_t seqnum, const UsbEndpoint &ep,
                                                          std::uint32_t transfer_flags,
                                                          std::uint32_t transfer_buffer_length,
                                                          const SetupPacket &setup_packet,
                                                          const data_type &out_data, std::error_code &ec) override;

        virtual void handle_non_hid_request_type_control_urb(std::uint32_t seqnum,
                                                             const UsbEndpoint &ep,
                                                             std::uint32_t transfer_flags,
                                                             std::uint32_t transfer_buffer_length,
                                                             const SetupPacket &setup_packet,
                                                             const data_type &out_data, std::error_code &ec) =0;

        data_type request_get_descriptor(std::uint8_t type, std::uint8_t language_id,
                                         std::uint16_t descriptor_length, std::uint32_t *p_status) override;

        [[nodiscard]] data_type get_class_specific_descriptor() override;

        virtual data_type get_report_descriptor() =0;
        virtual std::uint16_t get_report_descriptor_size() =0;

        /**
         * @brief Rarely implemented, this is optional for unbooted devices
         * @param p_status
         * @return
         */
        virtual std::uint8_t request_get_protocol(std::uint32_t *p_status) {
            SPDLOG_WARN("unhandled request_get_protocol");
            *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
            return 0;
        };

        /**
         * @brief Rarely implemented, this is optional for unbooted devices
         * @param type
         * @param p_status
         */
        virtual void request_set_protocol(std::uint16_t type, std::uint32_t *p_status) {
            SPDLOG_WARN("unhandled request_set_protocol");
            *p_status = static_cast<std::uint32_t>(UrbStatusType::StatusEPIPE);
        };

        virtual data_type request_get_report(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                                             std::uint32_t *p_status) =0;
        virtual void request_set_report(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                                        const data_type &data,
                                        std::uint32_t *p_status) =0;

        virtual data_type request_get_idle(std::uint8_t type, std::uint8_t report_id, std::uint16_t length,
                                           std::uint32_t *p_status);
        virtual void request_set_idle(std::uint8_t speed, std::uint32_t *p_status);

    };
}
