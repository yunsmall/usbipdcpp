#!/usr/bin/env bash
# cdc_throttle 端到端：attach 后串口出现，读到周期计数上报
# （mock_cdc_throttle 每 window/2 毫秒把累计 '1' 计数以「数字\n」发回主机）
set -e
source "$(dirname "$0")/common.sh"

setup_workdir cdc_throttle
echo "== cdc_throttle: 启动服务器 + attach"
# -l 64 -w 5：每 5 秒窗口最多收 64 字节，超了 NAK 到窗口结束（背压演示）
start_server mock_cdc_throttle -l 64 -w 5 || { report cdc_throttle; exit 1; }
attach_device 1-1

TTY=$(wait_dev "/dev/ttyACM" 10) || true
assert "串口出现（$TTY）" [ -n "$TTY" ]
[ -n "$TTY" ] || { stop_server; report cdc_throttle; exit 1; }

# 读 3 秒：mock 周期上报计数（数字\n），读到即通过
DATA=$(timeout 3 sudo cat "$TTY" 2>/dev/null || true)
assert "收到计数上报" grep -qE '^[0-9]+$' <<<"$DATA"

stop_server
report cdc_throttle
