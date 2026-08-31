#!/usr/bin/env bash
# cdc_acm 端到端：attach 后串口出现，回显串口写读精确一致。
# 读端用 dd 一次性读固定字节数（count=1 读第一份即退出，避开 usb-serial
# 持续读会重复推数据的问题）；写读前关 tty 转换（echo/icrnl/onlcr）防干扰
set -e
source "$(dirname "$0")/common.sh"

setup_workdir cdc_acm
echo "== cdc_acm: 启动服务器 + attach"
start_server mock_cdc_acm || { report cdc_acm; exit 1; }
attach_device 1-1

TTY=$(wait_dev "/dev/ttyACM" 10) || true
assert "串口出现（$TTY）" [ -n "$TTY" ]
[ -n "$TTY" ] || { stop_server; report cdc_acm; exit 1; }

sleep 1 # 等驱动就绪（ACM 初始化需要一点时间）

# 写读回显：python3 子脚本一个 fd 完成 termios（关 echo/规范模式等）+ 写 + 读回。
# 不用 sh 的 tee/dd 分进程：写进程退出、读进程后开时实测收不到回显；
# 且串口默认规范模式按行返回读，数据无换行时读端永远等不到行结束符
MSG="E2E_$(date +%s)"
GOT=$(timeout 12 sudo python3 "$E2E_DIR/cdc_acm_test.py" "$TTY" "$MSG" 2>/dev/null)
assert "回显精确一致（[$GOT]）" [ "$GOT" = "$MSG" ]

stop_server
report cdc_acm
