#!/usr/bin/env bash
# pipe 端到端：attach 成功 + 服务器日志显示周期传输活动
# （mock_pipe 每 2 秒写一条计数消息到 IN 通道；vendor 类设备主机无标准驱动，
# 没有 /dev 节点，验证 attach 状态与服务器侧传输日志）
set -e
source "$(dirname "$0")/common.sh"

setup_workdir pipe
echo "== pipe: 启动服务器 + attach"
start_server mock_pipe || { report pipe; exit 1; }
attach_device 1-1

# 等 attach 成功（usbip port 显示导入）
sleep 1
PORTS=$(sudo usbip port 2>/dev/null || true)
assert "设备已导入" grep -q "1-1 -> usbip://$HOST_IP:$PORT/1-1" <<<"$PORTS"

# 实际数据面：python(pyusb) bulk OUT 发一条 → bulk IN 读回显。
# vendor 类主机无驱动，没有 /dev 节点，libusb 直接访问是唯一的数据面验证
timeout 15 sudo python3 "$E2E_DIR/pipe_test.py" > "$WORK_DIR/pipe_test.out" 2>&1
assert "数据面传输正常（OUT 回显 IN 读回）" [ $? -eq 0 ]

# 服务器侧：周期发送线程日志（每 2 秒一条计数消息）
assert "服务器日志有周期发送" grep -q "周期发送" "$LOG_DIR/mock_pipe.log"

stop_server
report pipe
