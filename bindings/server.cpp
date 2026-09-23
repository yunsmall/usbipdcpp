#include <pybind11/pybind11.h>
#include <pybind11/functional.h>
#include <asio/ip/tcp.hpp>
#include "usbipdcpp/Server.h"
#include "usbipdcpp/Device.h"

namespace py = pybind11;

// Trampoline 类：允许 Python 继承 ServerObserver 并重写事件回调。
// 回调在服务器的会话/网络/后端线程上执行，pybind11 的 override 宏会自动
// 获取 GIL；主线程在 start/stop 等会等待回调的阻塞调用里已释放 GIL
// （见下面的绑定），两边不会互相等锁
class PyServerObserver : public usbipdcpp::ServerObserver {
public:
    using usbipdcpp::ServerObserver::ServerObserver;

    void on_session_started(std::uint64_t session_id, const std::string &peer) override {
        PYBIND11_OVERRIDE(void, usbipdcpp::ServerObserver, on_session_started, session_id, peer);
    }

    void on_session_ended(std::uint64_t session_id) override {
        PYBIND11_OVERRIDE(void, usbipdcpp::ServerObserver, on_session_ended, session_id);
    }

    void on_device_attached(const std::string &busid) override {
        PYBIND11_OVERRIDE(void, usbipdcpp::ServerObserver, on_device_attached, busid);
    }

    void on_device_released(const std::string &busid, usbipdcpp::DeviceReleaseReason reason) override {
        PYBIND11_OVERRIDE(void, usbipdcpp::ServerObserver, on_device_released, busid, reason);
    }
};

void bind_server(py::module_ &m) {
    // ServerNetworkConfig
    py::class_<usbipdcpp::ServerNetworkConfig>(m, "ServerNetworkConfig")
        .def(py::init<>())
        .def_readwrite("socket_recv_buffer_size", &usbipdcpp::ServerNetworkConfig::socket_recv_buffer_size)
        .def_readwrite("socket_send_buffer_size", &usbipdcpp::ServerNetworkConfig::socket_send_buffer_size)
        .def_readwrite("tcp_no_delay", &usbipdcpp::ServerNetworkConfig::tcp_no_delay);

    // DeviceReleaseReason（on_device_released 的入参）
    py::enum_<usbipdcpp::DeviceReleaseReason>(m, "DeviceReleaseReason")
        .value("ClientDisconnected", usbipdcpp::DeviceReleaseReason::ClientDisconnected)
        .value("DeviceRemoved", usbipdcpp::DeviceReleaseReason::DeviceRemoved)
        .value("ServerStopped", usbipdcpp::DeviceReleaseReason::ServerStopped)
        .export_values();

    // ServerObserver：继承并按需重写关心的事件方法
    py::class_<usbipdcpp::ServerObserver, PyServerObserver>(
            m, "ServerObserver",
            "服务器状态观察者：继承本类并重写关心的事件方法（未重写的不做事）。"
            "回调在服务器的会话/网络/后端线程上执行（不是 Python 主线程），应快速"
            "返回；回调抛出的异常由库捕获并记日志，不会波及服务器")
        .def(py::init<>())
        .def("on_session_started", &usbipdcpp::ServerObserver::on_session_started,
             py::arg("session_id"), py::arg("peer"))
        .def("on_session_ended", &usbipdcpp::ServerObserver::on_session_ended,
             py::arg("session_id"))
        .def("on_device_attached", &usbipdcpp::ServerObserver::on_device_attached,
             py::arg("busid"))
        .def("on_device_released", &usbipdcpp::ServerObserver::on_device_released,
             py::arg("busid"), py::arg("reason"));

    // Server
    py::class_<usbipdcpp::Server>(m, "Server")
        .def(py::init<>())
        .def(py::init<const usbipdcpp::ServerNetworkConfig &>(), py::arg("network_config"))
        .def("start", [](usbipdcpp::Server &self, const std::string &address, unsigned short port) {
            asio::ip::tcp::endpoint ep(asio::ip::make_address(address), port);
            py::gil_scoped_release release;
            // start 不抛异常，错误通过返回值报告（便于无异常环境的嵌入式平台），
            // 语言绑定在这里转回异常抛给调用方
            if (auto ec = self.start(ep); ec) {
                throw std::runtime_error("server start failed: " + ec.message());
            }
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
        .def("add_observer", &usbipdcpp::Server::add_observer, py::arg("observer"),
             py::keep_alive<1, 2>(),
             "注册状态观察者（幂等）。Server 只存裸指针、不接管所有权，"
             "keep_alive 会把观察者对象绑定到本 Server 的存活期上；"
             "需要提前注销时调用 remove_observer")
        .def("remove_observer", &usbipdcpp::Server::remove_observer, py::arg("observer"));
}
