#!/usr/bin/env bash
# speaker 端到端：attach 后 usbip 播放声卡出现，aplay 播放已知 440Hz 正弦波，
# mock_speaker --output 收流写文件，内容级比对（频率/波峰波谷数一致，
# 不是简单"文件非空"）
set -e
source "$(dirname "$0")/common.sh"

setup_workdir speaker
echo "== speaker: 启动服务器（--output 收流写文件）+ attach"
start_server mock_speaker --output "$WORK_DIR/speaker_rec.wav" || { report speaker; exit 1; }
attach_device 1-1

CARDS=$(wait_usb_soundcard 10) || true
assert "usbip 声卡出现" [ -n "$CARDS" ]

# 生成 440Hz 正弦波源文件（48kHz 16bit 单声道 1 秒）
python3 - "$WORK_DIR/tone.wav" <<'EOF'
import array
import math
import sys
import wave

sr = 48000
data = array.array("h", (int(12000 * math.sin(2 * math.pi * 440 * i / sr)) for i in range(sr)))
w = wave.open(sys.argv[1], "wb")
w.setnchannels(1)
w.setsampwidth(2)
w.setframerate(sr)
w.writeframes(data.tobytes())
w.close()
EOF

# 播放 → ISO OUT 收流 → mock_speaker 写入 WAV 文件
if command -v aplay > /dev/null 2>&1; then
    CARD_NO=$(grep -oP '^\s*\K[0-9]+' /proc/asound/cards | head -1)
    aplay -q -D "hw:$CARD_NO,0" "$WORK_DIR/tone.wav" || true
    sleep 1.5 # 等收流收尾（WAV 头回填）
    # 内容级比对：频率（过零率估频）与波峰波谷数
    timeout 15 python3 "$E2E_DIR/speaker_verify.py" "$WORK_DIR/tone.wav" \
        "$WORK_DIR/speaker_rec.wav" > "$WORK_DIR/verify.out" 2>&1 || true
    assert "收流内容与源一致（$(head -2 "$WORK_DIR/verify.out" | tail -1)）" \
        grep -q "MATCH" "$WORK_DIR/verify.out"
else
    echo "  [SKIP] 无 aplay，跳过播放验证"
fi

stop_server
report speaker
