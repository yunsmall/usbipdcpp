"""
Usage example: python examples/python/test_absolute_mouse.py

Prerequisites:
    cmake -DUSBIPDCPP_BUILD_PYTHON_BINDINGS=ON -B build
    cmake --build build
    set PYTHONPATH=build/python_package  (Windows)
    export PYTHONPATH=build/python_package  (Linux)
"""

import usbipdcpp


def main():
    # 创建字符串池
    string_pool = usbipdcpp.StringPool()

    # 创建设备（先添加接口，再在接口上创建 handler）
    device = usbipdcpp.UsbDevice()
    device.path = "/usbipdcpp/absolute_mouse"
    device.busid = "1-1"
    device.bus_num = 1
    device.dev_num = 1
    device.speed = usbipdcpp.UsbSpeed.Full
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

    # 创建 HID 鼠标接口
    interface = usbipdcpp.UsbInterface()
    interface.interface_class = usbipdcpp.ClassCode.HID
    interface.interface_subclass = 0x01
    interface.interface_protocol = 0x02  # Mouse

    # 添加中断端点
    endpoint = usbipdcpp.UsbEndpoint()
    endpoint.address = 0x81  # IN endpoint
    endpoint.attributes = 0x03  # Interrupt
    endpoint.max_packet_size = 8
    endpoint.interval = 1
    interface.add_endpoint(endpoint)

    # 接口加入设备
    device.add_interface(interface)

    # 在设备内的接口上创建 handler（确保 handler 引用的是 vector 内的接口）
    iface_in_device = device.get_interface(0)
    mouse = usbipdcpp.AbsoluteMouseHandler(iface_in_device, string_pool, 1920, 1080)
    iface_in_device.set_handler(mouse)

    # 设置设备 handler 并初始化接口
    device_handler = usbipdcpp.SimpleVirtualDeviceHandler(device, string_pool)
    device.set_handler(device_handler)
    device_handler.setup_interface_handlers()

    # 启动服务器
    server = usbipdcpp.Server()
    server.add_device(device)
    server.start("0.0.0.0", 54327)

    print("Server started on port 54327, busid: 1-1")
    print("Connect with: usbip attach -r <host> -b 1-1")
    print("Waiting for client to connect...")

    if not mouse.wait_for_client():
        print("Timeout waiting for client")
        server.stop()
        return

    print("Client connected!")
    print()
    print("Commands: p, pos <x y>, click, move, human, drag, q, h")

    try:
        cx = mouse.get_screen_width() // 2
        cy = mouse.get_screen_height() // 2
        mouse.set_position(cx, cy)

        while True:
            try:
                parts = input("> ").strip().split()
            except EOFError:
                break
            if not parts:
                continue

            cmd = parts[0]
            if cmd == "p":
                state = mouse.get_button_state()
                print(f"按钮: L={state.left_button} R={state.right_button} "
                      f"M={state.middle_button} W={state.wheel}")
                print(f"屏幕: {mouse.get_screen_width()}x{mouse.get_screen_height()}")
            elif cmd == "pos" and len(parts) >= 3:
                x, y = int(parts[1]), int(parts[2])
                mouse.set_position(x, y)
                print(f"Moved to ({x}, {y})")
            elif cmd == "click":
                mouse.left_click(cx, cy)
                print("Left click at center")
            elif cmd == "move":
                mouse.move(100, 100, cx, cy, 1000)
                print("Smooth move done")
            elif cmd == "human":
                mouse.humanized_move(100, 100, cx, cy, 1500)
                print("Humanized move done")
            elif cmd == "drag":
                mouse.drag(cx, cy, cx + 300, cy + 300, 1000)
                print("Drag done")
            elif cmd == "q":
                break
            elif cmd == "h":
                print("Commands: p, pos <x y>, click, move, human, drag, q, h")
            else:
                print(f"Unknown command: {cmd}")

    finally:
        server.stop()
        print("Server stopped")


if __name__ == "__main__":
    main()
