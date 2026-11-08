#include <pybind11/pybind11.h>
#include <pybind11/functional.h>
#include <pybind11/stl.h>
#include "usbipdcpp/DeviceHandler/DeviceHandler.h"
#include "usbipdcpp/virtual_device/TransferResponder.h"
#include "usbipdcpp/virtual_device/VirtualInterfaceHandler.h"
#include "usbipdcpp/virtual_device/HidVirtualInterfaceHandler.h"
#include "usbipdcpp/virtual_device/SimpleVirtualDeviceHandler.h"
#include "usbipdcpp/Interface.h"
#include "usbipdcpp/Session.h"

namespace py = pybind11;

// Trampoline 类：允许 Python 继承 C++ 虚类
class PyVirtualInterfaceHandler : public usbipdcpp::VirtualInterfaceHandler {
public:
    using usbipdcpp::VirtualInterfaceHandler::VirtualInterfaceHandler;

    void on_new_connection(usbipdcpp::TransferResponder &responder, usbipdcpp::error_code &ec) override {
        // 手写转发而不是 PYBIND11_OVERRIDE：ec 是纯输出参数，Python 侧的
        // on_new_connection(self, responder) 只接收 responder（与下面 .def
        // 暴露的签名一致）；基类登记 responder 由 Python 侧经
        // super().on_new_connection(responder) 完成
        pybind11::gil_scoped_acquire gil;
        pybind11::function override = pybind11::get_override(this, "on_new_connection");
        if (override) {
            override(pybind11::cast(&responder, pybind11::return_value_policy::reference));
        }
        else {
            // Python 子类没重写时基类登记必须照常发生，否则 responder 永远为空，
            // 之后任何经基类提交应答的路径（如 HID 的 send_input_report）都会
            // 解引用空指针。PYBIND11_OVERRIDE 的默认回退行为在这里必须手写
            usbipdcpp::VirtualInterfaceHandler::on_new_connection(responder, ec);
        }
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

    void on_new_connection(usbipdcpp::TransferResponder &responder, usbipdcpp::error_code &ec) override {
        pybind11::gil_scoped_acquire gil;
        pybind11::function overload = pybind11::get_override(this, "on_new_connection");
        if (overload) {
            overload(pybind11::cast(&responder, pybind11::return_value_policy::reference));
        }
        else {
            // 未重写时补上 HID 基类登记（绑定 input_channel 的会话指针），
            // 否则 send_input_report 的通道没有会话可用（同 VirtualInterfaceHandler）
            usbipdcpp::HidVirtualInterfaceHandler::on_new_connection(responder, ec);
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
    // TransferResponder：传输应答接口。Python 继承 handler 重写
    // on_new_connection 时会收到它，只暴露对 Python 有意义的方法（停止传输），
    // 回包系列（submit/enqueue_ret_*）是 C++ 内部通道，不暴露
    py::class_<usbipdcpp::TransferResponder>(m, "TransferResponder",
                                             "传输应答接口（控制操作用）")
        .def("stop_transfer", &usbipdcpp::TransferResponder::stop_transfer,
             "请求停止传输");

    // VirtualInterfaceHandler - 支持继承
    py::class_<usbipdcpp::VirtualInterfaceHandler, PyVirtualInterfaceHandler,
               std::shared_ptr<usbipdcpp::VirtualInterfaceHandler>>(m, "VirtualInterfaceHandler")
        .def(py::init<usbipdcpp::UsbInterface &, usbipdcpp::StringPool &>(),
             py::arg("handle_interface"), py::arg("string_pool"))
        .def("on_new_connection", [](usbipdcpp::VirtualInterfaceHandler &self,
                                     usbipdcpp::TransferResponder &responder) {
            std::error_code ec;
            self.VirtualInterfaceHandler::on_new_connection(responder, ec);
        }, py::arg("responder"))
        .def("on_disconnection", [](usbipdcpp::VirtualInterfaceHandler &self) {
            // Python 重写 on_disconnection 时按契约在末尾调 super()，这里必须
            // 真正执行基类清理（清 session 指针），否则 responder 悬垂到断连后
            std::error_code ec;
            self.VirtualInterfaceHandler::on_disconnection(ec);
        })
        .def("get_string_interface_value", &usbipdcpp::VirtualInterfaceHandler::get_string_interface_value)
        .def("get_string_interface", &usbipdcpp::VirtualInterfaceHandler::get_string_interface)
        .def("change_string_interface", &usbipdcpp::VirtualInterfaceHandler::change_string_interface,
             py::arg("new_str"));

    // HidVirtualInterfaceHandler - 支持继承
    py::class_<usbipdcpp::HidVirtualInterfaceHandler, PyHidVirtualInterfaceHandler,
               std::shared_ptr<usbipdcpp::HidVirtualInterfaceHandler>>(m, "HidVirtualInterfaceHandler",
                                                                       py::base<usbipdcpp::VirtualInterfaceHandler>())
        .def(py::init<usbipdcpp::UsbInterface &, usbipdcpp::StringPool &>(),
             py::arg("handle_interface"), py::arg("string_pool"))
        // 覆盖基类同名绑定：Python 子类 super().on_new_connection(responder)
        // 必须落到 HID 实现上，否则 input_channel 拿不到会话指针
        .def("on_new_connection", [](usbipdcpp::HidVirtualInterfaceHandler &self,
                                     usbipdcpp::TransferResponder &responder) {
            std::error_code ec;
            self.HidVirtualInterfaceHandler::on_new_connection(responder, ec);
        }, py::arg("responder"))
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
        .def("setup_interface_handlers", &usbipdcpp::SimpleVirtualDeviceHandler::setup_interface_handlers)
        .def("change_string_manufacturer", &usbipdcpp::SimpleVirtualDeviceHandler::change_string_manufacturer,
             py::arg("new_str"))
        .def("change_string_product", &usbipdcpp::SimpleVirtualDeviceHandler::change_string_product,
             py::arg("new_str"))
        .def("change_string_serial", &usbipdcpp::SimpleVirtualDeviceHandler::change_string_serial,
             py::arg("new_str"))
        .def("change_string_configuration", &usbipdcpp::SimpleVirtualDeviceHandler::change_string_configuration,
             py::arg("new_str"));
}
