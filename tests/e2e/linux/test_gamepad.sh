#!/usr/bin/env bash
# gamepad 端到端：attach 后手柄输入设备出现。
# 默认只验证枚举，不注入事件；设 HID_EVENTS=1 才启动示例的周期状态变化
# 并用 evtest 捕获（会影响宿主机的手柄输入设备）。
set -e
source "$(dirname "$0")/common.sh"

HID_EVENTS="${HID_EVENTS:-0}"

setup_workdir gamepad
echo "== gamepad: 启动服务器 + attach"
EXTRA=()
if [ "$HID_EVENTS" != "1" ]; then
    EXTRA=(--no-demo)
fi
start_server mock_gamepad "${EXTRA[@]}" || { report gamepad; exit 1; }
attach_device 1-1

# 等手柄设备（by-path 定位）
PAD=""
for _ in $(seq 1 100); do
    PAD=$(ls /dev/input/by-path/*event-joystick 2>/dev/null | grep -i usb | head -1)
    [ -n "$PAD" ] && break
    sleep 0.1
done
assert "手柄设备出现（$PAD）" [ -n "$PAD" ]
[ -n "$PAD" ] || { stop_server; report gamepad; exit 1; }

if [ "$HID_EVENTS" = "1" ]; then
    EVENTS=$(timeout 3 sudo evtest "$PAD" 2>/dev/null | grep -c "EV_ABS\|EV_KEY" || true)
    assert "捕获到手柄事件（$EVENTS 条）" [ "$EVENTS" -ge 1 ]
else
    echo "  [SKIP] HID_EVENTS=0：不注入手柄事件（只验证枚举；设 HID_EVENTS=1 开启）"
fi

stop_server
report gamepad
