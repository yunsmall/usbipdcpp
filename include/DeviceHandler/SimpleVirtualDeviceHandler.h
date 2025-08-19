#pragma once

#include "VirtualDeviceHandler.h"

namespace usbipdcpp {
    class SimpleVirtualDeviceHandler : public VirtualDeviceHandler {
    public:
        SimpleVirtualDeviceHandler(UsbDevice &handle_device, StringPool &string_pool) :
            VirtualDeviceHandler(handle_device, string_pool) {
        }

    protected:
        void handle_non_standard_request_type_control_urb(Session &session, std::uint32_t seqnum, const UsbEndpoint &ep,
                                                          std::uint32_t transfer_flags,
                                                          std::uint32_t transfer_buffer_length,
                                                          const SetupPacket &setup_packet,
                                                          const data_type &out_data, std::error_code &ec) override;

        void request_clear_feature(std::uint16_t feature_selector, std::uint32_t *p_status) override;

        std::uint16_t request_get_status(std::uint32_t *p_status) override;

        void request_set_address(std::uint16_t address, std::uint32_t *status) override;

        void request_set_configuration(std::uint16_t configuration_value, std::uint32_t *p_status) override;

        void request_set_descriptor(std::uint8_t desc_type, std::uint8_t desc_index, std::uint16_t language_id,
                                    std::uint16_t descriptor_length, const data_type &descriptor,
                                    std::uint32_t *p_status) override;

        void request_set_feature(std::uint16_t feature_selector, std::uint32_t *p_status) override;

        data_type get_other_speed_descriptor(std::uint8_t language_id, std::uint16_t descriptor_length,
                                             std::uint32_t *p_status) override;

        void set_descriptor(std::uint16_t configuration_value) override;

    public:
        void handle_unlink_seqnum(std::uint32_t seqnum) override;
    };
}
