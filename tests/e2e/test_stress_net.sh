#!/usr/bin/env bash
# 持续传输压力：ECM 网卡上按指定时长（默认 60 秒，STRESS_SECONDS 可调）
# 循环做 8KB TCP echo，验证长时间传输不回显失败、服务器进程存活、内存
# 无明显增长（用户长时间使用虚拟网卡的稳定性）。
set -e
source "$(dirname "$0")/common.sh"

DURATION="${STRESS_SECONDS:-60}"

setup_workdir stress_net
echo "== stress_net: 启动服务器 + attach（持续 ${DURATION}s）"
start_server mock_ecm || { report stress_net; exit 1; }
attach_device 1-1

NETDEV=$(wait_netdev 10) || true
assert "网卡出现（$NETDEV）" [ -n "$NETDEV" ]
[ -n "$NETDEV" ] || { stop_server; report stress_net; exit 1; }
sudo ip addr add 192.168.53.2/24 dev "$NETDEV"
sudo ip link set "$NETDEV" up
sleep 1
assert "ping 通" ping -c 2 -W 1 192.168.53.1

SERVER_PID=$(sudo ss -tlnp "sport = :$PORT" 2>/dev/null | grep -oP 'pid=\K[0-9]+' | head -1)
RSS_BEFORE=$(awk '/VmRSS/{print $2}' "/proc/$SERVER_PID/status" 2>/dev/null || echo 0)

RESULT=$(timeout $((DURATION + 30)) python3 "$E2E_DIR/net_stress.py" 192.168.53.1 5000 8192 "$DURATION" 2>&1 || true)
SUM=$(tail -2 <<<"$RESULT" | head -1)
assert "持续传输无失败（$SUM）" grep -q "STRESS_OK" <<<"$RESULT"

RSS_AFTER=$(awk '/VmRSS/{print $2}' "/proc/$SERVER_PID/status" 2>/dev/null || echo 0)
GROWTH=$((RSS_AFTER - RSS_BEFORE))
assert "服务器进程仍在运行" kill -0 "$SERVER_PID"
assert "内存增长 < 50MB（${GROWTH}KB）" [ "$GROWTH" -lt 51200 ]

# 压测后设备仍正常工作（不是"能跑但收尾坏了"）
AFTER=$(timeout 20 python3 "$E2E_DIR/net_echo.py" 192.168.53.1 5000 8192 2>&1 || true)
assert "压测后仍能正常回显" grep -q "ECHO_OK" <<<"$AFTER"

sudo ip addr flush dev "$NETDEV" 2>/dev/null || true
stop_server
report stress_net
