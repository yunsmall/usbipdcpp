#!/usr/bin/env bash
# uvc 端到端：attach 后 /dev/video0 出现，ffmpeg 实际拉帧验证 ISO 取流
# （mock_uvc 默认 320x240 彩条；拉出图片非空即取流通）
set -e
source "$(dirname "$0")/common.sh"

setup_workdir uvc
echo "== uvc: 启动服务器 + attach"
start_server mock_uvc || { report uvc; exit 1; }
attach_device 1-1

VIDEO=$(wait_dev "/dev/video" 15) || true
assert "video 设备出现（$VIDEO）" [ -n "$VIDEO" ]
[ -n "$VIDEO" ] || { stop_server; report uvc; exit 1; }

# 实际取流：ffmpeg 拉 5 帧（v4l2 自动协商 mock 默认格式），输出非空即通
if command -v ffmpeg > /dev/null 2>&1; then
    OUT="$WORK_DIR/frame.jpg"
    # -update 1：image2 多帧写同一文件需覆盖模式，否则报 "Cannot write more
    # than one file with the same name"
    timeout 15 ffmpeg -loglevel error -f v4l2 -i "$VIDEO" -frames:v 5 -update 1 -y "$OUT" || true
    assert "ffmpeg 拉到帧（$OUT）" [ -s "$OUT" ]
fi

stop_server
report uvc
