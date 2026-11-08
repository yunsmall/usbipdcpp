#!/usr/bin/env python3
# 校验 WAV 的单音频率（过零率估频），用于麦克风录音的内容级验证：
# 只判断频率，不比较极值数——录音时长与源文件不同，极值数无法直接对齐
# （speaker_verify.py 是源/收流两文件比对，不能用在这里）。
import array
import sys
import wave

if len(sys.argv) != 3:
    print("usage: tone_check.py <wav> <expected_hz>")
    sys.exit(2)

path = sys.argv[1]
expected = float(sys.argv[2])

w = wave.open(path)
ch = w.getnchannels()
sr = w.getframerate()
n = w.getnframes()
data = array.array("h")
data.frombytes(w.readframes(n))
if ch > 1:
    data = data[::ch]
if not data or n == 0:
    print("EMPTY")
    sys.exit(2)

crossings = sum(1 for i in range(1, len(data)) if (data[i - 1] < 0) != (data[i] < 0))
freq = crossings / 2.0 / (n / sr)

ok = abs(freq - expected) / expected <= 0.02
print(f"freq={freq:.1f}Hz expected={expected:.1f}Hz frames={n}")
print("MATCH" if ok else "MISMATCH")
sys.exit(0 if ok else 1)
