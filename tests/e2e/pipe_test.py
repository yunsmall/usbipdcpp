#!/usr/bin/env python3
# mock_pipe 数据面测试：libusb bulk OUT 发一条消息 → bulk IN 读回显，
# 验证 vendor 类设备的完整传输链路（mock 收到 OUT 原样回显到 IN）。
# IN 方向混有 mock 的周期计数消息（每 2 秒一条），循环读到含本消息内容为止。
import sys
import time

import usb.core
import usb.util

dev = usb.core.find(idVendor=0x1234, idProduct=0x5690)
if dev is None:
    print("ERROR: 找不到 1234:5690（attach 了吗）")
    sys.exit(2)

msg = b"PY_PIPE_TEST_" + str(int(time.time())).encode()
dev.set_configuration()
usb.util.claim_interface(dev, 0)

written = dev.write(0x02, msg, timeout=2000)  # bulk OUT ep 0x02
got = b""
deadline = time.time() + 6
while time.time() < deadline and msg not in got:
    try:
        chunk = dev.read(0x81, 4096, timeout=1000)  # bulk IN ep 0x81
        got += bytes(chunk)
    except usb.core.USBError:
        continue  # 超时继续等（周期消息也在 IN 里）
print("sent:", written, "bytes")
print("got:", got)
print("match:", msg in got)
sys.exit(0 if msg in got else 1)
