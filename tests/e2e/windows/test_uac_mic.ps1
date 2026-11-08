param(
    [int]$Port = 53240,
    [string]$BusId = '1-1',
    [string]$BuildDir = ''
)

# UAC 麦克风：Windows 枚举成音频输入设备后，用 ffmpeg 录音 2 秒。
# uaudio.sys 对采样率/通道/等时包格式的校验很严，枚举成功但录不到音是常见故障。

. "$PSScriptRoot/common.ps1"

Assert-Admin
$BuildDir = Resolve-BuildDir $BuildDir
$logDir = New-E2eLogDir 'uac_mic'
$server = $null
$portNum = $null

try {
    $server = Start-MockServer -Exe (Join-Path $BuildDir 'mock_audio.exe') `
        -Port $Port -BusId $BusId -LogDir $logDir
    $portNum = Attach-Device -Port $Port -BusId $BusId

    $null = Wait-PnpDevice -InstancePattern '*VID_1234&PID_5682*' -Class 'MEDIA'
    $deviceName = Get-DshowDeviceName -Kind 'audio' -Keyword 'Usbipdcpp'

    $wav = Join-Path $logDir 'record.wav'
    Remove-Item -Force $wav -ErrorAction SilentlyContinue
    # ffmpeg 的进度/警告走 stderr，Stop 策略下会被当成终止错误，临时放宽
    $oldEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        # -sample_rate 是 dshow 输入参数：按设备原生 48kHz 打开，避免 Windows
        # 音频引擎以 44.1kHz 重采样（重采样时钟偏差会让 440Hz 估频偏出容差）
        & ffmpeg -hide_banner -loglevel warning -f dshow -sample_rate 48000 -i "audio=$deviceName" `
            -t 2 -y $wav 2>&1 | Out-Null
        $ffmpegExit = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $oldEap
    }
    if ($ffmpegExit -ne 0) {
        throw "ffmpeg 录音失败（exit $ffmpegExit）"
    }
    $size = (Get-Item $wav).Length
    if ($size -lt 10000) {
        throw "录音数据过小：$size 字节（uaudio 没收到等时数据）"
    }

    # 内容级校验用 FFT 主峰（与 speaker 测试同一分析器）；过零率法会被
    # Windows 音频栈引入的少量毛刺/直流偏置带偏，实测 FFT 主峰为 440.2Hz
    $analyzer = Join-Path $PSScriptRoot 'analyze_wav.py'
    $oldEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $analysis = & python $analyzer $wav 2>&1 | Out-String
    }
    finally {
        $ErrorActionPreference = $oldEap
    }
    $json = $analysis | ConvertFrom-Json
    $peak = [double]$json.peak_hz
    if ([math]::Abs($peak - 440.0) / 440.0 -gt 0.02) {
        throw "录音主频不符（期望 440Hz）：$analysis"
    }
    Write-Host "[PASS] UAC 麦克风枚举为 $deviceName，录音 $size 字节，主频 $($peak.ToString('F1'))Hz"
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
