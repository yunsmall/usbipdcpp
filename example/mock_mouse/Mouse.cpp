#include <iostream>


#include "VirtualDeviceHandler.h"
#include "InterfaceHandler.h"

using namespace usbipcpp;

class MouseDeviceHandler : public VirtualDeviceHandler {
public:
    MouseDeviceHandler(UsbDevice &handle_device, StringPool &string_pool) :
        VirtualDeviceHandler(handle_device, string_pool) {
        change_string_product(L"Usbipcpp mock mouse");
    }

protected:
    void handle_non_standard_control_urb(Session &session, std::uint32_t seqnum, const UsbEndpoint &ep,
                                         std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
                                         const SetupPacket &setup_packet,
                                         const data_type &out_data, std::error_code &ec) override {
    }

    void handle_non_standard_control_urb_to_endpoint(Session &session, std::uint32_t seqnum, const UsbEndpoint &ep,
                                                     std::uint32_t transfer_flags, std::uint32_t transfer_buffer_length,
                                                     const SetupPacket &setup_packet,
                                                     const data_type &out_data, std::error_code &ec) override {
    }

    void request_clear_feature(std::uint16_t feature_selector) override {
    }

    void request_set_address(std::uint16_t address) override {
    }

    void request_set_configuration(std::uint16_t configuration_value) override {
    }

    data_type request_set_descriptor(std::uint8_t desc_type, std::uint8_t desc_index, std::uint16_t language_id,
                                     std::uint16_t descriptor_length) override {
    }

    void request_set_feature(std::uint16_t feature_selector) override {
    }

    data_type get_other_speed_descriptor(std::uint8_t language_id, std::uint16_t descriptor_length) override {
    }

    void set_descriptor(std::uint16_t configuration_value) override {
    }

};

int main() {


    return 0;
}
