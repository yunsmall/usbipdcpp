"""
翻转鼠标左键示例：通过 Python 继承 HidVirtualInterfaceHandler 实现自定义 HID 鼠标设备，
每秒自动翻转（按下/释放）左键。

Usage: python examples/python/flip_left_button.py

Prerequisites:
    cmake -DUSBIPDCPP_BUILD_PYTHON_BINDINGS=ON -B build
    cmake --build build
    set PYTHONPATH=build/python_package  (Windows)
    export PYTHONPATH=build/python_package  (Linux)
"""

import threading
import usbipdcpp


# HID 报告描述符：相对坐标鼠标（3 键 + X + Y + 滚轮）
REPORT_DESCRIPTOR = bytes([
    0x05, 0x01,        # Usage Page (Generic Desktop)
    0x09, 0x02,        # Usage (Mouse)
    0xA1, 0x01,        # Collection (Application)
    0x09, 0x01,        #   Usage (Pointer)
    0xA1, 0x00,        #   Collection (Physical)

    # 按钮 (3 键 + 5 位填充)
    0x05, 0x09,        #   Usage Page (Button)
    0x19, 0x01,        #   Usage Minimum (Button 1)
    0x29, 0x03,        #   Usage Maximum (Button 3)
    0x15, 0x00,        #   Logical Minimum (0)
    0x25, 0x01,        #   Logical Maximum (1)
    0x95, 0x03,        #   Report Count (3)
    0x75, 0x01,        #   Report Size (1)
    0x81, 0x02,        #   Input (Data,Var,Abs)
    0x95, 0x05,        #   Report Count (5) — 填充
    0x81, 0x03,        #   Input (Const,Var,Abs)

    # X, Y 相对坐标
    0x05, 0x01,        #   Usage Page (Generic Desktop)
    0x09, 0x30,        #   Usage (X)
    0x09, 0x31,        #   Usage (Y)
    0x15, 0x81,        #   Logical Minimum (-127)
    0x25, 0x7F,        #   Logical Maximum (127)
    0x75, 0x08,        #   Report Size (8)
    0x95, 0x02,        #   Report Count (2)
    0x81, 0x06,        #   Input (Data,Var,Rel)

    # 滚轮
    0x09, 0x38,        #   Usage (Wheel)
    0x15, 0x81,        #   Logical Minimum (-127)
    0x25, 0x7F,        #   Logical Maximum (127)
    0x75, 0x08,        #   Report Size (8)
    0x95, 0x01,        #   Report Count (1)
    0x81, 0x06,        #   Input (Data,Var,Rel)

    0xC0,              # End Collection (Physical)
    0xC0,              # End Collection (Application)
])

# 鼠标报告: [按钮(1B), X(1B), Y(1B), 滚轮(1B)]
BTN_NONE   = b'\x00\x00\x00\x00'
BTN_LEFT   = b'\x01\x00\x00\x00'
BTN_RIGHT  = b'\x02\x00\x00\x00'
BTN_MIDDLE = b'\x04\x00\x00\x00'


class FlipMouseHandler(usbipdcpp.HidVirtualInterfaceHandler):
    """自定义 HID 鼠标：每秒翻转一次左键状态"""

    def __init__(self, interface, string_pool):
        super().__init__(interface, string_pool)
        self._running = False
        self._thread = None
        self._left_pressed = False

    def get_report_descriptor(self):
        return REPORT_DESCRIPTOR

    def get_report_descriptor_size(self):
        return len(REPORT_DESCRIPTOR)

    def on_new_connection(self, session):
        super().on_new_connection(session)
        self._running = True
        self._thread = threading.Thread(target=self._flip_loop, daemon=True)
        self._thread.start()
        print("客户端已连接，开始翻转左键...")

    def on_disconnection(self):
        self._running = False
        if self._thread:
            self._thread.join()
        super().on_disconnection()
        print("客户端已断开")

    def _flip_loop(self):
        while self._running:
            self._left_pressed = not self._left_pressed
            report = BTN_LEFT if self._left_pressed else BTN_NONE
            self.send_input_report(report)
            state = "按下" if self._left_pressed else "释放"
            print(f"左键: {state}")
            threading.Event().wait(1.0)


def main():
    string_pool = usbipdcpp.StringPool()

    device = usbipdcpp.UsbDevice()
    device.path = "/usbipdcpp/flip_mouse"
    device.busid = "1-1"
    device.bus_num = 1
    device.dev_num = 1
    device.speed = usbipdcpp.UsbSpeed.Low
    device.vendor_id = 0x1234
    device.product_id = 0x5680
    device.device_bcd = 0x0100
    device.device_class = 0x00
    device.device_subclass = 0x00
    device.device_protocol = 0x00
    device.configuration_value = 1
    device.num_configurations = 1
    device.ep0_in = usbipdcpp.UsbEndpoint.get_default_ep0_in()
    device.ep0_out = usbipdcpp.UsbEndpoint.get_default_ep0_out()

    interface = usbipdcpp.UsbInterface()
    interface.interface_class = usbipdcpp.ClassCode.HID
    interface.interface_subclass = 0x00
    interface.interface_protocol = 0x00  # None (引导设备，不限协议)

    endpoint = usbipdcpp.UsbEndpoint()
    endpoint.address = 0x81
    endpoint.attributes = 0x03  # Interrupt
    endpoint.max_packet_size = 8
    endpoint.interval = 10
    interface.add_endpoint(endpoint)

    device.add_interface(interface)

    # 在 Python 中创建自定义 Handler
    iface_in_device = device.get_interface(0)
    mouse = FlipMouseHandler(iface_in_device, string_pool)
    iface_in_device.set_handler(mouse)

    device_handler = usbipdcpp.SimpleVirtualDeviceHandler(device, string_pool)
    device.set_handler(device_handler)
    device_handler.setup_interface_handlers()

    server = usbipdcpp.Server()
    server.add_device(device)
    server.start("0.0.0.0", 54324)

    print("翻转鼠标服务器已启动，端口 54324")
    print("Connect with: usbip attach -r <host> -b 1-1")
    print("按 Enter 退出...")
    input()

    server.stop()
    print("服务器已停止")


if __name__ == "__main__":
    main()
