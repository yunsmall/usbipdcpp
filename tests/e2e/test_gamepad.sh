#!/usr/bin/env bash
# gamepad 端到端：attach 后手柄输入设备出现，evtest 捕获到事件
# （mock_gamepad 周期改变摇杆/按键状态；evtest 需要 sudo 读 /dev/input）
set -e
source "$(dirname "$0")/common.sh"

setup_workdir gamepad
echo "== gamepad: 启动服务器 + attach"
start_server mock_gamepad || { report gamepad; exit 1; }
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

# evtest 捕获 3 秒：mock 周期产生事件，应捕获 ≥1 条
EVENTS=$(timeout 3 sudo evtest "$PAD" 2>/dev/null | grep -c "EV_ABS\|EV_KEY" || true)
assert "捕获到手柄事件（$EVENTS 条）" [ "$EVENTS" -ge 1 ]

stop_server
report gamepad
