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
        & ffmpeg -hide_banner -loglevel warning -f dshow -i "audio=$deviceName" `
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
    Write-Host "[PASS] UAC 麦克风枚举为 $deviceName，录音 $size 字节"
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
