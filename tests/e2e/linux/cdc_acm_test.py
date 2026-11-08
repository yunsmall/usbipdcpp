#!/usr/bin/env python3
# cdc_acm 回显测试子脚本：一个 fd 完成 termios 设置 + 写 + 读回。
# 用法：cdc_acm_test.py <串口设备> <要写的字符串>，成功输出读回内容（无换行）。
# 注意：必须 O_RDWR 打开（open 即挂起读请求）；sh 里先写（tee）后开读（dd）
# 两个进程时序不可控，实测读端开晚时收不到回显。
import os
import sys
import termios
import time

dev = sys.argv[1]
msg = sys.argv[2].encode()

fd = None
# 设备刚枚举出来驱动可能未就绪，重试打开 + 设置 termios
for _ in range(10):
    try:
        fd = os.open(dev, os.O_RDWR | os.O_NOCTTY)
        a = termios.tcgetattr(fd)
        a[0] &= ~(termios.ICRNL | termios.IXON)            # 输入转换/软流控
        a[1] &= ~termios.ONLCR                             # 输出 NL->CRNL
        a[2] &= ~termios.CRTSCTS                           # 硬件流控
        a[3] &= ~(termios.ECHO | termios.ICANON | termios.ISIG)  # echo/规范模式
        a[6][termios.VMIN] = 1
        a[6][termios.VTIME] = 0
        termios.tcsetattr(fd, termios.TCSANOW, a)
        break
    except OSError:
        if fd is not None:
            os.close(fd)
            fd = None
        time.sleep(0.5)
if fd is None:
    sys.exit(2)

os.write(fd, msg)
time.sleep(1)  # 等 mock 回显回到 tty 读缓冲
os.set_blocking(fd, False)
buf = b""
deadline = time.time() + 3
while time.time() < deadline and len(buf) < len(msg):
    try:
        chunk = os.read(fd, len(msg) - len(buf))
        if not chunk:
            break
        buf += chunk
    except BlockingIOError:
        time.sleep(0.05)
os.close(fd)
sys.stdout.write(buf.decode(errors="replace"))
