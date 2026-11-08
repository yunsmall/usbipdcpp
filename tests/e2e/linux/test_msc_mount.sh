#!/usr/bin/env bash
# msc 真实使用流程：attach → 身份双路确认 → mkfs.ext4 → mount → 写文件读回
# → umount → 再次 mount 读回（模拟用户挂载 U 盘拷贝文件）。现有 test_msc
# 只写裸扇区，覆盖不到文件系统/块层多块读写的实际路径。
#
# 安全规范同 test_msc.sh：确认设备身份与大小之前绝不执行任何写操作。
set -e
source "$(dirname "$0")/common.sh"

setup_workdir msc_mount
EXPECTED_SIZE=$((4096 * 512)) # mock_msc 默认 4096 块 × 512 字节 = 2MiB
dd if=/dev/zero of="$WORK_DIR/disk.img" bs=512 count=4096 status=none

if ! command -v mkfs.ext4 > /dev/null 2>&1; then
    echo "  [SKIP] 无 mkfs.ext4（e2fsprogs），跳过挂载流程"
    report msc_mount
    exit 0
fi

echo "== msc_mount: 启动服务器（镜像 $WORK_DIR/disk.img）"
start_server mock_msc -i "$WORK_DIR/disk.img" || { report msc_mount; exit 1; }

BEFORE=$(snapshot_block_devices)
DMESG_LINES=$(sudo dmesg | wc -l)
attach_device 1-1

# 分步 1（只查不改）：lsblk 对比 + dmesg 内核枚举双路互证
NEW_DEV=""
for _ in $(seq 1 100); do
    NEW_DEV=$(find_new_block_device "$BEFORE")
    DMESG_DEV=$(dmesg_find_new_device "$DMESG_LINES")
    [ -n "$NEW_DEV" ] && [ "$NEW_DEV" = "$DMESG_DEV" ] && break
    sleep 0.1
done
assert "新块设备出现（lsblk=$NEW_DEV，dmesg=$DMESG_DEV，两路一致）" bash -c \
    "[ -n '$NEW_DEV' ] && [ '$NEW_DEV' = '$DMESG_DEV' ]"
[ -n "$NEW_DEV" ] && [ "$NEW_DEV" = "$DMESG_DEV" ] || { stop_server; report msc_mount; exit 1; }

# 分步 2（只查不改）：身份校验，大小不符立即中止
assert "设备 $NEW_DEV 大小 == ${EXPECTED_SIZE}（身份校验）" \
    check_block_device_size "$NEW_DEV" "$EXPECTED_SIZE"
check_block_device_size "$NEW_DEV" "$EXPECTED_SIZE" || {
    echo "ABORT: 设备身份校验失败，不执行任何写操作"
    stop_server
    report msc_mount
    exit 1
}

# 分步 2a（只读）：厂商/型号硬校验。大小相同还不够，必须确认是 mock_msc
assert "设备 $NEW_DEV 厂商/型号 == mock_msc（身份校验）" check_msc_identity "$NEW_DEV"
check_msc_identity "$NEW_DEV" || {
    echo "ABORT: 设备身份校验失败（vendor/model 不是 mock_msc），不执行任何写操作"
    stop_server
    report msc_mount
    exit 1
}

MNT="$WORK_DIR/mnt"
mkdir -p "$MNT"
cleanup_mount() {
    sudo umount "$MNT" 2>/dev/null || true
}
trap cleanup_mount EXIT

echo "== msc_mount: mkfs.ext4 /dev/$NEW_DEV"
if sudo mkfs.ext4 -q -F "/dev/$NEW_DEV" > "$WORK_DIR/mkfs.log" 2>&1; then
    assert "mkfs.ext4 成功" true
else
    assert "mkfs.ext4 成功" false
    echo "  mkfs 失败，见 $WORK_DIR/mkfs.log"
    stop_server
    report msc_mount
    exit 1
fi

echo "== msc_mount: mount + 写文件读回"
sudo mount "/dev/$NEW_DEV" "$MNT"
PAYLOAD="msc mount e2e $(date +%s)"
printf "%s\n" "$PAYLOAD" | sudo tee "$MNT/hello.txt" > /dev/null
sudo sync
GOT=$(sudo cat "$MNT/hello.txt" 2>/dev/null || true)
assert "挂载后写入读回一致（[$GOT]）" [ "$GOT" = "$PAYLOAD" ]

# 写一个跨块的大文件（1MiB），验证多块写入而非单扇区
sudo dd if=/dev/urandom of="$MNT/big.bin" bs=64K count=16 status=none
sudo sync
BIG_SUM=$(sudo sha256sum "$MNT/big.bin" | cut -d' ' -f1)
assert "大文件写入成功（sha256 非空）" [ -n "$BIG_SUM" ]

sudo umount "$MNT"
assert "umount 后挂载点已空" [ ! -e "$MNT/hello.txt" ]

echo "== msc_mount: 重新挂载读回（模拟拔插后重新挂载）"
sudo mount "/dev/$NEW_DEV" "$MNT"
GOT2=$(sudo cat "$MNT/hello.txt" 2>/dev/null || true)
BIG_SUM2=$(sudo sha256sum "$MNT/big.bin" 2>/dev/null | cut -d' ' -f1)
assert "重新挂载后文本文件仍一致" [ "$GOT2" = "$PAYLOAD" ]
assert "重新挂载后大文件校验和一致" [ -n "$BIG_SUM" ] && [ "$BIG_SUM" = "$BIG_SUM2" ]
sudo umount "$MNT"

stop_server
report msc_mount
