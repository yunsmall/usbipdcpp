#pragma once

// #include "InterfaceHandler.h"

//全走libusb了，这个类没用了

// namespace usbipcpp {
//     class LibusbInterfaceHandler : public InterfaceHandlerBase {
//     public:
//         explicit LibusbInterfaceHandler(UsbInterface& handle_interface) :
//             InterfaceHandlerBase(handle_interface) {
//         }
//
//         data_type handle_urb(const UsbEndpoint &ep, std::uint32_t transfer_buffer_length, const SetupPacket &setup,
//                 const data_type &out_data, std::error_code &ec) override;
//         data_type get_class_specific_descriptor() override;
//         std::uint8_t get_string_interface_value() const override;
//         std::wstring get_string_interface() const override;
//     };
// }
