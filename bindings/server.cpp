#include <pybind11/pybind11.h>
#include <pybind11/functional.h>
#include <asio/ip/tcp.hpp>
#include "Server.h"
#include "Device.h"

namespace py = pybind11;

void bind_server(py::module_ &m) {
    // ServerNetworkConfig
    py::class_<usbipdcpp::ServerNetworkConfig>(m, "ServerNetworkConfig")
        .def(py::init<>())
        .def_readwrite("socket_recv_buffer_size", &usbipdcpp::ServerNetworkConfig::socket_recv_buffer_size)
        .def_readwrite("socket_send_buffer_size", &usbipdcpp::ServerNetworkConfig::socket_send_buffer_size)
        .def_readwrite("tcp_no_delay", &usbipdcpp::ServerNetworkConfig::tcp_no_delay);

    // Server
    py::class_<usbipdcpp::Server>(m, "Server")
        .def(py::init<>())
        .def(py::init<const usbipdcpp::ServerNetworkConfig &>(), py::arg("network_config"))
        .def("start", [](usbipdcpp::Server &self, const std::string &address, unsigned short port) {
            asio::ip::tcp::endpoint ep(asio::ip::make_address(address), port);
            py::gil_scoped_release release;
            self.start(ep);
        }, py::arg("address"), py::arg("port"))
        .def("stop", [](usbipdcpp::Server &self) {
            py::gil_scoped_release release;
            self.stop();
        })
        .def("add_device", [](usbipdcpp::Server &self, std::shared_ptr<usbipdcpp::UsbDevice> device) {
            return self.add_device(std::move(device));
        })
        .def("has_bound_device", &usbipdcpp::Server::has_bound_device, py::arg("busid"))
        .def("get_session_count", &usbipdcpp::Server::get_session_count)
        .def("print_bound_devices", &usbipdcpp::Server::print_bound_devices)
        .def("register_session_exit_callback", &usbipdcpp::Server::register_session_exit_callback);
}
