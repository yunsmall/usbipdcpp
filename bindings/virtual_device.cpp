#include <pybind11/pybind11.h>
#include <pybind11/functional.h>
#include <pybind11/stl.h>
#include "DeviceHandler/DeviceHandler.h"
#include "virtual_device/VirtualInterfaceHandler.h"
#include "virtual_device/HidVirtualInterfaceHandler.h"
#include "virtual_device/SimpleVirtualDeviceHandler.h"
#include "Interface.h"
#include "Session.h"

namespace py = pybind11;

// Trampoline 类：允许 Python 继承 C++ 虚类
class PyVirtualInterfaceHandler : public usbipdcpp::VirtualInterfaceHandler {
public:
    using usbipdcpp::VirtualInterfaceHandler::VirtualInterfaceHandler;

    void on_new_connection(usbipdcpp::Session &current_session, usbipdcpp::error_code &ec) override {
        PYBIND11_OVERRIDE(void, usbipdcpp::VirtualInterfaceHandler, on_new_connection,
                          std::ref(current_session), std::ref(ec));
    }

    void on_disconnection(usbipdcpp::error_code &ec) override {
        PYBIND11_OVERRIDE(void, usbipdcpp::VirtualInterfaceHandler, on_disconnection, std::ref(ec));
    }

    void request_clear_feature(std::uint16_t feature_selector, std::uint32_t *p_status) override {
        PYBIND11_OVERRIDE_PURE(void, usbipdcpp::VirtualInterfaceHandler, request_clear_feature,
                               feature_selector, p_status);
    }

    void request_endpoint_clear_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                        std::uint32_t *p_status) override {
        PYBIND11_OVERRIDE_PURE(void, usbipdcpp::VirtualInterfaceHandler, request_endpoint_clear_feature,
                               feature_selector, ep_address, p_status);
    }

    std::uint8_t request_get_interface(std::uint32_t *p_status) override {
        PYBIND11_OVERRIDE_PURE(std::uint8_t, usbipdcpp::VirtualInterfaceHandler, request_get_interface, p_status);
    }

    void request_set_interface(std::uint16_t alternate_setting, std::uint32_t *p_status) override {
        PYBIND11_OVERRIDE_PURE(void, usbipdcpp::VirtualInterfaceHandler, request_set_interface,
                               alternate_setting, p_status);
    }

    std::uint16_t request_get_status(std::uint32_t *p_status) override {
        PYBIND11_OVERRIDE_PURE(std::uint16_t, usbipdcpp::VirtualInterfaceHandler, request_get_status, p_status);
    }

    std::uint16_t request_endpoint_get_status(std::uint8_t ep_address, std::uint32_t *p_status) override {
        PYBIND11_OVERRIDE_PURE(std::uint16_t, usbipdcpp::VirtualInterfaceHandler, request_endpoint_get_status,
                               ep_address, p_status);
    }

    usbipdcpp::data_type request_get_descriptor(std::uint8_t type, std::uint8_t language_id,
                                                 std::uint16_t descriptor_length, std::uint32_t *p_status) override {
        PYBIND11_OVERRIDE(usbipdcpp::data_type, usbipdcpp::VirtualInterfaceHandler, request_get_descriptor,
                          type, language_id, descriptor_length, p_status);
    }

    void request_set_feature(std::uint16_t feature_selector, std::uint32_t *p_status) override {
        PYBIND11_OVERRIDE_PURE(void, usbipdcpp::VirtualInterfaceHandler, request_set_feature,
                               feature_selector, p_status);
    }

    void request_endpoint_set_feature(std::uint16_t feature_selector, std::uint8_t ep_address,
                                      std::uint32_t *p_status) override {
        PYBIND11_OVERRIDE_PURE(void, usbipdcpp::VirtualInterfaceHandler, request_endpoint_set_feature,
                               feature_selector, ep_address, p_status);
    }

    usbipdcpp::data_type get_class_specific_descriptor() override {
        PYBIND11_OVERRIDE_PURE(usbipdcpp::data_type, usbipdcpp::VirtualInterfaceHandler, get_class_specific_descriptor);
    }
};

// HID Handler Trampoline
class PyHidVirtualInterfaceHandler : public usbipdcpp::HidVirtualInterfaceHandler {
public:
    using usbipdcpp::HidVirtualInterfaceHandler::HidVirtualInterfaceHandler;

    usbipdcpp::data_type get_report_descriptor() override {
        pybind11::gil_scoped_acquire gil;
        pybind11::function override = pybind11::get_override(this, "get_report_descriptor");
        if (override) {
            std::string str = pybind11::cast<std::string>(override());
            return usbipdcpp::data_type(str.begin(), str.end());
        }
        throw pybind11::error_already_set();
    }

    std::uint16_t get_report_descriptor_size() override {
        pybind11::gil_scoped_acquire gil;
        pybind11::function override = pybind11::get_override(this, "get_report_descriptor_size");
        if (override)
            return pybind11::cast<std::uint16_t>(override());
        throw pybind11::error_already_set();
    }

    void on_new_connection(usbipdcpp::Session &current_session, usbipdcpp::error_code &ec) override {
        session = &current_session;
        pybind11::gil_scoped_acquire gil;
        pybind11::function overload = pybind11::get_override(this, "on_new_connection");
        if (overload) {
            pybind11::tuple args(1);
            args[0] = pybind11::cast(&current_session, pybind11::return_value_policy::reference);
            overload(*args);
        }
    }

    void on_disconnection(usbipdcpp::error_code &ec) override {
        {
            pybind11::gil_scoped_acquire gil;
            pybind11::function overload = pybind11::get_override(this, "on_disconnection");
            if (overload)
                overload();
        }
        HidVirtualInterfaceHandler::on_disconnection(ec);
    }
};

void bind_virtual_device(py::module_ &m) {
    // VirtualInterfaceHandler - 支持继承
    py::class_<usbipdcpp::VirtualInterfaceHandler, PyVirtualInterfaceHandler,
               std::shared_ptr<usbipdcpp::VirtualInterfaceHandler>>(m, "VirtualInterfaceHandler")
        .def(py::init<usbipdcpp::UsbInterface &, usbipdcpp::StringPool &>(),
             py::arg("handle_interface"), py::arg("string_pool"))
        .def("on_new_connection", [](usbipdcpp::VirtualInterfaceHandler &self, usbipdcpp::Session &session) {
            std::error_code ec;
            self.VirtualInterfaceHandler::on_new_connection(session, ec);
        })
        .def("on_disconnection", [](usbipdcpp::VirtualInterfaceHandler &self) {
            // 实际清理由 trampoline 在 Python 回调后自动执行
        })
        .def("get_string_interface_value", &usbipdcpp::VirtualInterfaceHandler::get_string_interface_value)
        .def("get_string_interface", &usbipdcpp::VirtualInterfaceHandler::get_string_interface);

    // HidVirtualInterfaceHandler - 支持继承
    py::class_<usbipdcpp::HidVirtualInterfaceHandler, PyHidVirtualInterfaceHandler,
               std::shared_ptr<usbipdcpp::HidVirtualInterfaceHandler>>(m, "HidVirtualInterfaceHandler",
                                                                       py::base<usbipdcpp::VirtualInterfaceHandler>())
        .def(py::init<usbipdcpp::UsbInterface &, usbipdcpp::StringPool &>(),
             py::arg("handle_interface"), py::arg("string_pool"))
        .def("get_report_descriptor", &usbipdcpp::HidVirtualInterfaceHandler::get_report_descriptor)
        .def("get_report_descriptor_size", &usbipdcpp::HidVirtualInterfaceHandler::get_report_descriptor_size)
        .def("send_input_report", [](usbipdcpp::HidVirtualInterfaceHandler &self, py::bytes data) {
            std::string str = data;
            self.send_input_report(asio::buffer(str.data(), str.size()));
        });

    // AbstDeviceHandler - 基类（不直接构造，仅供继承链注册）
    py::class_<usbipdcpp::AbstDeviceHandler, std::shared_ptr<usbipdcpp::AbstDeviceHandler>>(
        m, "AbstDeviceHandler");

    // SimpleVirtualDeviceHandler - 设备级 Handler
    py::class_<usbipdcpp::SimpleVirtualDeviceHandler, usbipdcpp::AbstDeviceHandler,
               std::shared_ptr<usbipdcpp::SimpleVirtualDeviceHandler>>(
        m, "SimpleVirtualDeviceHandler")
        .def(py::init<usbipdcpp::UsbDevice &, usbipdcpp::StringPool &>(),
             py::arg("handle_device"), py::arg("string_pool"))
        .def("setup_interface_handlers", &usbipdcpp::SimpleVirtualDeviceHandler::setup_interface_handlers);
}
