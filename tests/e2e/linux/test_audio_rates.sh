#!/usr/bin/env bash
# UAC 采样率切换：设备声明 48000/44100 两个采样率，主机分别按两个速率录音，
# 验证 SET_CUR（采样率控制请求）切换后数据流仍然有效且音调正确。
# 现有 test_audio 只验证默认 48kHz 能录到非空文件。
set -e
source "$(dirname "$0")/common.sh"

setup_workdir audio_rates
echo "== audio_rates: 启动服务器（rates 48000,44100）"
start_server mock_audio --rates 48000,44100 || { report audio_rates; exit 1; }
attach_device 1-1

CARDS=$(wait_usb_soundcard 10) || true
assert "usbip 声卡出现" [ -n "$CARDS" ]
[ -n "$CARDS" ] || { stop_server; report audio_rates; exit 1; }

if ! command -v arecord > /dev/null 2>&1; then
    echo "  [SKIP] 无 arecord，跳过录音验证"
    stop_server
    report audio_rates
    exit 0
fi

CARD_NO=$(grep -oP '^\s*\K[0-9]+' /proc/asound/cards | head -1)

# 先按设备初始采样率录，再切到 44100 录，最后切回 48000
for RATE in 48000 44100 48000; do
    REC="$WORK_DIR/rec_${RATE}_$(date +%s%N).wav"
    timeout 10 arecord -q -D "hw:$CARD_NO,0" -f S16_LE -r "$RATE" -c 1 -d 2 "$REC" || true
    sleep 1 # 等推流收尾
    assert "${RATE}Hz 录音文件非空" [ -s "$REC" ]
    if [ -s "$REC" ] && command -v python3 > /dev/null 2>&1; then
        TONE=$(timeout 15 python3 "$SHARED_DIR/tone_check.py" "$REC" 440 2>&1 || true)
        assert "${RATE}Hz 录音音调为 440Hz（$(tail -2 <<<"$TONE" | head -1)）" \
            grep -q "MATCH" <<<"$TONE"
    fi
done

stop_server
report audio_rates
