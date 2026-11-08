#!/usr/bin/env bash
# 重连端到端：同一设备 detach 后重新 attach 仍可用；服务器停止后主机侧
# 设备节点消失、端口释放，重启服务器后重新 attach 仍可用。
# 覆盖用户最高频的异常路径（拔插/服务器重启），现有测试每个只 attach 一次。
set -e
source "$(dirname "$0")/common.sh"

# 等待所有串口字符节点消失（detach 后内核异步释放，不能立刻断言）
wait_tty_gone() { # wait_tty_gone <超时秒>
    local timeout="${1:-10}"
    for _ in $(seq 1 $((timeout * 10))); do
        if [ -z "$(ls /dev/ttyACM* 2>/dev/null | while read -r d; do [ -c "$d" ] && echo "$d"; done | head -1)" ]; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

setup_workdir reconnect
echo "== reconnect: 启动服务器 + attach"
start_server mock_cdc_acm || { report reconnect; exit 1; }
attach_device 1-1

TTY=$(wait_dev "/dev/ttyACM" 10) || true
assert "首次 attach 串口出现（$TTY）" [ -n "$TTY" ]
[ -n "$TTY" ] || { stop_server; report reconnect; exit 1; }
sleep 1

MSG1="RECONNECT_1_$(date +%s)"
GOT1=$(timeout 12 sudo python3 "$E2E_DIR/cdc_acm_test.py" "$TTY" "$MSG1" 2>/dev/null || true)
assert "首次回显一致（[$GOT1]）" [ "$GOT1" = "$MSG1" ]

echo "== reconnect: detach 后节点应消失"
detach_all
assert "detach 后串口节点消失" wait_tty_gone 10

echo "== reconnect: 重新 attach"
attach_device 1-1
TTY2=$(wait_dev "/dev/ttyACM" 10) || true
assert "重新 attach 串口再次出现（$TTY2）" [ -n "$TTY2" ]
[ -n "$TTY2" ] || { stop_server; report reconnect; exit 1; }
sleep 1

MSG2="RECONNECT_2_$(date +%s)"
GOT2=$(timeout 12 sudo python3 "$E2E_DIR/cdc_acm_test.py" "$TTY2" "$MSG2" 2>/dev/null || true)
assert "重连后回显一致（[$GOT2]）" [ "$GOT2" = "$MSG2" ]

echo "== reconnect: 停止服务器"
stop_server
assert "服务器停止后端口已释放" bash -c "! ss -tln | grep -q ':$PORT '"
assert "服务器停止后串口节点消失" wait_tty_gone 10
assert "服务器停止后无残留导入" bash -c "! sudo usbip port 2>/dev/null | grep -q 'usbip://'"

echo "== reconnect: 重启服务器并重新 attach"
start_server mock_cdc_acm || { report reconnect; exit 1; }
attach_device 1-1
TTY3=$(wait_dev "/dev/ttyACM" 10) || true
assert "服务器重启后重新 attach 成功（$TTY3）" [ -n "$TTY3" ]
if [ -n "$TTY3" ]; then
    sleep 1
    MSG3="RECONNECT_3_$(date +%s)"
    GOT3=$(timeout 12 sudo python3 "$E2E_DIR/cdc_acm_test.py" "$TTY3" "$MSG3" 2>/dev/null || true)
    assert "服务器重启后回显一致（[$GOT3]）" [ "$GOT3" = "$MSG3" ]
fi

stop_server
report reconnect
