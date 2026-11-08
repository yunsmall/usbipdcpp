#!/usr/bin/env bash
# 重复导入：同一设备已被客户端占用时，第二个客户端 import 必须被拒绝
# （服务器返回 NA，见 Session.cpp 的 is_device_using 分支），且不影响
# 第一个客户端的会话；原客户端 detach 后设备应能重新导入。
set -e
source "$(dirname "$0")/common.sh"

imported_count() {
    grep -c -- "-> usbip://$HOST_IP:$PORT/" <<<"$(sudo usbip port 2>/dev/null || true)" || true
}

setup_workdir repeat_import
echo "== repeat_import: 启动服务器 + 第一个客户端导入"
start_server mock_keyboard --no-demo || { report repeat_import; exit 1; }
attach_device 1-1

COUNT=0
for _ in $(seq 1 50); do
    COUNT=$(imported_count)
    [ "$COUNT" -ge 1 ] && break
    sleep 0.1
done
assert "第一个客户端导入成功（$COUNT 个端口）" [ "$COUNT" -ge 1 ]

echo "== repeat_import: 第二个客户端尝试导入同一设备（应被拒绝）"
if sudo usbip -t "$PORT" attach -r "$HOST_IP" -b 1-1 > "$WORK_DIR/second.log" 2>&1; then
    SECOND_REJECTED=0
else
    SECOND_REJECTED=1
fi
assert "重复导入被拒绝（usbip 返回失败）" [ "$SECOND_REJECTED" = "1" ]
assert "服务器日志记录设备正在使用" grep -q "正在使用的设备不支持导出" "$LOG_DIR/mock_keyboard.log"

COUNT=$(imported_count)
assert "第一个客户端的会话未受影响（仍有 $COUNT 个端口）" [ "$COUNT" -ge 1 ]

echo "== repeat_import: 原客户端 detach 后应能重新导入"
detach_all
COUNT=0
for _ in $(seq 1 50); do
    COUNT=$(imported_count)
    [ "$COUNT" -eq 0 ] && break
    sleep 0.1
done
assert "detach 后无残留导入" [ "$COUNT" -eq 0 ]

attach_device 1-1
COUNT=0
for _ in $(seq 1 50); do
    COUNT=$(imported_count)
    [ "$COUNT" -ge 1 ] && break
    sleep 0.1
done
assert "detach 后可再次导入（$COUNT 个端口）" [ "$COUNT" -ge 1 ]

stop_server
report repeat_import
