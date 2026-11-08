#!/usr/bin/env bash
# webcam 端到端：单个复合设备同时出摄像头 + 麦克风（UVC VS 取流 + UAC AS 推流）。
# attach 后 /dev/video0 与 usbip 声卡都出现，ffmpeg 拉帧 + arecord 录音双验证
# （mock_webcam 默认 320x240 彩条 + 440Hz 正弦波）
set -e
source "$(dirname "$0")/common.sh"

setup_workdir webcam
echo "== webcam: 启动服务器 + attach"
start_server mock_webcam || { report webcam; exit 1; }
attach_device 1-1

VIDEO=$(wait_dev "/dev/video" 15) || true
assert "video 设备出现（$VIDEO）" [ -n "$VIDEO" ]

CARDS=$(wait_usb_soundcard 10) || true
assert "usbip 声卡出现" [ -n "$CARDS" ]
[ -n "$VIDEO" ] && [ -n "$CARDS" ] || { stop_server; report webcam; exit 1; }

# 实际取流：ffmpeg 拉 5 帧（v4l2 自动协商 mock 默认格式），输出非空即通
if command -v ffmpeg > /dev/null 2>&1; then
    OUT="$WORK_DIR/frame.jpg"
    # -update 1：image2 多帧写同一文件需覆盖模式，否则报 "Cannot write more
    # than one file with the same name"
    timeout 15 ffmpeg -loglevel error -f v4l2 -i "$VIDEO" -frames:v 5 -update 1 -y "$OUT" || true
    assert "ffmpeg 拉到帧（$OUT）" [ -s "$OUT" ]
fi

# 实际录音：arecord 录 2 秒（48kHz 16 位单声道，mock_webcam 默认），
# 文件非空即取流通（数据经 ISO IN 从服务器推给主机声卡）
if command -v arecord > /dev/null 2>&1; then
    CARD_NO=$(grep -oP '^\s*\K[0-9]+' /proc/asound/cards | head -1)
    REC="$WORK_DIR/rec.wav"
    timeout 8 arecord -q -D "hw:$CARD_NO,0" -f S16_LE -r 48000 -c 1 -d 2 "$REC" || true
    sleep 1 # 等推流收尾
    assert "录音文件非空（$REC）" [ -s "$REC" ]
fi

stop_server
report webcam
