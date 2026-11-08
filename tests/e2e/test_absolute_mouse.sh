#!/usr/bin/env bash
# 绝对坐标鼠标：attach 后主机出现鼠标输入设备，通过示例的命令管道发送移动
# 命令，evtest 捕获到绝对坐标事件（EV_ABS/ABS_X/ABS_Y）。验证 absolute_mouse
# 的坐标映射链路（现有 HID 测试只覆盖相对鼠标）。
#
# absolute_mouse 从 stdin 读命令，stdin EOF 会直接退出；start_server 把 stdin
# 接到 /dev/null 无法使用，这里自己起进程并用 FIFO 保持命令通道。
set -e
source "$(dirname "$0")/common.sh"

setup_workdir absolute_mouse
FIFO="$WORK_DIR/cmd.fifo"
mkfifo "$FIFO"

detach_all
kill_by_port
wait_port_free || { report absolute_mouse; exit 1; }

echo "== absolute_mouse: 启动服务器（stdin 走命令 FIFO）"
cd "$WORK_DIR"
setsid "$BUILD_DIR/absolute_mouse" < "$FIFO" > "$LOG_DIR/absolute_mouse.log" 2>&1 &
# 以读写方式打开 FIFO（O_RDWR 不阻塞，普通只写打开在没有读端时会永久挂住），
# 同时保持写端存活，否则示例 getline 读到 EOF 直接退出
exec 9<>"$FIFO"
if ! wait_port_ready; then
    echo "FAIL: absolute_mouse 未监听端口（见 $LOG_DIR/absolute_mouse.log）"
    exec 9>&-
    report absolute_mouse
    exit 1
fi

attach_device 1-1
MOUSE=""
for _ in $(seq 1 100); do
    MOUSE=$(ls /dev/input/by-path/*event-mouse 2>/dev/null | grep -i usb | head -1)
    [ -n "$MOUSE" ] && break
    sleep 0.1
done
assert "绝对坐标鼠标设备出现（$MOUSE）" [ -n "$MOUSE" ]
[ -n "$MOUSE" ] || {
    exec 9>&-
    kill_by_port
    report absolute_mouse
    exit 1
}

# 后台捕获 4 秒事件，期间通过命令通道发两次移动
timeout 4 sudo evtest "$MOUSE" > "$WORK_DIR/ev.log" 2>&1 &
EV_PID=$!
sleep 1
echo "pos 500 400" >&9
sleep 0.5
echo "pos 100 200" >&9
wait "$EV_PID" || true

EV_ABS=$(grep -c "EV_ABS" "$WORK_DIR/ev.log" 2>/dev/null || true)
ABS_X=$(grep -c "ABS_X" "$WORK_DIR/ev.log" 2>/dev/null || true)
ABS_Y=$(grep -c "ABS_Y" "$WORK_DIR/ev.log" 2>/dev/null || true)
assert "捕获到绝对坐标事件（$EV_ABS 条）" [ "$EV_ABS" -ge 1 ]
assert "捕获到 ABS_X（$ABS_X 条）" [ "$ABS_X" -ge 1 ]
assert "捕获到 ABS_Y（$ABS_Y 条）" [ "$ABS_Y" -ge 1 ]

# 优雅退出：发 q 后关闭写端
echo "q" >&9 || true
exec 9>&-
sleep 0.5
kill_by_port
# 端口释放后清理设备
detach_all
report absolute_mouse
