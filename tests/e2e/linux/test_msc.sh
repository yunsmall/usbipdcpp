#!/usr/bin/env bash
# msc 端到端：attach 后新块设备出现，分步验证读写。
#
# 安全规范（必须遵守，写错盘符会毁掉 WSL 里的物理硬盘）：
#  - 块设备读写必须分步：先单独确认新设备是哪个，确认无误后再单独执行写
#  - 写前必须过大小身份校验：设备大小与 mock 镜像不符立即中止，绝不写入
#  - 禁止一条命令混查设备与写设备
set -e
source "$(dirname "$0")/common.sh"

setup_workdir msc
EXPECTED_SIZE=$((4096 * 512)) # mock_msc 默认 disk.img：4096 块 × 512 字节 = 2MiB

# 准备磁盘镜像（虚拟盘文件；写它不碰任何真实盘）
dd if=/dev/zero of="$WORK_DIR/disk.img" bs=512 count=4096 status=none

echo "== msc: 启动服务器（镜像 $WORK_DIR/disk.img）"
start_server mock_msc -i "$WORK_DIR/disk.img" || { report msc; exit 1; }

echo "== msc: 记录 attach 前块设备快照与 dmesg 行数，attach"
BEFORE=$(snapshot_block_devices)
DMESG_LINES=$(sudo dmesg | wc -l)
attach_device 1-1

# 分步 1（只查不改）：lsblk 对比 + dmesg 内核枚举双路互证新增设备
NEW_DEV=""
for _ in $(seq 1 100); do
    NEW_DEV=$(find_new_block_device "$BEFORE")
    DMESG_DEV=$(dmesg_find_new_device "$DMESG_LINES")
    [ -n "$NEW_DEV" ] && [ "$NEW_DEV" = "$DMESG_DEV" ] && break
    sleep 0.1
done
assert "新块设备出现（lsblk=$NEW_DEV，dmesg=$DMESG_DEV，两路一致）" \
    [ -n "$NEW_DEV" ] && [ "$NEW_DEV" = "$DMESG_DEV" ]
[ -n "$NEW_DEV" ] && [ "$NEW_DEV" = "$DMESG_DEV" ] || { stop_server; report msc; exit 1; }

# 分步 2（只查不改）：身份校验——大小必须等于 mock 镜像，不符说明设备可疑
assert "设备 $NEW_DEV 大小 == ${EXPECTED_SIZE}（身份校验）" \
    check_block_device_size "$NEW_DEV" "$EXPECTED_SIZE"
check_block_device_size "$NEW_DEV" "$EXPECTED_SIZE" || {
    echo "ABORT: 设备身份校验失败，不执行任何写操作"
    stop_server
    report msc
    exit 1
}

# 分步 2a（只读）：厂商/型号硬校验。大小相同还不够，必须确认是 mock_msc
assert "设备 $NEW_DEV 厂商/型号 == mock_msc（身份校验）" check_msc_identity "$NEW_DEV"
check_msc_identity "$NEW_DEV" || {
    echo "ABORT: 设备身份校验失败（vendor/model 不是 mock_msc），不执行任何写操作"
    stop_server
    report msc
    exit 1
}

# 分步 2b（只读）：校验 SCSI VPD 页内容。USB 存储默认 try_rc_10_first，
# 小容量设备内核不会发 READ CAPACITY(16)，也就看不到 LBPME/discard，
# 所以这里直接用 sg_inq 验证设备侧返回是否按 SBC-4 组织
if command -v sg_inq >/dev/null 2>&1; then
    assert "VPD 0xB0 Block Limits 宣告 UNMAP" \
        bash -c "sudo sg_inq -p 0xb0 /dev/$NEW_DEV | grep -q 'Maximum unmap LBA count: 65536'"
    assert "VPD 0xB0 Block Limits 宣告 WRITE SAME" \
        bash -c "sudo sg_inq -p 0xb0 /dev/$NEW_DEV | grep -q 'Maximum write same length: 0xffff'"
    assert "VPD 0xB1 报告非旋转介质" \
        bash -c "sudo sg_inq -p 0xb1 /dev/$NEW_DEV | grep -q 'Non-rotating medium'"
    assert "VPD 0x80 序列号可读" \
        bash -c "sudo sg_inq -p 0x80 /dev/$NEW_DEV | grep -q 'USBIPDCPSN'"
    assert "READ CAPACITY(16) 报告 LBPME/LBPRZ" \
        bash -c "sudo sg_readcap -l /dev/$NEW_DEV | grep -q 'lbpme=1, lbprz=1'"
else
    echo "  [SKIP] 未安装 sg3-utils，跳过 VPD 校验"
fi

# 分步 3（写）：设备已确认且大小匹配，单独执行写（扇区 100 起，不碰分区表）。
# 数据用文件准备（填满 512 字节），读写都走文件比对（cmp）——echo 管道喂 dd
# 数据不足 512 字节写不满块，读回含 NUL/空白，字符串比较有陷阱
TEST_FILE="$WORK_DIR/test_data.bin"
printf "e2e msc test %s\n" "$(date +%s)" > "$TEST_FILE"
truncate -s 512 "$TEST_FILE"
echo "== msc: 向 /dev/$NEW_DEV 写测试数据（扇区 100）"
sudo dd if="$TEST_FILE" of="/dev/$NEW_DEV" bs=512 seek=100 count=1 conv=fsync status=none
sudo sync

# 分步 4（读回验证，只读）
GOT_FILE="$WORK_DIR/read_back.bin"
sudo dd if="/dev/$NEW_DEV" bs=512 skip=100 count=1 of="$GOT_FILE" status=none
assert "写读回一致" cmp -s "$TEST_FILE" "$GOT_FILE"

stop_server
report msc
