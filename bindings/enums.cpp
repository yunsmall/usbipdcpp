#include <pybind11/pybind11.h>
#include "constant.h"

namespace py = pybind11;

void bind_enums(py::module_ &m) {
    // UsbSpeed 枚举
    py::enum_<usbipdcpp::UsbSpeed>(m, "UsbSpeed")
        .value("Unknown", usbipdcpp::UsbSpeed::Unknown)
        .value("Low", usbipdcpp::UsbSpeed::Low)
        .value("Full", usbipdcpp::UsbSpeed::Full)
        .value("High", usbipdcpp::UsbSpeed::High)
        .value("Wireless", usbipdcpp::UsbSpeed::Wireless)
        .value("Super", usbipdcpp::UsbSpeed::Super)
        .value("SuperPlus", usbipdcpp::UsbSpeed::SuperPlus)
        .export_values();

    // ClassCode 枚举
    py::enum_<usbipdcpp::ClassCode>(m, "ClassCode")
        .value("SeeInterface", usbipdcpp::ClassCode::SeeInterface)
        .value("Audio", usbipdcpp::ClassCode::Audio)
        .value("CDC", usbipdcpp::ClassCode::CDC)
        .value("HID", usbipdcpp::ClassCode::HID)
        .value("Physical", usbipdcpp::ClassCode::Physical)
        .value("Image", usbipdcpp::ClassCode::Image)
        .value("Printer", usbipdcpp::ClassCode::Printer)
        .value("MassStorage", usbipdcpp::ClassCode::MassStorage)
        .value("Hub", usbipdcpp::ClassCode::Hub)
        .value("CDCData", usbipdcpp::ClassCode::CDCData)
        .value("SmartCard", usbipdcpp::ClassCode::SmartCard)
        .value("ContentSecurity", usbipdcpp::ClassCode::ContentSecurity)
        .value("Video", usbipdcpp::ClassCode::Video)
        .value("PersonalHealthcare", usbipdcpp::ClassCode::PersonalHealthcare)
        .value("AudioVideo", usbipdcpp::ClassCode::AudioVideo)
        .value("Billboard", usbipdcpp::ClassCode::Billboard)
        .value("TypeCBridge", usbipdcpp::ClassCode::TypeCBridge)
        .value("Diagnostic", usbipdcpp::ClassCode::Diagnostic)
        .value("WirelessController", usbipdcpp::ClassCode::WirelessController)
        .value("Misc", usbipdcpp::ClassCode::Misc)
        .value("ApplicationSpecific", usbipdcpp::ClassCode::ApplicationSpecific)
        .value("VendorSpecific", usbipdcpp::ClassCode::VendorSpecific)
        .export_values();

    // Direction 枚举
    py::enum_<usbipdcpp::Direction>(m, "Direction")
        .value("In", usbipdcpp::Direction::In)
        .value("Out", usbipdcpp::Direction::Out)
        .export_values();

    // EndpointAttributes 枚举
    py::enum_<usbipdcpp::EndpointAttributes>(m, "EndpointAttributes")
        .value("Control", usbipdcpp::EndpointAttributes::Control)
        .value("Isochronous", usbipdcpp::EndpointAttributes::Isochronous)
        .value("Bulk", usbipdcpp::EndpointAttributes::Bulk)
        .value("Interrupt", usbipdcpp::EndpointAttributes::Interrupt)
        .export_values();
}
