#include <pybind11/pybind11.h>
#include <pybind11/functional.h>
#include <pybind11/stl.h>
#include "virtual_device/devices/AbsoluteMouseHandler.h"
#include "Interface.h"

namespace py = pybind11;

void bind_absolute_mouse(py::module_ &m) {
    // ButtonState 结构体
    py::class_<usbipdcpp::AbsoluteMouseHandler::ButtonState>(m, "ButtonState")
        .def(py::init<>())
        .def_readwrite("left_button", &usbipdcpp::AbsoluteMouseHandler::ButtonState::left_button)
        .def_readwrite("right_button", &usbipdcpp::AbsoluteMouseHandler::ButtonState::right_button)
        .def_readwrite("middle_button", &usbipdcpp::AbsoluteMouseHandler::ButtonState::middle_button)
        .def_readwrite("wheel", &usbipdcpp::AbsoluteMouseHandler::ButtonState::wheel);

    // AbsoluteMouseHandler
    py::class_<usbipdcpp::AbsoluteMouseHandler, usbipdcpp::HidVirtualInterfaceHandler,
               std::shared_ptr<usbipdcpp::AbsoluteMouseHandler>>(m, "AbsoluteMouseHandler")
        .def(py::init<usbipdcpp::UsbInterface &, usbipdcpp::StringPool &, int, int>(),
             py::arg("handle_interface"), py::arg("string_pool"),
             py::arg("screen_width") = 1920, py::arg("screen_height") = 1080)
        // 屏幕配置
        .def("set_screen_size", &usbipdcpp::AbsoluteMouseHandler::set_screen_size,
             py::arg("width"), py::arg("height"))
        .def("set_screen_bounds", &usbipdcpp::AbsoluteMouseHandler::set_screen_bounds,
             py::arg("x1"), py::arg("y1"), py::arg("x2"), py::arg("y2"))
        .def("get_screen_width", &usbipdcpp::AbsoluteMouseHandler::get_screen_width)
        .def("get_screen_height", &usbipdcpp::AbsoluteMouseHandler::get_screen_height)
        .def("get_screen_x1", &usbipdcpp::AbsoluteMouseHandler::get_screen_x1)
        .def("get_screen_y1", &usbipdcpp::AbsoluteMouseHandler::get_screen_y1)
        .def("get_screen_x2", &usbipdcpp::AbsoluteMouseHandler::get_screen_x2)
        .def("get_screen_y2", &usbipdcpp::AbsoluteMouseHandler::get_screen_y2)
        // 鼠标位置（屏幕坐标）
        .def("set_position", &usbipdcpp::AbsoluteMouseHandler::set_position,
             py::arg("x"), py::arg("y"))
        .def("move", &usbipdcpp::AbsoluteMouseHandler::move,
             py::arg("from_x"), py::arg("from_y"), py::arg("to_x"), py::arg("to_y"),
             py::arg("duration_ms"), py::arg("callback") = nullptr)
        .def("humanized_move", &usbipdcpp::AbsoluteMouseHandler::humanized_move,
             py::arg("from_x"), py::arg("from_y"), py::arg("to_x"), py::arg("to_y"),
             py::arg("duration_ms"), py::arg("callback") = nullptr)
        // 鼠标位置（HID 坐标）
        .def("set_position_raw", &usbipdcpp::AbsoluteMouseHandler::set_position_raw,
             py::arg("x"), py::arg("y"))
        .def("move_raw", &usbipdcpp::AbsoluteMouseHandler::move_raw,
             py::arg("from_x"), py::arg("from_y"), py::arg("to_x"), py::arg("to_y"),
             py::arg("duration_ms"), py::arg("callback") = nullptr)
        // 按钮
        .def("set_left_button", &usbipdcpp::AbsoluteMouseHandler::set_left_button, py::arg("pressed"))
        .def("set_right_button", &usbipdcpp::AbsoluteMouseHandler::set_right_button, py::arg("pressed"))
        .def("set_middle_button", &usbipdcpp::AbsoluteMouseHandler::set_middle_button, py::arg("pressed"))
        .def("set_wheel", &usbipdcpp::AbsoluteMouseHandler::set_wheel, py::arg("delta"))
        // 点击
        .def("left_click", &usbipdcpp::AbsoluteMouseHandler::left_click,
             py::arg("x"), py::arg("y"), py::arg("delay_ms") = 50)
        .def("right_click", &usbipdcpp::AbsoluteMouseHandler::right_click,
             py::arg("x"), py::arg("y"), py::arg("delay_ms") = 50)
        .def("middle_click", &usbipdcpp::AbsoluteMouseHandler::middle_click,
             py::arg("x"), py::arg("y"), py::arg("delay_ms") = 50)
        .def("double_click", &usbipdcpp::AbsoluteMouseHandler::double_click,
             py::arg("x"), py::arg("y"), py::arg("delay_ms") = 100)
        // 拖动
        .def("drag", &usbipdcpp::AbsoluteMouseHandler::drag,
             py::arg("from_x"), py::arg("from_y"), py::arg("to_x"), py::arg("to_y"),
             py::arg("duration_ms"), py::arg("callback") = nullptr)
        .def("humanized_drag", &usbipdcpp::AbsoluteMouseHandler::humanized_drag,
             py::arg("from_x"), py::arg("from_y"), py::arg("to_x"), py::arg("to_y"),
             py::arg("duration_ms"), py::arg("callback") = nullptr)
        // 坐标转换
        .def("screen_to_hid", &usbipdcpp::AbsoluteMouseHandler::screen_to_hid,
             py::arg("screen_x"), py::arg("screen_y"))
        .def("hid_to_screen", &usbipdcpp::AbsoluteMouseHandler::hid_to_screen,
             py::arg("hid_x"), py::arg("hid_y"))
        // 状态查询
        .def("get_button_state", &usbipdcpp::AbsoluteMouseHandler::get_button_state)
        .def("wait_for_client", &usbipdcpp::AbsoluteMouseHandler::wait_for_client,
             py::arg("timeout_ms") = -1);

    // 常量
    m.attr("HID_MAX") = usbipdcpp::AbsoluteMouseHandler::HID_MAX;
}
