#!/bin/bash
# 手动运行一个 libusb e2e 用例（不注册 ctest）
#
# 用法: sudo ./run_one.sh <control|bulk|interrupt|iso>
#
# control/bulk/interrupt 链路: FunctionFS 设备 -> dummy_hcd -> libusb_server -> USB/IP -> vhci -> libusb 客户端
# iso 链路: 项目虚拟等时设备 -> vhci -> libusb_server -> vhci -> libusb 客户端（双跳，不经过 gadget）
#   两个 hop 必须落在不同的 vhci 控制器上：同一 usb_hcd 上的两个导入设备共用
#   bandwidth_mutex，SET_INTERFACE 会互等死锁。hop2 用 attach_port.py 指定
#   控制器 1 的端口（16），usbip 命令行做不到。
set -euo pipefail

PROFILE="${1:-}"
MODE=gadget
RAW_MODE=0
SERVER_PORT=""
HOP2_VHCI_PORT=""
HOP1_BUSID=""
VDEV_BUSID="1-1"

usage() {
    cat <<EOF
用法: $0 <control|bulk|interrupt|iso|raw>

手动运行一个 libusb 后端 e2e 用例（需要 root）。

用例:
  control    控制传输（FunctionFS 设备 + dummy_hcd）
  bulk       bulk 传输
  interrupt  interrupt 传输
  iso        等时传输（虚拟设备双跳，需要 CONFIG_USBIP_VHCI_NR_HCS>=2）
  raw        直接走 USB/IP 协议的负路径（非法端点/wLength/重复 seqnum/iso 描述符）

环境变量:
  BUILD_DIR  构建目录（默认自动查找 build，再尝试 build_wsl2）
  USBIP      usbip 可执行文件（默认从 PATH 查找）

完整说明见同目录 README.md。
EOF
}

case "$PROFILE" in
    -h|--help)
        usage
        exit 0
        ;;
    "")
        usage >&2
        exit 2
        ;;
esac

case "$PROFILE" in
    control)
        DEVICE_BIN=e2e_ffs_control
        CLIENT_BIN=e2e_libusb_control_client
        PID_HEX=0x0104
        PORT=53261
        ;;
    bulk)
        DEVICE_BIN=e2e_ffs_bulk
        CLIENT_BIN=e2e_libusb_bulk_client
        PID_HEX=0x0105
        PORT=53262
        ;;
    interrupt)
        DEVICE_BIN=e2e_ffs_interrupt
        CLIENT_BIN=e2e_libusb_interrupt_client
        PID_HEX=0x0106
        PORT=53263
        ;;
    iso)
        MODE=vdev
        DEVICE_BIN=e2e_vdev_iso
        CLIENT_BIN=e2e_libusb_iso_client
        PID_HEX=0x0107
        PORT=53264
        SERVER_PORT=53265
        # 全局端口号：16 是控制器 1 的 hs 端口 0（0-7 控制器 0 hs，8-15 ss）
        HOP2_VHCI_PORT=16
        ;;
    raw)
        MODE=vdev
        RAW_MODE=1
        DEVICE_BIN=e2e_vdev_iso
        CLIENT_BIN=""
        PID_HEX=0x0107
        PORT=53266
        SERVER_PORT=53267
        ;;
    *)
        echo "用法: $0 <control|bulk|interrupt|iso|raw>（$0 --help 查看说明）" >&2
        exit 2
        ;;
esac

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$SCRIPT_DIR/../../.." && pwd)
if [ -z "${BUILD_DIR:-}" ]; then
    # 优先用仓库默认构建目录；本地已有其他名字的构建目录时自动回退
    for candidate in build build_wsl2; do
        if [ -x "$REPO_ROOT/$candidate/$DEVICE_BIN" ] && [ -x "$REPO_ROOT/$candidate/libusb_server" ]; then
            BUILD_DIR="$REPO_ROOT/$candidate"
            break
        fi
    done
    BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build}"
fi
USBIP="${USBIP:-usbip}"
# 允许只写命令名（默认从 PATH 找），也允许写绝对路径
USBIP=$(command -v "$USBIP" 2>/dev/null) || USBIP=""

GADGET_NAME="e2e_${PROFILE}"
FFS_NAME="$GADGET_NAME"
GADGET_DIR="/sys/kernel/config/usb_gadget/$GADGET_NAME"
FFS_MOUNT="/dev/ffs-$GADGET_NAME"
RUN_DIR="$REPO_ROOT/tmp/e2e_libusb_$PROFILE"
DEVICE_LOG="$RUN_DIR/device.log"
SERVER_LOG="$RUN_DIR/server.log"
# 仓库在 /mnt 下时不能建 FIFO（DrvFs 不支持 mkfifo），命令管道放 /tmp
COMMAND_FIFO="/tmp/usbipdcpp-e2e-$PROFILE.fifo"

SERVER_PID=""
DEVICE_PID=""
VDEV_PID=""
DETACH_PORT=""
DETACH_PORT_A=""
DETACH_PORT_B=""
COMMAND_FD_OPEN=0
ports_before=""
ports_after=""

log() {
    echo "[$PROFILE] $*"
}

die() {
    echo "[$PROFILE] 错误: $*" >&2
    exit 1
}

# 拆掉 configfs gadget；cleanup 和启动前的残留清理共用
remove_gadget() {
    [ -d "$GADGET_DIR" ] || return 0
    printf '' > "$GADGET_DIR/UDC" 2>/dev/null
    rm -f "$GADGET_DIR/configs/c.1/ffs.$FFS_NAME" 2>/dev/null
    rmdir "$GADGET_DIR/configs/c.1" \
          "$GADGET_DIR/functions/ffs.$FFS_NAME" \
          "$GADGET_DIR/strings/0x409" 2>/dev/null
    rmdir "$GADGET_DIR" 2>/dev/null
}

cleanup() {
    set +e
    if [ -n "$DETACH_PORT" ]; then
        "$USBIP" detach -p "$DETACH_PORT" >/dev/null 2>&1
    fi
    if [ -n "$DETACH_PORT_A" ]; then
        "$USBIP" detach -p "$DETACH_PORT_A" >/dev/null 2>&1
    fi
    if [ -n "$DETACH_PORT_B" ]; then
        "$USBIP" detach -p "$DETACH_PORT_B" >/dev/null 2>&1
    fi
    if [ "$COMMAND_FD_OPEN" = "1" ]; then
        printf 'q\n' >&9 2>/dev/null
    fi
    if [ -n "$SERVER_PID" ]; then
        # 服务端可能因为异常不响应 q，等一小会儿后直接 kill，避免 trap 挂死
        for _ in $(seq 1 50); do
            kill -0 "$SERVER_PID" 2>/dev/null || break
            sleep 0.1
        done
        kill "$SERVER_PID" 2>/dev/null
        wait "$SERVER_PID" 2>/dev/null
    fi
    exec 9>&- 2>/dev/null
    [ -n "${ports_before:-}" ] && rm -f "$ports_before"
    [ -n "${ports_after:-}" ] && rm -f "$ports_after"
    if [ -n "$DEVICE_PID" ]; then
        kill "$DEVICE_PID" 2>/dev/null
        wait "$DEVICE_PID" 2>/dev/null
    fi
    if [ -n "$VDEV_PID" ]; then
        # 虚拟设备进程处理 SIGTERM 走 server.stop() 正常清理
        kill "$VDEV_PID" 2>/dev/null
        wait "$VDEV_PID" 2>/dev/null
    fi
    remove_gadget
    if mountpoint -q "$FFS_MOUNT" 2>/dev/null; then
        umount "$FFS_MOUNT" 2>/dev/null || true
    fi
    rmdir "$FFS_MOUNT" 2>/dev/null || true
    rm -f "$COMMAND_FIFO"
}

# bash 在收到 SIGINT/SIGTERM 后会先执行 EXIT trap 再退出（已实测），只挂 EXIT
# 就够了；另外挂 INT/TERM 的话，cleanup 执行完脚本还会继续往下跑
trap cleanup EXIT

[ "$(id -u)" -eq 0 ] || die "需要 root 运行"
[ -x "$BUILD_DIR/$DEVICE_BIN" ] || die "找不到 $BUILD_DIR/$DEVICE_BIN，先编译构建目录"
[ -z "$CLIENT_BIN" ] || [ -x "$BUILD_DIR/$CLIENT_BIN" ] || \
    die "找不到 $BUILD_DIR/$CLIENT_BIN，先编译构建目录"
[ -x "$BUILD_DIR/libusb_server" ] || die "找不到 $BUILD_DIR/libusb_server"
[ -x "$USBIP" ] || die "找不到 usbip 工具: $USBIP"

# 上一次异常退出（SIGKILL/崩溃）不会执行 EXIT trap，可能残留 vdev 或
# libusb_server 继续占着端口。提前检测并给出明确提示，避免表现成"服务端
# 没有起来"的误导性错误（旧进程还握着已被删除的 FIFO inode，不会自己退出）
port_in_use() {
    local port="$1" output=""
    if command -v ss >/dev/null 2>&1; then
        if output=$(ss -ltn 2>/dev/null); then
            printf '%s\n' "$output" | awk -v suffix=":$port" '$4 ~ suffix "$"' | grep -q .
            return
        fi
        # ss 执行失败（BusyBox 不支持该选项等）才继续尝试 netstat
    fi
    if command -v netstat >/dev/null 2>&1; then
        if output=$(netstat -ltn 2>/dev/null); then
            printf '%s\n' "$output" | awk -v suffix=":$port" '$4 ~ suffix "$"' | grep -q .
            return
        fi
    fi
    # 静默跳过会让端口冲突表现成"服务端没起来"，这里显式报错
    die "无法通过 ss/netstat 检查端口 $port 是否被占用（命令执行失败或缺少工具）"
}

check_port_free() {
    local port="$1"
    [ -n "$port" ] || return 0
    if port_in_use "$port"; then
        die "端口 $port 已被占用，可能有上次残留的进程（ps -eo pid,comm | grep -E 'libusb_server|e2e_'），先清理再跑"
    fi
}

check_port_free "$PORT"
check_port_free "$SERVER_PORT"

mkdir -p "$RUN_DIR"
rm -f "$DEVICE_LOG" "$SERVER_LOG"

PID_DECIMAL="${PID_HEX#0x}"

# 所有 WSL 发行版共享同一个内核和 /dev、/sys，用 /dev 节点差集定位导入设备
# 会被别的进程/残留设备干扰。这里对比 attach 前后“VID/PID 匹配且不在
# dummy_hcd 总线上”的 sysfs 设备集合，取新增的那个
matching_busids() {
    for device_dir in /sys/bus/usb/devices/*; do
        [ -r "$device_dir/idVendor" ] || continue
        [ "$(cat "$device_dir/idVendor")" = "1d6b" ] || continue
        [ "$(cat "$device_dir/idProduct" 2>/dev/null)" = "$PID_DECIMAL" ] || continue
        case "$(readlink -f "$device_dir")" in
            */dummy_hcd*) continue ;;
        esac
        basename "$device_dir"
    done
    return 0
}

ports_snapshot() {
    "$USBIP" port 2>/dev/null | sed -n 's/^Port \([0-9][0-9]*\):.*/\1/p' | sort -n
}

# 等待 attach 后新增的 sysfs 设备 busid
wait_new_busid() {
    local before="$1" found=""
    for _ in $(seq 1 100); do
        found=$(comm -13 <(printf '%s\n' "$before") <(matching_busids | sort) | head -1)
        [ -n "$found" ] && break
        sleep 0.1
    done
    printf '%s' "$found"
}

# 等待 attach 后新增的 vhci 端口
wait_new_port() {
    local before="$1" found=""
    for _ in $(seq 1 100); do
        found=$(comm -13 <(printf '%s\n' "$before") <(ports_snapshot) | head -1)
        [ -n "$found" ] && break
        sleep 0.1
    done
    printf '%s' "$found"
}

# 启动虚拟设备 → vhci 导入 → 启动 libusb_server 并绑定。成功后 HOP1_BUSID 可用
start_vdev_and_bind() {
    log "启动等时虚拟设备（端口 $PORT，busid $VDEV_BUSID）"
    "$BUILD_DIR/$DEVICE_BIN" -p "$PORT" -b "$VDEV_BUSID" > "$DEVICE_LOG" 2>&1 &
    VDEV_PID=$!
    for _ in $(seq 1 100); do
        "$USBIP" -t "$PORT" list -r 127.0.0.1 >/dev/null 2>&1 && break
        sleep 0.05
    done
    "$USBIP" -t "$PORT" list -r 127.0.0.1 >/dev/null 2>&1 || \
        die "虚拟设备服务端没有起来，见 $DEVICE_LOG"

    log "第一次 vhci 导入（虚拟设备 → 本机 USB 设备）"
    local hop1_before ports_before_hop1
    hop1_before=$(matching_busids | sort)
    ports_before_hop1=$(ports_snapshot)
    "$USBIP" -t "$PORT" attach -r 127.0.0.1 -b "$VDEV_BUSID" || die "第一次 usbip attach 失败"
    HOP1_BUSID=$(wait_new_busid "$hop1_before")
    [ -n "$HOP1_BUSID" ] || die "5 秒内没有出现第一次导入的设备节点"
    DETACH_PORT_A=$(wait_new_port "$ports_before_hop1")
    [ -n "$DETACH_PORT_A" ] || die "找不到第一次 attach 占用的 vhci 端口"
    log "第一次导入后的设备 busid=$HOP1_BUSID（vhci 端口 $DETACH_PORT_A）"

    log "启动 libusb_server（交互模式，导出第一次导入的设备）"
    rm -f "$COMMAND_FIFO"
    mkfifo "$COMMAND_FIFO"
    exec 9<>"$COMMAND_FIFO"
    COMMAND_FD_OPEN=1
    "$BUILD_DIR/libusb_server" -p "$SERVER_PORT" < "$COMMAND_FIFO" > "$SERVER_LOG" 2>&1 &
    SERVER_PID=$!
    for _ in $(seq 1 100); do
        "$USBIP" -t "$SERVER_PORT" list -r 127.0.0.1 >/dev/null 2>&1 && break
        sleep 0.05
    done
    "$USBIP" -t "$SERVER_PORT" list -r 127.0.0.1 >/dev/null 2>&1 || \
        die "libusb_server 没有起来，见 $SERVER_LOG"

    printf 'b %s\n' "$HOP1_BUSID" >&9
    local bound=0
    for _ in $(seq 1 100); do
        # 精确匹配 busid 字段，避免 1-1 误匹配 1-10 这类前缀相同的设备
        if "$USBIP" -t "$SERVER_PORT" list -r 127.0.0.1 2>/dev/null |
            awk -v id="$HOP1_BUSID:" '$1 == id { found = 1 } END { exit !found }'; then
            bound=1
            break
        fi
        sleep 0.05
    done
    [ "$bound" = "1" ] || die "libusb_server 没有绑定 $HOP1_BUSID，见 $SERVER_LOG"
}

# iso 用例：项目虚拟等时设备 → vhci → libusb_server → vhci → libusb 客户端
# 双跳链路不经过内核 gadget，因此不需要 dummy_hcd / FunctionFS
run_vdev_iso() {
    # 双跳必须落在两个不同的 vhci 控制器上（同一 usb_hcd 共用 bandwidth_mutex，
    # SET_INTERFACE 会死锁）。内核只编了一个控制器时端口 16 不存在
    local controllers
    controllers=$(find /sys/devices/platform -maxdepth 1 -name 'vhci_hcd.*' | wc -l)
    [ "$controllers" -ge 2 ] || \
        die "iso 用例需要 CONFIG_USBIP_VHCI_NR_HCS>=2（当前 $controllers 个控制器）"

    # 上一次异常退出可能留下端口 16 的导入，attach 会直接 EBUSY
    "$USBIP" detach -p "$HOP2_VHCI_PORT" >/dev/null 2>&1 || true

    start_vdev_and_bind

    log "第二次 vhci 导入（libusb_server → 本机 USB 设备，指定端口 $HOP2_VHCI_PORT）"
    # usbip 命令行固定选第一个空闲端口（永远在控制器 0）；hop2 必须落到
    # 控制器 1 的 hs HCD 上，否则两个导入设备共用同一把 bandwidth_mutex，
    # 客户端一切 SET_INTERFACE 就会和 libusb_server 转发的那次互等死锁
    local hop2_before hop2_busid
    hop2_before=$(matching_busids | sort)
    DETACH_PORT_B=$HOP2_VHCI_PORT
    python3 "$SCRIPT_DIR/attach_port.py" --server-port "$SERVER_PORT" --busid "$HOP1_BUSID" \
            --vhci-port "$HOP2_VHCI_PORT" || die "第二次导入到端口 $HOP2_VHCI_PORT 失败"
    hop2_busid=$(wait_new_busid "$hop2_before")
    [ -n "$hop2_busid" ] || die "5 秒内没有出现第二次导入的设备节点"
    ports_snapshot | grep -qx "$HOP2_VHCI_PORT" || die "端口 $HOP2_VHCI_PORT 没有进入导入状态"
    log "第二次导入后的设备 busid=$hop2_busid（vhci 端口 $DETACH_PORT_B）"

    local bus_number dev_number client_rc
    bus_number=$((10#$(cat "/sys/bus/usb/devices/$hop2_busid/busnum")))
    dev_number=$((10#$(cat "/sys/bus/usb/devices/$hop2_busid/devnum")))
    log "运行客户端 $CLIENT_BIN（/dev/bus/usb/$(printf '%03d' "$bus_number")/$(printf '%03d' "$dev_number")）"
    set +e
    timeout 180 "$BUILD_DIR/$CLIENT_BIN" --bus "$bus_number" --addr "$dev_number" --vid 0x1d6b \
            --pid "$PID_HEX"
    client_rc=$?
    set -e
    log "客户端退出码 $client_rc"
    return "$client_rc"
}

# raw 用例：不经过第二个 vhci，直接按 USB/IP 线协议连 libusb_server，
# 构造 libusb 客户端在本地就会拒绝的非法请求，验证服务端自己的校验/应答/清理
run_vdev_raw() {
    start_vdev_and_bind

    log "运行 raw_client.py（直接走 USB/IP 协议）"
    local client_rc
    set +e
    timeout 120 python3 "$SCRIPT_DIR/raw_client.py" --server-port "$SERVER_PORT" --busid "$HOP1_BUSID"
    client_rc=$?
    set -e
    log "raw 客户端退出码 $client_rc"
    return "$client_rc"
}

if [ "$MODE" = "vdev" ]; then
    if [ "$RAW_MODE" = "1" ]; then
        run_vdev_raw
    else
        run_vdev_iso
    fi
    exit $?
fi

log "加载内核模块"
modprobe dummy_hcd || die "modprobe dummy_hcd 失败"
modprobe libcomposite || die "modprobe libcomposite 失败"
modprobe usb_f_fs || die "modprobe usb_f_fs 失败"
mountpoint -q /sys/kernel/config || mount -t configfs none /sys/kernel/config || \
    die "挂载 configfs 失败"

if [ -d "$GADGET_DIR" ]; then
    log "发现上次残留的 gadget，先尝试清理"
    remove_gadget
    [ ! -d "$GADGET_DIR" ] || die "无法自动清理 $GADGET_DIR，按顺序手动执行：echo '' > $GADGET_DIR/UDC; rm -f $GADGET_DIR/configs/c.1/ffs.$FFS_NAME; rmdir $GADGET_DIR/configs/c.1 $GADGET_DIR/functions/ffs.$FFS_NAME $GADGET_DIR/strings/0x409 $GADGET_DIR"
fi
if mountpoint -q "$FFS_MOUNT" 2>/dev/null; then
    log "发现上次残留的 functionfs 挂载，先卸载"
    umount "$FFS_MOUNT" 2>/dev/null || umount -l "$FFS_MOUNT" 2>/dev/null || true
fi

log "创建 configfs gadget"
mkdir -p "$GADGET_DIR"
printf '0x1d6b\n' > "$GADGET_DIR/idVendor"
printf '%s\n' "$PID_HEX" > "$GADGET_DIR/idProduct"
printf '0x0200\n' > "$GADGET_DIR/bcdUSB"
printf '0x0100\n' > "$GADGET_DIR/bcdDevice"
mkdir -p "$GADGET_DIR/strings/0x409"
printf 'usbipdcpp-e2e-%s\n' "$PROFILE" > "$GADGET_DIR/strings/0x409/serialnumber"
printf 'usbipdcpp\n' > "$GADGET_DIR/strings/0x409/manufacturer"
printf 'usbipdcpp %s\n' "$PROFILE" > "$GADGET_DIR/strings/0x409/product"

mkdir -p "$GADGET_DIR/functions/ffs.$FFS_NAME"
mkdir -p "$FFS_MOUNT"
mount -t functionfs "$FFS_NAME" "$FFS_MOUNT" || die "挂载 functionfs 失败"

log "启动 FunctionFS 设备守护进程"
"$BUILD_DIR/$DEVICE_BIN" "$FFS_MOUNT" > "$DEVICE_LOG" 2>&1 &
DEVICE_PID=$!
for _ in $(seq 1 100); do
    grep -q FFS_READY "$DEVICE_LOG" 2>/dev/null && break
    sleep 0.05
done
grep -q FFS_READY "$DEVICE_LOG" || die "设备守护进程没有写完描述符，见 $DEVICE_LOG"

log "把 function 链进配置并绑定 UDC"
mkdir -p "$GADGET_DIR/configs/c.1"
printf '0xc0\n' > "$GADGET_DIR/configs/c.1/bmAttributes"
printf '0x32\n' > "$GADGET_DIR/configs/c.1/MaxPower"
ln -s "$GADGET_DIR/functions/ffs.$FFS_NAME" "$GADGET_DIR/configs/c.1/ffs.$FFS_NAME"
printf 'dummy_udc.0\n' > "$GADGET_DIR/UDC" || die "绑定 dummy_udc.0 失败"

log "等待主机侧枚举"
BUSID=""
for _ in $(seq 1 100); do
    for device_dir in /sys/bus/usb/devices/*; do
        [ -r "$device_dir/idVendor" ] || continue
        [ "$(cat "$device_dir/idVendor")" = "1d6b" ] || continue
        [ "$(cat "$device_dir/idProduct")" = "$PID_DECIMAL" ] || continue
        [ -r "$device_dir/bConfigurationValue" ] || continue
        # 只认 dummy_hcd 总线上的设备：vhci 上可能残留同 VID/PID 的导入设备，
        # 选错的话服务端绑的就是“导入设备再导出”，链路不再经过 gadget
        case "$(readlink -f "$device_dir")" in
            */dummy_hcd*) ;;
            *) continue ;;
        esac
        BUSID=$(basename "$device_dir")
        break
    done
    [ -n "$BUSID" ] && break
    sleep 0.1
done
[ -n "$BUSID" ] || die "10 秒内设备没有枚举出来，见 $DEVICE_LOG"
log "主机侧 busid=$BUSID"

log "启动 libusb_server（交互模式，只绑定指定设备）"
rm -f "$COMMAND_FIFO"
mkfifo "$COMMAND_FIFO"
# 先用 O_RDWR 打开 FIFO：打开不会阻塞（自己既是读端也是写端），
# 之后服务端再打开读端就不会出现“双方互相等对方打开”的死锁；
# 服务端启动失败退出时，脚本也不会卡在 open 上
exec 9<>"$COMMAND_FIFO"
COMMAND_FD_OPEN=1
"$BUILD_DIR/libusb_server" -p "$PORT" < "$COMMAND_FIFO" > "$SERVER_LOG" 2>&1 &
SERVER_PID=$!

for _ in $(seq 1 100); do
    if "$USBIP" -t "$PORT" list -r 127.0.0.1 >/dev/null 2>&1; then
        break
    fi
    sleep 0.05
done
"$USBIP" -t "$PORT" list -r 127.0.0.1 >/dev/null 2>&1 || die "libusb_server 没有起来，见 $SERVER_LOG"

printf 'b %s\n' "$BUSID" >&9
BOUND=0
for _ in $(seq 1 100); do
    # 精确匹配 busid 字段，避免 1-1 误匹配 1-10 这类前缀相同的设备
    if "$USBIP" -t "$PORT" list -r 127.0.0.1 2>/dev/null |
        awk -v id="$BUSID:" '$1 == id { found = 1 } END { exit !found }'; then
        BOUND=1
        break
    fi
    sleep 0.05
done
[ "$BOUND" = "1" ] || die "libusb_server 没有绑定 $BUSID，见 $SERVER_LOG"

log "vhci 导入设备"
local_busids_before=$(matching_busids | sort)
ports_before=$(ports_snapshot)

"$USBIP" -t "$PORT" attach -r 127.0.0.1 -b "$BUSID" || die "usbip attach 失败"

IMPORTED_BUSID=$(wait_new_busid "$local_busids_before")
[ -n "$IMPORTED_BUSID" ] || die "5 秒内没有出现导入的设备节点"

BUS_NUMBER=$((10#$(cat "/sys/bus/usb/devices/$IMPORTED_BUSID/busnum")))
DEV_NUMBER=$((10#$(cat "/sys/bus/usb/devices/$IMPORTED_BUSID/devnum")))

DETACH_PORT=$(wait_new_port "$ports_before")
[ -n "$DETACH_PORT" ] || die "找不到新占用的 vhci 端口"

log "导入后的设备: /dev/bus/usb/$(printf '%03d' "$BUS_NUMBER")/$(printf '%03d' "$DEV_NUMBER") (bus=$BUS_NUMBER addr=$DEV_NUMBER, vhci 端口 $DETACH_PORT)"

log "运行客户端 $CLIENT_BIN"
set +e
timeout 180 "$BUILD_DIR/$CLIENT_BIN" --bus "$BUS_NUMBER" --addr "$DEV_NUMBER" --vid 0x1d6b \
        --pid "$PID_HEX"
CLIENT_RC=$?
set -e
log "客户端退出码 $CLIENT_RC"
exit "$CLIENT_RC"
