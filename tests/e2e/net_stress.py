#!/usr/bin/env python3
# 持续 TCP 回显压测：按指定时长循环建连、发固定长度模式数据、收满比对。
# 统计成功/失败轮数与总字节数；任一轮失败即判 STRESS_FAIL。
import socket
import sys
import time

if len(sys.argv) != 5:
    print("usage: net_stress.py <host> <port> <bytes> <duration_seconds>")
    sys.exit(2)

host = sys.argv[1]
port = int(sys.argv[2])
size = int(sys.argv[3])
duration = float(sys.argv[4])
payload = bytes((i * 37 + 11) & 0xFF for i in range(size))

deadline = time.time() + duration
ok = 0
fail = 0
total = 0
while time.time() < deadline:
    try:
        sock = socket.create_connection((host, port), timeout=3)
        sock.settimeout(3)
        sock.sendall(payload)
        got = b""
        while len(got) < size:
            chunk = sock.recv(min(4096, size - len(got)))
            if not chunk:
                break
            got += chunk
        sock.close()
        if got == payload:
            ok += 1
            total += size
        else:
            fail += 1
    except OSError:
        fail += 1

print(f"rounds={ok + fail} ok={ok} fail={fail} bytes={total}")
if ok > 0 and fail == 0:
    print("STRESS_OK")
    sys.exit(0)
print("STRESS_FAIL")
sys.exit(1)
