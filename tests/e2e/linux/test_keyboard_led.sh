#!/usr/bin/env bash
# 键盘 LED 端到端：内核按报告描述符创建 capslock LED class 设备，主机写
# brightness 触发 HID SET_REPORT(Output)，服务器侧 KeyboardHandler 的
# LED 状态跟着变化（示例在状态变化时打日志）。覆盖 Caps Lock 灯的实际链路。
set -e
source "$(dirname "$0")/common.sh"

setup_workdir keyboard_led
echo "== keyboard_led: 启动服务器（--no-demo，避免向宿主机注入按键）+ attach"
start_server mock_keyboard --no-demo || { report keyboard_led; exit 1; }
attach_device 1-1

KBD=""
for _ in $(seq 1 100); do
    KBD=$(ls /dev/input/by-path/*event-kbd 2>/dev/null | grep -i usb | head -1)
    [ -n "$KBD" ] && break
    sleep 0.1
done
assert "键盘设备出现（$KBD）" [ -n "$KBD" ]

# LED class 设备是内核解析报告描述符后创建的：没有它说明描述符缺 LED Output
LED=""
for _ in $(seq 1 100); do
    LED=$(ls /sys/class/leds/ 2>/dev/null | grep -i "caps" | head -1)
    [ -n "$LED" ] && break
    sleep 0.1
done
assert "主机侧 capslock LED 设备出现（$LED）" [ -n "$LED" ]
[ -n "$LED" ] || { stop_server; report keyboard_led; exit 1; }

echo "== keyboard_led: 点亮 Caps Lock（SET_REPORT → 服务器 LED 状态 0x02）"
echo 1 | sudo tee "/sys/class/leds/$LED/brightness" > /dev/null
ON=0
for _ in $(seq 1 50); do
    if grep -q "LED status changed: 0x02" "$LOG_DIR/mock_keyboard.log"; then
        ON=1
        break
    fi
    sleep 0.1
done
assert "服务器收到 LED 点亮状态（日志 0x02）" [ "$ON" = "1" ]

echo "== keyboard_led: 熄灭 Caps Lock（LED 状态回到 0x00）"
echo 0 | sudo tee "/sys/class/leds/$LED/brightness" > /dev/null
OFF=0
for _ in $(seq 1 50); do
    if grep -q "LED status changed: 0x00" "$LOG_DIR/mock_keyboard.log"; then
        OFF=1
        break
    fi
    sleep 0.1
done
assert "服务器收到 LED 熄灭状态（日志 0x00）" [ "$OFF" = "1" ]

stop_server
report keyboard_led
