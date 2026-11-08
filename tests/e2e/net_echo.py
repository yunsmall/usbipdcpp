#!/usr/bin/env python3
# TCP 大包回显校验：连设备侧 echo 服务，发送指定长度的模式数据，收满同长度
# 后逐字节比对。验证虚拟网卡的多段/持续传输路径（现有脚本只发 5 字节）。
import socket
import sys

if len(sys.argv) != 4:
    print("usage: net_echo.py <host> <port> <bytes>")
    sys.exit(2)

host = sys.argv[1]
port = int(sys.argv[2])
size = int(sys.argv[3])
payload = bytes((i * 37 + 11) & 0xFF for i in range(size))

sock = socket.create_connection((host, port), timeout=5)
sock.settimeout(5)
sock.sendall(payload)

got = b""
while len(got) < size:
    chunk = sock.recv(min(4096, size - len(got)))
    if not chunk:
        break
    got += chunk
sock.close()

if got != payload:
    print(f"MISMATCH sent={len(payload)} got={len(got)}")
    sys.exit(1)
print(f"ECHO_OK {size} bytes")
