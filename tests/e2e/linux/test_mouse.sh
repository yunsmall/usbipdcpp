#!/usr/bin/env bash
# mouse 端到端：attach 后鼠标输入设备出现。
# 默认只验证枚举，不注入移动；设 HID_EVENTS=1 才启动示例的周期移动并用
# evtest 捕获（会移动宿主机光标，必须确认没人在用这台机器）。
set -e
source "$(dirname "$0")/common.sh"

HID_EVENTS="${HID_EVENTS:-0}"

setup_workdir mouse
echo "== mouse: 启动服务器 + attach"
EXTRA=()
if [ "$HID_EVENTS" != "1" ]; then
    EXTRA=(--no-demo)
fi
start_server mock_mouse "${EXTRA[@]}" || { report mouse; exit 1; }
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

if [ "$HID_EVENTS" = "1" ]; then
    EVENTS=$(timeout 3 sudo evtest "$MOUSE" 2>/dev/null | grep -c "EV_REL" || true)
    assert "捕获到 REL 移动事件（$EVENTS 条）" [ "$EVENTS" -ge 1 ]
else
    echo "  [SKIP] HID_EVENTS=0：不注入鼠标移动（只验证枚举；设 HID_EVENTS=1 开启）"
fi

stop_server
report mouse
