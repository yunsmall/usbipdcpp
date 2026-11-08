#!/usr/bin/env bash
# msc SuperSpeed 端到端：mock_msc 以 USB 3.0（SuperSpeed）导出，验证主机按
# SuperSpeed 枚举并且块设备可读。
#
# 只读测试：不向设备写入（写路径由 test_msc.sh 在 High Speed 下覆盖）。
# 块设备操作仍遵守安全规范：先确认新设备身份，再做只读操作。
set -e
source "$(dirname "$0")/common.sh"

setup_workdir msc_usb3
EXPECTED_SIZE=$((4096 * 512)) # 与 test_msc.sh 相同的 2MiB 镜像

# 准备磁盘镜像（写文件不碰任何真实盘）
dd if=/dev/zero of="$WORK_DIR/disk.img" bs=512 count=4096 status=none

echo "== msc_usb3: 启动 SuperSpeed 服务器"
start_server mock_msc -i "$WORK_DIR/disk.img" --super || { report msc_usb3; exit 1; }

echo "== msc_usb3: 记录 attach 前块设备快照与 dmesg 行数，attach"
BEFORE=$(snapshot_block_devices)
DMESG_LINES=$(sudo dmesg | wc -l)
attach_device 1-1

# 分步 1（只查不改）：lsblk 对比 + 内核 dmesg 双路互证新增设备
NEW_DEV=""
DMESG_DEV=""
for _ in $(seq 1 100); do
    NEW_DEV=$(find_new_block_device "$BEFORE")
    DMESG_DEV=$(dmesg_find_new_device "$DMESG_LINES")
    [ -n "$NEW_DEV" ] && [ "$NEW_DEV" = "$DMESG_DEV" ] && break
    sleep 0.1
done
assert "新块设备出现（lsblk=$NEW_DEV，dmesg=$DMESG_DEV，两路一致）" \
    [ -n "$NEW_DEV" ] && [ "$NEW_DEV" = "$DMESG_DEV" ]
[ -n "$NEW_DEV" ] && [ "$NEW_DEV" = "$DMESG_DEV" ] || { stop_server; report msc_usb3; exit 1; }

# 分步 2（只查不改）：从块设备反查 USB 设备，确认枚举速度和 bcdUSB。
# /sys/block/sdX 的符号链接路径里有 USB 设备名（如 2-1），据此读该设备的
# speed/version，避免按端口号猜设备名
USB_DEV=$(readlink -f "/sys/block/$NEW_DEV" | grep -oP '/usb[0-9]+/\K[0-9]+-[0-9.]+' | head -1)
assert "能反查到 USB 设备节点（$USB_DEV）" [ -n "$USB_DEV" ]
if [ -n "$USB_DEV" ]; then
    SPEED=$(cat "/sys/bus/usb/devices/$USB_DEV/speed")
    # sysfs 的 version 带前导空格（" 3.00"），比较前去掉空白
    VERSION=$(tr -d '[:space:]' < "/sys/bus/usb/devices/$USB_DEV/version")
    assert "主机以 SuperSpeed 枚举（speed=${SPEED}Mbps）" [ "$SPEED" = "5000" ]
    assert "设备描述符 bcdUSB = 3.00（实际 $VERSION）" [ "$VERSION" = "3.00" ]
fi
assert "usbip port 显示 Super Speed" \
    bash -c "sudo usbip port | grep -q 'at Super Speed'"

# 分步 3（只查不改）：身份校验——大小必须等于 mock 镜像，不符立即中止
assert "设备 $NEW_DEV 大小 == ${EXPECTED_SIZE}（身份校验）" \
    check_block_device_size "$NEW_DEV" "$EXPECTED_SIZE"
check_block_device_size "$NEW_DEV" "$EXPECTED_SIZE" || {
    echo "ABORT: 设备身份校验失败，不执行任何读取"
    stop_server
    report msc_usb3
    exit 1
}

# 分步 4（只读）：读前 8 个扇区，验证 SuperSpeed bulk 数据通路
assert "SuperSpeed 下只读 8 个扇区成功" \
    sudo dd if="/dev/$NEW_DEV" of=/dev/null bs=512 count=8 status=none

stop_server
report msc_usb3
