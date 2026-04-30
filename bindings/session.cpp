#include <pybind11/pybind11.h>
#include "Session.h"

namespace py = pybind11;

void bind_session(py::module_ &m) {
    py::class_<usbipdcpp::Session, std::shared_ptr<usbipdcpp::Session>>(m, "Session")
        .def("immediately_stop", &usbipdcpp::Session::immediately_stop);
}