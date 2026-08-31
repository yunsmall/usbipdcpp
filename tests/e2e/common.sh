#!/usr/bin/env bash
# e2e 公共函数：mock 服务器启停 / usbip attach-detach / 设备等待 / 断言
# 用法：source common.sh 后调用各函数；脚本需 sudo usbip（vhci 写 /sys 要 root）
set -u

# 构建目录（可被 BUILD_DIR 环境变量覆盖；默认 WSL2 构建）
E2E_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$E2E_DIR/../.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$PROJECT_DIR/build_wsl2}"

PORT="${PORT:-53240}"
HOST_IP="${HOST_IP:-127.0.0.1}"
WORK_DIR="${WORK_DIR:-/tmp/usbip_e2e}"
LOG_DIR="${LOG_DIR:-$WORK_DIR/logs}"

PASS_COUNT=0
FAIL_COUNT=0

# 工作目录与日志目录（每个测试独立工作目录防串扰）
setup_workdir() { # setup_workdir <测试名>
    WORK_DIR="/tmp/usbip_e2e/$1"
    LOG_DIR="$WORK_DIR/logs"
    rm -rf "$WORK_DIR"
    mkdir -p "$LOG_DIR"
}

# 断言工具：条件为真记 pass，否则记 fail（脚本最后按 fail 数退出非 0）
assert() { # assert <描述> <命令...>
    local desc="$1"
    shift
    if "$@"; then
        PASS_COUNT=$((PASS_COUNT + 1))
        echo "  [PASS] $desc"
    else
        FAIL_COUNT=$((FAIL_COUNT + 1))
        echo "  [FAIL] $desc"
    fi
}

# 服务器进程管理 ==============================================================
# 注意：pkill -x 按可执行名匹配，进程名超 15 字符会被内核截断（如
# mock_cdc_throttle→mock_cdc_thrott），按端口找 PID 杀最稳（不依赖进程名）

# 杀占用 PORT 的进程（等待优雅退出）。
# 必须 sudo：非 root 时 ss -p 的 pid 列是 "-"，拿不到 pid 杀不掉服务器，
# 残留服务器会占端口导致新服务器起不来、attach 连到旧会话
kill_by_port() {
    local pid
    pid=$(sudo ss -tlnp "sport = :$PORT" 2>/dev/null | grep -oP 'pid=\K[0-9]+' | head -1)
    if [ -n "$pid" ]; then
        sudo kill "$pid" 2>/dev/null
    fi
}

# 等待端口就绪（服务器开始监听）
wait_port_ready() { # wait_port_ready <超时秒>
    local timeout="${1:-10}"
    for _ in $(seq 1 $((timeout * 10))); do
        if ss -tln | grep -q ":$PORT "; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

# 等待端口释放（服务器优雅退出后端口才可复用，误 attach 旧设备）
wait_port_free() { # wait_port_free <超时秒>
    local timeout="${1:-10}"
    for _ in $(seq 1 $((timeout * 10))); do
        if ! ss -tln | grep -q ":$PORT "; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

# 启动 mock 服务器：先清旧进程 → setsid 后台（< /dev/null 防 stdin 挂住）→ 等端口
start_server() { # start_server <二进制名> [参数...]
    local bin="$1"
    shift
    detach_all # 清残留 attach（残留会话会让新 attach 失败/连到旧设备）
    kill_by_port
    wait_port_free || {
        echo "FAIL: 端口 $PORT 未释放（旧服务器没杀掉？）"
        return 1
    }
    if [ ! -x "$BUILD_DIR/$bin" ]; then
        echo "FAIL: 找不到 $BUILD_DIR/$bin（先编译？）"
        return 1
    fi
    cd "$WORK_DIR" 2>/dev/null || true
    setsid "$BUILD_DIR/$bin" "$@" < /dev/null > "$LOG_DIR/$bin.log" 2>&1 &
    if ! wait_port_ready; then
        echo "FAIL: $bin 未在端口 $PORT 监听（见 $LOG_DIR/$bin.log）"
        return 1
    fi
    return 0
}

# 停止服务器：先 detach 全部设备（否则会话占用端口不释放），杀进程 + 等端口释放
stop_server() {
    detach_all
    kill_by_port
    wait_port_free || echo "WARN: 端口 $PORT 未释放"
}

# usbip 客户端 ================================================================

# attach 设备（sudo：vhci 写 /sys 要 root）
attach_device() { # attach_device <busid>
    sudo usbip -t "$PORT" attach -r "$HOST_IP" -b "$1"
}

# detach 所有已导入设备（port 列表逐行解析）
detach_all() {
    sudo usbip port 2>/dev/null | grep -oP '^Port \K[0-9]+' | while read -r p; do
        sudo usbip detach -p "$p" 2>/dev/null
    done
    sleep 0.5
}

# 设备等待 ====================================================================

# 等待网络接口出现（ECM 网卡名 enx+MAC）
wait_netdev() { # wait_netdev <超时秒>
    local timeout="${1:-10}"
    for _ in $(seq 1 $((timeout * 10))); do
        local dev
        dev=$(ip -br link | grep -oP 'enx[0-9a-f]+' | head -1)
        if [ -n "$dev" ]; then
            echo "$dev"
            return 0
        fi
        sleep 0.1
    done
    return 1
}

# 等待字符设备出现（串口 /dev/ttyACM* 等）。
# 只认字符设备（-c）：设备 detach 后节点若残留普通文件（tee 误建），
# 按名字匹配会拿到假节点（stty/读写都会诡异失败）
wait_dev() { # wait_dev <路径前缀> <超时秒>
    local prefix="$1"
    local timeout="${2:-10}"
    for _ in $(seq 1 $((timeout * 10))); do
        local dev
        dev=$(ls "$prefix"* 2>/dev/null | while read -r d; do
            [ -c "$d" ] && echo "$d"
        done | head -1)
        if [ -n "$dev" ]; then
            echo "$dev"
            return 0
        fi
        sleep 0.1
    done
    return 1
}

# 等待 /proc/asound/cards 出现 usbip 声卡（UAC）。
# grep -i：声卡名是 "Usbipdcpp ..."（大写 U），大小写敏感匹配会漏
wait_usb_soundcard() { # wait_usb_soundcard <超时秒>
    local timeout="${1:-10}"
    for _ in $(seq 1 $((timeout * 10))); do
        if grep -qi "usbip" /proc/asound/cards 2>/dev/null; then
            cat /proc/asound/cards
            return 0
        fi
        sleep 0.1
    done
    return 1
}

# 块设备（MSC）===============================================================
# 安全规范（见项目 CLAUDE.md）：块设备读写必须分步，attach 后先单独确认新设备
# 是哪个，确认无误后再单独执行读写。禁止一条命令混查设备与写设备——WSL 里
# 还有其他物理硬盘，写错盘符会毁数据。

# 记录当前块设备集合（attach 前后对比用）
snapshot_block_devices() {
    lsblk -n -o NAME | sort
}

# 对比出新增的块设备（单独一步：只查不改）
# 返回新增的 sd 设备名（如 sdb）；无新增返回空
find_new_block_device() { # find_new_block_device <attach前快照>
    local before="$1"
    local after
    after=$(lsblk -n -o NAME | sort)
    comm -13 <(printf '%s\n' "$before") <(printf '%s\n' "$after") | grep -E '^sd' | head -1
}

# 从 attach 后的 dmesg 增量里提取内核枚举的 sd 设备名（与 lsblk 对比互证，
# 双保险：内核说枚举了哪个盘、lsblk 对比出哪个盘，两者必须一致才认）
dmesg_find_new_device() { # dmesg_find_new_device <attach前dmesg行数>
    local before_lines="$1"
    sudo dmesg | tail -n +$((before_lines + 1)) | grep -oP '\[sd[a-z]+\]' | tail -1 | tr -d '[]'
}

# 校验块设备大小与预期一致（写操作前必须过这一关：大小不符说明设备身份可疑，
# 立即中止，绝不写入）。mock_msc 默认 2MiB（4096 块 × 512 字节）
check_block_device_size() { # check_block_device_size <设备名> <期望字节数>
    local dev="$1"
    local expected="$2"
    [ -b "/dev/$dev" ] || return 1
    local size
    size=$(sudo blockdev --getsize64 "/dev/$dev")
    [ "$size" = "$expected" ]
}

# 结果汇总 ====================================================================
report() { # report <测试名>
    echo "=============================================="
    echo "$1: $PASS_COUNT passed, $FAIL_COUNT failed"
    [ "$FAIL_COUNT" -eq 0 ]
}
