#!/usr/bin/env bash
# keyboard 端到端：attach 后键盘输入设备出现，evtest 捕获到按键事件
# （mock_keyboard 每秒按下/释放 A 键；evtest 需要 sudo 读 /dev/input）
set -e
source "$(dirname "$0")/common.sh"

setup_workdir keyboard
echo "== keyboard: 启动服务器 + attach"
start_server mock_keyboard || { report keyboard; exit 1; }
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

# evtest 捕获 3 秒：mock 每秒按+放各一次 A（KEY_A），应捕获 ≥2 条事件
EVENTS=$(timeout 3 sudo evtest "$KBD" 2>/dev/null | grep -c "KEY_A" || true)
assert "捕获到 KEY_A 事件（$EVENTS 条）" [ "$EVENTS" -ge 2 ]

stop_server
report keyboard
