#!/usr/bin/env bash
# MSC 写入中断开：主机正在写块设备时直接 detach，验证服务器不崩溃、端口
# 继续监听，断开后重新 attach 设备仍可读写。覆盖用户拔 U 盘/断网的场景。
#
# 安全规范同 test_msc.sh：确认设备身份与大小之前绝不写入。
set -e
source "$(dirname "$0")/common.sh"

setup_workdir msc_interrupt
EXPECTED_SIZE=$((4096 * 512))
dd if=/dev/zero of="$WORK_DIR/disk.img" bs=512 count=4096 status=none

echo "== msc_interrupt: 启动服务器 + attach"
start_server mock_msc -i "$WORK_DIR/disk.img" || { report msc_interrupt; exit 1; }

BEFORE=$(snapshot_block_devices)
DMESG_LINES=$(sudo dmesg | wc -l)
attach_device 1-1

find_confirmed_device() {
    local before="$1" dmesg_lines="$2" dev=""
    for _ in $(seq 1 100); do
        dev=$(find_new_block_device "$before")
        local kernel_dev
        kernel_dev=$(dmesg_find_new_device "$dmesg_lines")
        [ -n "$dev" ] && [ "$dev" = "$kernel_dev" ] && { echo "$dev"; return 0; }
        sleep 0.1
    done
    return 1
}

NEW_DEV=$(find_confirmed_device "$BEFORE" "$DMESG_LINES") || true
assert "新块设备双路确认（$NEW_DEV）" [ -n "$NEW_DEV" ]
[ -n "$NEW_DEV" ] || { stop_server; report msc_interrupt; exit 1; }
assert "设备大小 == ${EXPECTED_SIZE}（身份校验）" check_block_device_size "$NEW_DEV" "$EXPECTED_SIZE"
check_block_device_size "$NEW_DEV" "$EXPECTED_SIZE" || {
    echo "ABORT: 设备身份校验失败，不执行任何写操作"
    stop_server
    report msc_interrupt
    exit 1
}

echo "== msc_interrupt: 后台持续写 /dev/$NEW_DEV，中途 detach"
sudo dd if=/dev/zero of="/dev/$NEW_DEV" bs=512 count=4096 conv=fsync status=none &
DD_PID=$!
sleep 0.5
detach_all
# 写入会被内核以 I/O 错误终止；若 dd 没退出就强杀（写的是 mock 镜像）
sleep 2
sudo kill -9 "$DD_PID" 2>/dev/null || true
wait "$DD_PID" 2>/dev/null || true

assert "断开后服务器仍在监听" bash -c "ss -tln | grep -q ':$PORT '"
SERVER_PID=$(sudo ss -tlnp "sport = :$PORT" 2>/dev/null | grep -oP 'pid=\K[0-9]+' | head -1)
assert "服务器进程仍在运行（pid=$SERVER_PID）" kill -0 "$SERVER_PID"
assert "服务器日志无异常终止" bash -c "! grep -qE 'terminate called|Segmentation fault|Aborted' '$LOG_DIR/mock_msc.log'"

echo "== msc_interrupt: 重新 attach，设备应仍可读写"
BEFORE2=$(snapshot_block_devices)
DMESG_LINES2=$(sudo dmesg | wc -l)
attach_device 1-1
NEW_DEV2=$(find_confirmed_device "$BEFORE2" "$DMESG_LINES2") || true
assert "重新 attach 后新块设备出现（$NEW_DEV2）" [ -n "$NEW_DEV2" ]
if [ -n "$NEW_DEV2" ]; then
    assert "重新 attach 后大小仍正确" check_block_device_size "$NEW_DEV2" "$EXPECTED_SIZE"
    if check_block_device_size "$NEW_DEV2" "$EXPECTED_SIZE"; then
        # 只读验证：中断写入后设备仍能应答 READ(10)（扇区 0）
        sudo dd if="/dev/$NEW_DEV2" of="$WORK_DIR/sector0.bin" bs=512 count=1 status=none
        assert "断开重连后读取扇区 0 成功" [ -s "$WORK_DIR/sector0.bin" ]
    fi
fi

stop_server
report msc_interrupt
