#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
#include "utils/StringPool.h"

namespace py = pybind11;

void bind_string_pool(py::module_ &m) {
    py::class_<usbipdcpp::StringPool, std::shared_ptr<usbipdcpp::StringPool>>(m, "StringPool")
        .def(py::init<>())
        .def("new_string", &usbipdcpp::StringPool::new_string, py::arg("str"))
        .def("get_string", &usbipdcpp::StringPool::get_string, py::arg("index"))
        .def("remove_string", &usbipdcpp::StringPool::remove_string, py::arg("index"));
}
