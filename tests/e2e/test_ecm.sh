#!/usr/bin/env bash
# ecm 端到端：attach 后网卡出现（enx+MAC），配 IP 后 ping 通设备侧
# （mock_ecm 默认 echo 后端，纯用户态应答 ARP/ICMP，无需 root 服务器）
set -e
source "$(dirname "$0")/common.sh"

setup_workdir ecm
echo "== ecm: 启动服务器 + attach"
start_server mock_ecm || { report ecm; exit 1; }
attach_device 1-1

NETDEV=$(wait_netdev 10) || true
assert "网卡出现（$NETDEV）" [ -n "$NETDEV" ]
[ -n "$NETDEV" ] || { stop_server; report ecm; exit 1; }

# 配 IP 并启用（ping 目标 192.168.53.1 是设备侧地址，echo 后端应答）
sudo ip addr add 192.168.53.2/24 dev "$NETDEV"
sudo ip link set "$NETDEV" up
sleep 1
assert "ping 192.168.53.1 通" ping -c 3 -W 1 192.168.53.1

# TCP echo：连设备侧 5000 端口发 5 字节，验证原样回显（echo 后端按段回显）
TCP_REPLY=$(timeout 5 bash -c 'exec 3<>/dev/tcp/192.168.53.1/5000; printf "hello" >&3; IFS= read -r -N 5 -t 3 reply <&3; printf "%s" "$reply"' 2>/dev/null || true)
assert "TCP echo 5000 回显（$TCP_REPLY）" [ "$TCP_REPLY" = "hello" ]

# 清理：恢复网卡 IP（避免影响下次测试的 enx 状态）
sudo ip addr flush dev "$NETDEV" 2>/dev/null || true
stop_server
report ecm
