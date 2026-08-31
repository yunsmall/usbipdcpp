#!/usr/bin/env bash
# mouse 端到端：attach 后鼠标输入设备出现，evtest 捕获到移动事件
# （mock_mouse 周期产生移动事件；evtest 需要 sudo 读 /dev/input）
set -e
source "$(dirname "$0")/common.sh"

setup_workdir mouse
echo "== mouse: 启动服务器 + attach"
start_server mock_mouse || { report mouse; exit 1; }
attach_device 1-1

# 等鼠标设备（by-path 定位，避免 eventX 编号错位）
MOUSE=""
for _ in $(seq 1 100); do
    MOUSE=$(ls /dev/input/by-path/*event-mouse 2>/dev/null | grep -i usb | head -1)
    [ -n "$MOUSE" ] && break
    sleep 0.1
done
assert "鼠标设备出现（$MOUSE）" [ -n "$MOUSE" ]
[ -n "$MOUSE" ] || { stop_server; report mouse; exit 1; }

# evtest 捕获 3 秒：mock 周期产生 REL 移动事件，应捕获 ≥1 条
EVENTS=$(timeout 3 sudo evtest "$MOUSE" 2>/dev/null | grep -c "EV_REL" || true)
assert "捕获到 REL 移动事件（$EVENTS 条）" [ "$EVENTS" -ge 1 ]

stop_server
report mouse
