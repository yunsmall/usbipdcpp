"""分析 WAV 的主频与能量分布，供 Windows e2e 脚本验证数据面。

用法：
    analyze_wav.py <file.wav>                 分析并输出一行 JSON
    analyze_wav.py gen <file> <freq> <rate> <seconds>   生成 16 位单声道正弦 WAV

输出 JSON 包含采样率、帧数、整体 RMS、主峰频率/占比，以及分段（0.25 秒）
的 RMS 与峰值频率，用来发现"只有局部正确"的打包错误。
"""

import json
import sys
import wave

import numpy as np


def gen_tone(path, freq, rate, seconds):
    """生成幅度为满量程 1/8 的 16 位 PCM 单声道正弦。"""
    n = int(rate) * int(seconds)
    t = np.arange(n, dtype=np.float64)
    samples = np.sin(2 * np.pi * int(freq) * t / int(rate)) * 4096
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(int(rate))
        w.writeframes(samples.astype(np.int16).tobytes())


def main():
    path = sys.argv[1]
    with wave.open(path, "rb") as w:
        rate = w.getframerate()
        channels = w.getnchannels()
        width = w.getsampwidth()
        frames = w.getnframes()
        raw = w.readframes(frames)

    if width != 2:
        raise SystemExit(f"只支持 16 位 PCM，实际 {width * 8} 位")

    data = np.frombuffer(raw, dtype=np.int16).astype(np.float64)
    if channels > 1:
        data = data.reshape(-1, channels).mean(axis=1)
    if len(data) == 0:
        raise SystemExit("没有采样数据")

    def spectrum(seg):
        n = len(seg)
        win = np.hanning(n)
        sp = np.abs(np.fft.rfft(seg * win))
        freqs = np.fft.rfftfreq(n, 1.0 / rate)
        # 只看 20Hz 以上，避免直流分量抢主峰
        lo = max(1, int(20 * n / rate))
        k = lo + int(np.argmax(sp[lo:]))
        total = float(np.sum(sp[lo:] ** 2))
        ratio = float(sp[k] ** 2 / total) if total > 0 else 0.0
        return freqs[k], ratio

    # 整体分析取中间一段 2 的幂长度，避开首尾的启停瞬态
    n = 1
    while n * 2 <= min(65536, len(data)):
        n *= 2
    start = (len(data) - n) // 2
    peak_hz, peak_ratio = spectrum(data[start:start + n])

    seg_len = max(1024, rate // 4)
    segments = []
    for i in range(0, len(data) - seg_len + 1, seg_len):
        seg = data[i:i + seg_len]
        hz, _ = spectrum(seg)
        segments.append({
            "t": round(i / rate, 3),
            "rms": round(float(np.sqrt(np.mean(seg ** 2))), 1),
            "peak_hz": round(float(hz), 1),
        })

    print(json.dumps({
        "rate": rate,
        "channels": channels,
        "frames": len(data),
        "rms": round(float(np.sqrt(np.mean(data ** 2))), 1),
        "peak_hz": round(float(peak_hz), 2),
        "peak_ratio": round(peak_ratio, 4),
        "segments": segments,
    }))


if __name__ == "__main__":
    if sys.argv[1] == "gen":
        gen_tone(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5])
    else:
        main()
