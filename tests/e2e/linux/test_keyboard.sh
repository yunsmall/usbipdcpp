#!/usr/bin/env bash
# keyboard 端到端：attach 后键盘输入设备出现。
# 默认只验证枚举，不注入按键；设 HID_EVENTS=1 才启动示例的自动按键并用
# evtest 捕获（会向宿主机输入 A 键，必须确认没人在用这台机器）。
set -e
source "$(dirname "$0")/common.sh"

HID_EVENTS="${HID_EVENTS:-0}"

setup_workdir keyboard
echo "== keyboard: 启动服务器 + attach"
EXTRA=()
if [ "$HID_EVENTS" != "1" ]; then
    EXTRA=(--no-demo)
fi
start_server mock_keyboard "${EXTRA[@]}" || { report keyboard; exit 1; }
attach_device 1-1

# 等键盘设备：WSL devtmpfs 下 eventX 与 inputX 编号不一致（易拿错节点），
# 用 /dev/input/by-path 符号链接定位真实设备
KBD=""
for _ in $(seq 1 100); do
    KBD=$(ls /dev/input/by-path/*event-kbd 2>/dev/null | grep -i usb | head -1)
    [ -n "$KBD" ] && break
    sleep 0.1
done
assert "键盘设备出现（$KBD）" [ -n "$KBD" ]
[ -n "$KBD" ] || { stop_server; report keyboard; exit 1; }

if [ "$HID_EVENTS" = "1" ]; then
    # evtest 捕获 3 秒：mock 每秒按+放各一次 A（KEY_A），应捕获 ≥2 条事件
    EVENTS=$(timeout 3 sudo evtest "$KBD" 2>/dev/null | grep -c "KEY_A" || true)
    assert "捕获到 KEY_A 事件（$EVENTS 条）" [ "$EVENTS" -ge 2 ]
else
    echo "  [SKIP] HID_EVENTS=0：不注入按键事件（只验证枚举；设 HID_EVENTS=1 开启）"
fi

stop_server
report keyboard
