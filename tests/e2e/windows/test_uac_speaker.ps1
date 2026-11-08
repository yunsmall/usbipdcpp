param(
    [int]$Port = 53240,
    [string]$BusId = '1-1',
    [string]$BuildDir = '',
    [int]$ToneHz = 440,
    [int]$ToneSeconds = 2
)

# UAC 扬声器：Windows 枚举成音频输出设备后，用 WinMM 播放已知频率正弦，
# 再对 mock_speaker 收到的 WAV 做 FFT，验证主峰频率与源一致。
# uaudio.sys 对格式/等时包校验很严，只验证"有数据"不够，必须验证 PCM 内容。

. "$PSScriptRoot/common.ps1"

# WinMM 播放（P/Invoke 只能放在 C# 里）；WAV 分析交给 analyze_wav.py（numpy）。
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Runtime.InteropServices;

public static class E2eWinMm {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Auto)]
    public struct WAVEOUTCAPS {
        public ushort wMid;
        public ushort wPid;
        public uint vDriverVersion;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)]
        public string szPname;
        public uint dwFormats;
        public ushort wChannels;
        public ushort wReserved1;
        public uint dwSupport;
    }

    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    public struct WAVEFORMATEX {
        public ushort wFormatTag;
        public ushort nChannels;
        public uint nSamplesPerSec;
        public uint nAvgBytesPerSec;
        public ushort nBlockAlign;
        public ushort wBitsPerSample;
        public ushort cbSize;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct WAVEHDR {
        public IntPtr lpData;
        public uint dwBufferLength;
        public uint dwBytesRecorded;
        public UIntPtr dwUser;
        public uint dwFlags;
        public uint dwLoops;
        public IntPtr lpNext;
        public UIntPtr reserved;
    }

    [DllImport("winmm.dll", CharSet = CharSet.Auto)]
    static extern uint waveOutGetNumDevs();
    [DllImport("winmm.dll", CharSet = CharSet.Auto)]
    static extern uint waveOutGetDevCaps(IntPtr uDeviceID, ref WAVEOUTCAPS caps, uint cbCaps);
    [DllImport("winmm.dll")]
    static extern uint waveOutOpen(out IntPtr hwo, IntPtr uDeviceID, IntPtr pwfx,
        IntPtr dwCallback, IntPtr dwInstance, uint fdwOpen);
    [DllImport("winmm.dll")]
    static extern uint waveOutPrepareHeader(IntPtr hwo, IntPtr pwh, uint cbwh);
    [DllImport("winmm.dll")]
    static extern uint waveOutWrite(IntPtr hwo, IntPtr pwh, uint cbwh);
    [DllImport("winmm.dll")]
    static extern uint waveOutReset(IntPtr hwo);
    [DllImport("winmm.dll")]
    static extern uint waveOutUnprepareHeader(IntPtr hwo, IntPtr pwh, uint cbwh);
    [DllImport("winmm.dll")]
    static extern uint waveOutClose(IntPtr hwo);

    public static string[] GetOutputNames() {
        uint count = waveOutGetNumDevs();
        string[] names = new string[count];
        for (uint i = 0; i < count; i++) {
            WAVEOUTCAPS caps = new WAVEOUTCAPS();
            uint rc = waveOutGetDevCaps((IntPtr)i, ref caps,
                (uint)Marshal.SizeOf(typeof(WAVEOUTCAPS)));
            names[i] = rc == 0 ? caps.szPname : "";
        }
        return names;
    }

    static byte[] ReadAll(string path) {
        using (FileStream fs = new FileStream(path, FileMode.Open, FileAccess.Read,
            FileShare.ReadWrite | FileShare.Delete)) {
            byte[] b = new byte[fs.Length];
            int off = 0;
            while (off < b.Length) {
                int n = fs.Read(b, off, b.Length - off);
                if (n <= 0) break;
                off += n;
            }
            if (off < b.Length) Array.Resize(ref b, off);
            return b;
        }
    }

    // 播放 16 位 PCM WAV 到名称包含 match 的输出设备；播完再关闭
    public static void PlayWav(string path, string match, int extraMs) {
        string[] names = GetOutputNames();
        int index = -1;
        for (int i = 0; i < names.Length; i++) {
            if (names[i] != null && names[i].IndexOf(match, StringComparison.OrdinalIgnoreCase) >= 0) {
                index = i;
                break;
            }
        }
        if (index < 0)
            throw new Exception("找不到包含 " + match + " 的音频输出设备（已枚举：" + String.Join(" | ", names) + "）");

        byte[] b = ReadAll(path);
        int channels = 0, rate = 0, bits = 0, dataOff = -1, dataLen = 0;
        int pos = 12;
        while (pos + 8 <= b.Length) {
            string id = "" + (char)b[pos] + (char)b[pos + 1] + (char)b[pos + 2] + (char)b[pos + 3];
            int size = BitConverter.ToInt32(b, pos + 4);
            int body = pos + 8;
            if (id == "fmt ") {
                channels = BitConverter.ToUInt16(b, body + 2);
                rate = BitConverter.ToInt32(b, body + 4);
                bits = BitConverter.ToUInt16(b, body + 14);
            } else if (id == "data") {
                dataOff = body;
                dataLen = Math.Min(size, b.Length - body);
            }
            pos = body + size + (size % 2);
        }
        if (dataOff < 0 || rate == 0 || bits != 16)
            throw new Exception("WAV 缺少 fmt/data 或不是16位: " + path);

        IntPtr data = Marshal.AllocHGlobal(dataLen);
        Marshal.Copy(b, dataOff, data, dataLen);

        WAVEFORMATEX f = new WAVEFORMATEX();
        f.wFormatTag = 1;
        f.nChannels = (ushort)channels;
        f.nSamplesPerSec = (uint)rate;
        f.nAvgBytesPerSec = (uint)(rate * channels * 2);
        f.nBlockAlign = (ushort)(channels * 2);
        f.wBitsPerSample = 16;
        IntPtr fmt = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(WAVEFORMATEX)));
        Marshal.StructureToPtr(f, fmt, false);

        IntPtr hwo = IntPtr.Zero;
        uint rc = waveOutOpen(out hwo, (IntPtr)index, fmt, IntPtr.Zero, IntPtr.Zero, 0);
        if (rc != 0) {
            Marshal.FreeHGlobal(fmt);
            Marshal.FreeHGlobal(data);
            throw new Exception("waveOutOpen 失败: " + rc);
        }

        WAVEHDR h = new WAVEHDR();
        h.lpData = data;
        h.dwBufferLength = (uint)dataLen;
        IntPtr hdr = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(WAVEHDR)));
        Marshal.StructureToPtr(h, hdr, false);
        uint hdrSize = (uint)Marshal.SizeOf(typeof(WAVEHDR));
        try {
            rc = waveOutPrepareHeader(hwo, hdr, hdrSize);
            if (rc != 0) throw new Exception("waveOutPrepareHeader 失败: " + rc);
            rc = waveOutWrite(hwo, hdr, hdrSize);
            if (rc != 0) throw new Exception("waveOutWrite 失败: " + rc);
            // 轮询 WHDR_DONE（bit0），不按播放时长硬等：驱动缓冲/重采样会改变实际耗时
            int frames = dataLen / (channels * 2);
            int timeoutMs = (int)(frames * 1000.0 / rate) + extraMs + 5000;
            System.Diagnostics.Stopwatch sw = System.Diagnostics.Stopwatch.StartNew();
            while (sw.ElapsedMilliseconds < timeoutMs) {
                WAVEHDR cur = (WAVEHDR)Marshal.PtrToStructure(hdr, typeof(WAVEHDR));
                if ((cur.dwFlags & 1) != 0) break;
                System.Threading.Thread.Sleep(50);
            }
        } finally {
            waveOutReset(hwo);
            waveOutUnprepareHeader(hwo, hdr, hdrSize);
            waveOutClose(hwo);
            Marshal.FreeHGlobal(hdr);
            Marshal.FreeHGlobal(fmt);
            Marshal.FreeHGlobal(data);
        }
    }
}
'@

# mock_speaker 的 ofstream 独占打开输出文件，必须用共享模式读
function Read-SharedBytes {
    param([string]$Path)
    $fs = [IO.File]::Open($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read,
        ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    try {
        $buf = New-Object byte[] $fs.Length
        $off = 0
        while ($off -lt $buf.Length) {
            $n = $fs.Read($buf, $off, $buf.Length - $off)
            if ($n -le 0) { break }
            $off += $n
        }
        if ($off -lt $buf.Length) { [Array]::Resize([ref]$buf, $off) }
        return , $buf
    }
    finally {
        $fs.Dispose()
    }
}

# 只解析 WAV 头，得到采样率与已落盘的 data 帧数
function Get-WavInfo {
    param([byte[]]$Bytes)
    $rate = 0
    $channels = 0
    $bits = 0
    $dataBytes = 0
    $pos = 12
    while ($pos + 8 -le $Bytes.Length) {
        $id = [Text.Encoding]::ASCII.GetString($Bytes, $pos, 4)
        $size = [BitConverter]::ToInt32($Bytes, $pos + 4)
        $body = $pos + 8
        if ($id -eq 'fmt ') {
            $channels = [BitConverter]::ToUInt16($Bytes, $body + 2)
            $rate = [BitConverter]::ToInt32($Bytes, $body + 4)
            $bits = [BitConverter]::ToUInt16($Bytes, $body + 14)
        }
        elseif ($id -eq 'data') {
            $dataBytes = [Math]::Min($size, $Bytes.Length - $body)
        }
        $pos = $body + $size + ($size % 2)
    }
    if ($rate -le 0 -or $channels -le 0 -or $bits -ne 16) {
        return [pscustomobject]@{ Rate = 0; Frames = 0 }
    }
    return [pscustomobject]@{ Rate = $rate; Frames = [int]($dataBytes / ($channels * 2)) }
}

Assert-Admin
$BuildDir = Resolve-BuildDir $BuildDir
$logDir = New-E2eLogDir 'uac_speaker'
$server = $null
$portNum = $null
$recvWav = Join-Path $logDir 'speaker_recv.wav'
$toneWav = Join-Path $logDir 'tone.wav'
$analyzer = Join-Path $PSScriptRoot 'analyze_wav.py'

try {
    Remove-Item -Force $recvWav -ErrorAction SilentlyContinue
    $server = Start-MockServer -Exe (Join-Path $BuildDir 'mock_speaker.exe') `
        -Port $Port -BusId $BusId -LogDir $logDir `
        -ExtraArgs @('--output', $recvWav)
    $portNum = Attach-Device -Port $Port -BusId $BusId

    $null = Wait-PnpDevice -InstancePattern '*VID_1234&PID_5683*' -Class 'MEDIA'

    # 等 WinMM 能枚举到设备（uaudio 初始化晚于 PnP 出现）
    $deadline = (Get-Date).AddSeconds(20)
    $found = $false
    while ((Get-Date) -lt $deadline -and -not $found) {
        foreach ($name in [E2eWinMm]::GetOutputNames()) {
            if ($name -like '*Usbipdcpp*') { $found = $true; break }
        }
        if (-not $found) { Start-Sleep -Milliseconds 500 }
    }
    if (-not $found) {
        throw 'WinMM 枚举不到 Usbipdcpp 音频输出设备'
    }

    & python -I $analyzer gen $toneWav $ToneHz 48000 $ToneSeconds | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "生成测试音失败（exit $LASTEXITCODE）"
    }
    [E2eWinMm]::PlayWav($toneWav, 'Usbipdcpp', 1500)

    # mock_speaker 的 WavFileSink 在格式切换时会 trunc 重开文件，
    # 不能只看大小增量；等接收端累计到至少 1 秒数据后取快照分析。
    # 设备可能一直保持流（文件持续增长），所以只等"够长"，不等"稳定"。
    $deadline = (Get-Date).AddSeconds(20)
    $frames = 0
    $rate = 0
    while ((Get-Date) -lt $deadline) {
        try {
            $info = Get-WavInfo (Read-SharedBytes $recvWav)
            $rate = $info.Rate
            $frames = $info.Frames
        }
        catch {
            $rate = 0
            $frames = 0
        }
        if ($rate -gt 0 -and $frames -ge $rate) { break }
        Start-Sleep -Milliseconds 300
    }
    if ($rate -le 0 -or $frames -lt $rate) {
        throw "接收端只拿到 $frames 帧 @ ${rate}Hz（不足 1 秒），uaudio 可能没送出有效数据"
    }

    $snapshot = Join-Path $logDir 'speaker_recv_snapshot.wav'
    [IO.File]::WriteAllBytes($snapshot, (Read-SharedBytes $recvWav))
    $json = & python -I $analyzer $snapshot
    if ($LASTEXITCODE -ne 0) {
        throw "WAV 分析失败（exit $LASTEXITCODE）"
    }
    $analysis = $json | ConvertFrom-Json

    if ([Math]::Abs($analysis.peak_hz - $ToneHz) -gt 8) {
        throw "主峰频率 $($analysis.peak_hz) Hz 与源 $ToneHz Hz 偏差过大（重采样或打包错误）"
    }
    if ($analysis.peak_ratio -lt 0.3) {
        throw "主峰能量占比只有 $($analysis.peak_ratio)，接收到的不是干净单音（数据面可能损坏）"
    }
    if ($analysis.rms -lt 1000) {
        throw "接收 RMS 仅 $($analysis.rms)，信号过弱/基本是静音"
    }

    # 分段检查：能量足够大的每一段（跳过尾部静音）主峰都必须落在源频率附近，
    # 防止"整体 FFT 正确但中间丢块/错位"。
    $loud = @($analysis.segments | Where-Object { $_.rms -ge 0.5 * $analysis.rms })
    if ($loud.Count -lt 3) {
        throw "能量足够的分段只有 $($loud.Count) 个（期望 >= 3），播放数据不完整"
    }
    foreach ($seg in $loud) {
        if ([Math]::Abs($seg.peak_hz - $ToneHz) -gt 8) {
            throw "在 $($seg.t)s 处主峰为 $($seg.peak_hz)Hz（期望 $ToneHz Hz），数据面出现错位/丢块"
        }
    }

    Write-Host (('[PASS] UAC 扬声器：接收 {0} 帧 @ {1}Hz，FFT 主峰 {2}Hz（占比 {3}），' +
        '{4}/{5} 个有效分段峰值全部正确') -f $analysis.frames, $analysis.rate,
        $analysis.peak_hz, $analysis.peak_ratio, $loud.Count, $analysis.segments.Count)
}
finally {
    if ($null -ne $portNum) {
        try { Detach-Device -PortNum $portNum }
        catch { Write-Warning "detach 失败：$_" }
    }
    if ($null -ne $server) {
        try { Stop-ServerSafely -Server $server -BusId $BusId }
        catch { Write-Warning "停止服务器失败：$_" }
    }
}
