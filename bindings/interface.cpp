#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include "Interface.h"
#include "Endpoint.h"
#include "virtual_device/VirtualInterfaceHandler.h"

namespace py = pybind11;

void bind_interface(py::module_ &m) {
    py::class_<usbipdcpp::UsbInterface>(m, "UsbInterface")
        .def(py::init<>())
        .def_readwrite("interface_class", &usbipdcpp::UsbInterface::interface_class)
        .def_readwrite("interface_subclass", &usbipdcpp::UsbInterface::interface_subclass)
        .def_readwrite("interface_protocol", &usbipdcpp::UsbInterface::interface_protocol)
        .def_property_readonly("endpoints", [](const usbipdcpp::UsbInterface &self) {
            return self.endpoints;
        })
        .def("set_handler", [](usbipdcpp::UsbInterface &self, std::shared_ptr<usbipdcpp::VirtualInterfaceHandler> handler) {
            self.handler = handler;
        })
        .def("add_endpoint", [](usbipdcpp::UsbInterface &self, const usbipdcpp::UsbEndpoint &ep) {
            self.endpoints.push_back(ep);
        }, py::arg("endpoint"));
}
