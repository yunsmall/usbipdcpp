#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include <pybind11/stl/filesystem.h>
#include "Device.h"
#include "DeviceHandler/DeviceHandler.h"
#include "Interface.h"
#include "Endpoint.h"
#include "constant.h"
#include "Version.h"

namespace py = pybind11;

void bind_device(py::module_ &m) {
    // Version - BCD 版本号
    py::class_<usbipdcpp::Version>(m, "Version")
        .def(py::init<std::uint16_t>(), py::arg("bcd"))
        .def(py::init<std::uint8_t, std::uint8_t, std::uint8_t>(),
             py::arg("major"), py::arg("minor"), py::arg("patch"))
        .def("__int__", [](const usbipdcpp::Version &v) { return static_cast<std::uint16_t>(v); });

    py::class_<usbipdcpp::UsbDevice, std::shared_ptr<usbipdcpp::UsbDevice>>(m, "UsbDevice")
        .def(py::init<>())
        .def_property("path",
            [](const usbipdcpp::UsbDevice &self) { return self.path.string(); },
            [](usbipdcpp::UsbDevice &self, const std::string &val) { self.path = val; })
        .def_readwrite("busid", &usbipdcpp::UsbDevice::busid)
        .def_readwrite("bus_num", &usbipdcpp::UsbDevice::bus_num)
        .def_readwrite("dev_num", &usbipdcpp::UsbDevice::dev_num)
        .def_readwrite("speed", &usbipdcpp::UsbDevice::speed)
        .def_readwrite("vendor_id", &usbipdcpp::UsbDevice::vendor_id)
        .def_readwrite("product_id", &usbipdcpp::UsbDevice::product_id)
        .def_property("device_bcd",
            [](const usbipdcpp::UsbDevice &self) { return static_cast<std::uint16_t>(self.device_bcd); },
            [](usbipdcpp::UsbDevice &self, std::uint16_t bcd) { self.device_bcd = usbipdcpp::Version{bcd}; })
        .def_readwrite("device_class", &usbipdcpp::UsbDevice::device_class)
        .def_readwrite("device_subclass", &usbipdcpp::UsbDevice::device_subclass)
        .def_readwrite("device_protocol", &usbipdcpp::UsbDevice::device_protocol)
        .def_readwrite("configuration_value", &usbipdcpp::UsbDevice::configuration_value)
        .def_readwrite("num_configurations", &usbipdcpp::UsbDevice::num_configurations)
        .def_property_readonly("interfaces", [](const usbipdcpp::UsbDevice &self) {
            return self.interfaces;
        })
        .def_readwrite("ep0_in", &usbipdcpp::UsbDevice::ep0_in)
        .def_readwrite("ep0_out", &usbipdcpp::UsbDevice::ep0_out)
        .def_readwrite("handler", &usbipdcpp::UsbDevice::handler)
        .def("set_handler", [](usbipdcpp::UsbDevice &self, std::shared_ptr<usbipdcpp::AbstDeviceHandler> handler) {
            self.handler = handler;
        })
        .def("add_interface", [](usbipdcpp::UsbDevice &self, const usbipdcpp::UsbInterface &intf) {
            self.interfaces.push_back(intf);
        }, py::arg("interface"))
        .def("get_interface", [](usbipdcpp::UsbDevice &self, std::size_t index) -> usbipdcpp::UsbInterface & {
            return self.interfaces.at(index);
        }, py::arg("index"), py::return_value_policy::reference_internal);
}
