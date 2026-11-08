#!/usr/bin/env bash
# 多设备并发：一个服务器导出 3 台设备，验证 list 全量可见、三台可同时
# import（vhci 端口互不干扰）、主机侧三组 HID 输入节点同时存在。
# 现有测试全部是单设备，覆盖不到设备列表与多会话并发的实际使用场景。
set -e
source "$(dirname "$0")/common.sh"

setup_workdir multi_devices
echo "== multi_devices: 启动服务器（3 台设备）"
start_server multi_devices -n 3 || { report multi_devices; exit 1; }

# 设备列表：三台都要出现在 OP_REP_DEVLIST 里
LIST=$(sudo usbip -t "$PORT" list -r "$HOST_IP" 2>/dev/null || true)
for busid in 1-1 1-2 1-3; do
    assert "设备列表包含 $busid" grep -q "$busid" <<<"$LIST"
done

echo "== multi_devices: 同时 attach 三台设备"
for busid in 1-1 1-2 1-3; do
    attach_device "$busid"
done

# attach 返回后内核侧的导入状态是异步注册的，轮询等三个端口都出现
IMPORTED=0
for _ in $(seq 1 100); do
    PORTS=$(sudo usbip port 2>/dev/null || true)
    IMPORTED=$(grep -c -- "-> usbip://$HOST_IP:$PORT/" <<<"$PORTS" || true)
    [ "$IMPORTED" -ge 3 ] && break
    sleep 0.1
done
assert "三台设备均已导入（$IMPORTED 个端口）" [ "$IMPORTED" -ge 3 ]

# 三台都是 HID 设备，主机侧应出现三个各自独立的 input 节点
NODES=0
for _ in $(seq 1 100); do
    NODES=$(ls /dev/input/by-path/*event* 2>/dev/null | grep -c usb || true)
    [ "$NODES" -ge 3 ] && break
    sleep 0.1
done
assert "三组 HID 输入节点同时存在（$NODES 个）" [ "$NODES" -ge 3 ]

# 逐台 detach：前一台断开不能影响其余设备（同样等异步状态稳定）
sudo usbip detach -p 0 2>/dev/null || true
REMAIN=0
for _ in $(seq 1 100); do
    REMAIN=$(grep -c -- "-> usbip://$HOST_IP:$PORT/" <<<"$(sudo usbip port 2>/dev/null || true)" || true)
    [ "$REMAIN" -ge 2 ] && [ "$REMAIN" -lt 3 ] && break
    sleep 0.1
done
assert "detach 一台后其余设备仍在（$REMAIN 个端口）" [ "$REMAIN" -ge 2 ]

stop_server
report multi_devices
