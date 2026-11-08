#!/usr/bin/env bash
# 复合 HID：一个配置里同时有相对鼠标（接口 0）和键盘（接口 1），
# 验证两个接口各自枚举出正确设备类型、事件流互不串扰（键盘按键不能
# 跑到鼠标节点上）。现有 test_keyboard/test_mouse 都是单接口设备。
set -e
source "$(dirname "$0")/common.sh"

setup_workdir multi_interface_hid
echo "== multi_interface_hid: 启动服务器 + attach"
start_server multi_interface_hid || { report multi_interface_hid; exit 1; }
attach_device 1-1

# 等两种输入设备同时出现（by-path 定位，避免 eventX 编号错位）
KBD=""
MOUSE=""
for _ in $(seq 1 100); do
    KBD=$(ls /dev/input/by-path/*event-kbd 2>/dev/null | grep -i usb | head -1)
    MOUSE=$(ls /dev/input/by-path/*event-mouse 2>/dev/null | grep -i usb | head -1)
    [ -n "$KBD" ] && [ -n "$MOUSE" ] && break
    sleep 0.1
done
assert "键盘节点出现（$KBD）" [ -n "$KBD" ]
assert "鼠标节点出现（$MOUSE）" [ -n "$MOUSE" ]
[ -n "$KBD" ] && [ -n "$MOUSE" ] || { stop_server; report multi_interface_hid; exit 1; }

# 两个接口同时有数据流：并发跑 evtest，各自捕获自己的事件类型
timeout 4 sudo evtest "$KBD" > "$WORK_DIR/kbd.log" 2>&1 &
KBD_PID=$!
timeout 4 sudo evtest "$MOUSE" > "$WORK_DIR/mouse.log" 2>&1 &
MOUSE_PID=$!
wait "$KBD_PID" || true
wait "$MOUSE_PID" || true

KBD_EVENTS=$(grep -c "EV_KEY" "$WORK_DIR/kbd.log" 2>/dev/null || true)
MOUSE_EVENTS=$(grep -c "EV_REL" "$WORK_DIR/mouse.log" 2>/dev/null || true)
assert "键盘接口捕获到按键事件（$KBD_EVENTS 条）" [ "$KBD_EVENTS" -ge 1 ]
assert "鼠标接口捕获到移动事件（$MOUSE_EVENTS 条）" [ "$MOUSE_EVENTS" -ge 1 ]

# 串扰检查：键盘节点的实际按键码在 KEY_A..KEY_Z 范围（mock 每秒按 a-z）；
# 鼠标节点若收到大量按键说明接口路由错了
assert "鼠标节点没有串入键盘按键" bash -c "! grep -q 'KEY_A' '$WORK_DIR/mouse.log'"

stop_server
report multi_interface_hid
