#include <pybind11/pybind11.h>
#include "Endpoint.h"

namespace py = pybind11;

void bind_endpoint(py::module_ &m) {
    py::class_<usbipdcpp::UsbEndpoint>(m, "UsbEndpoint")
        .def(py::init<>())
        .def_readwrite("address", &usbipdcpp::UsbEndpoint::address)
        .def_readwrite("attributes", &usbipdcpp::UsbEndpoint::attributes)
        .def_readwrite("max_packet_size", &usbipdcpp::UsbEndpoint::max_packet_size)
        .def_readwrite("interval", &usbipdcpp::UsbEndpoint::interval)
        .def("direction", &usbipdcpp::UsbEndpoint::direction)
        .def("is_in", &usbipdcpp::UsbEndpoint::is_in)
        .def("is_ep0", &usbipdcpp::UsbEndpoint::is_ep0)
        .def_static("get_ep0_in", &usbipdcpp::UsbEndpoint::get_ep0_in, py::arg("max_packet_size"))
        .def_static("get_default_ep0_in", &usbipdcpp::UsbEndpoint::get_default_ep0_in)
        .def_static("get_ep0_out", &usbipdcpp::UsbEndpoint::get_ep0_out, py::arg("max_packet_size"))
        .def_static("get_default_ep0_out", &usbipdcpp::UsbEndpoint::get_default_ep0_out);

    // UsbEndpoint::Direction 内部枚举
    py::enum_<usbipdcpp::UsbEndpoint::Direction>(m, "EndpointDirection")
        .value("In", usbipdcpp::UsbEndpoint::Direction::In)
        .value("Out", usbipdcpp::UsbEndpoint::Direction::Out)
        .export_values();
}
