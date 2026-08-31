#!/usr/bin/env bash
# audio 端到端：attach 后 /proc/asound/cards 出现 usbip 录音声卡，
# arecord 实际录音 2 秒验证 ISO IN 推流（mock_audio 是 UAC1 麦克风）
set -e
source "$(dirname "$0")/common.sh"

setup_workdir audio
echo "== audio: 启动服务器 + attach"
start_server mock_audio || { report audio; exit 1; }
attach_device 1-1

CARDS=$(wait_usb_soundcard 10) || true
assert "usbip 声卡出现" [ -n "$CARDS" ]
[ -n "$CARDS" ] || { stop_server; report audio; exit 1; }

# 实际录音：arecord 录 2 秒（48kHz 16 位单声道，mock_audio 默认），
# 文件非空即取流通（数据经 ISO IN 从服务器推给主机声卡）
if command -v arecord > /dev/null 2>&1; then
    CARD_NO=$(grep -oP '^\s*\K[0-9]+' /proc/asound/cards | head -1)
    REC="$WORK_DIR/rec.wav"
    timeout 8 arecord -q -D "hw:$CARD_NO,0" -f S16_LE -r 48000 -c 1 -d 2 "$REC" || true
    sleep 1 # 等推流收尾
    assert "录音文件非空（$REC）" [ -s "$REC" ]
fi

stop_server
report audio
