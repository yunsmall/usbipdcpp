#!/usr/bin/env bash
# rndis 端到端：attach 后网卡出现（enx+MAC，rndis_host 驱动），配 IP 后 ping 通设备侧
# （mock_rndis 默认 echo 后端，纯用户态应答 ARP/ICMP，无需 root 服务器）
set -e
source "$(dirname "$0")/common.sh"

setup_workdir rndis
echo "== rndis: 启动服务器 + attach"
start_server mock_rndis || { report rndis; exit 1; }
attach_device 1-1

NETDEV=$(wait_netdev 10) || true
assert "网卡出现（$NETDEV）" [ -n "$NETDEV" ]
[ -n "$NETDEV" ] || { stop_server; report rndis; exit 1; }

# 配 IP 并启用（ping 目标 192.168.53.1 是设备侧地址，echo 后端应答）
sudo ip addr add 192.168.53.2/24 dev "$NETDEV"
sudo ip link set "$NETDEV" up
sleep 1
assert "ping 192.168.53.1 通" ping -c 3 -W 1 192.168.53.1

# 清理：恢复网卡 IP（避免影响下次测试的 enx 状态）
sudo ip addr flush dev "$NETDEV" 2>/dev/null || true
stop_server
report rndis
