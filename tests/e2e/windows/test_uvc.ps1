param(
    [int]$Port = 53240,
    [string]$BusId = '1-1',
    [string]$BuildDir = ''
)

# UVC：Windows 枚举成 Camera 后，用 ffmpeg 通过 usbvideo.sys 真正抓一帧。
# 只枚举成功不代表数据面通——usbvideo.sys 会对格式/帧率/等时包做严格校验。

. "$PSScriptRoot/common.ps1"

Assert-Admin
$BuildDir = Resolve-BuildDir $BuildDir
$logDir = New-E2eLogDir 'uvc'
$server = $null
$portNum = $null

try {
    $server = Start-MockServer -Exe (Join-Path $BuildDir 'mock_uvc.exe') `
        -Port $Port -BusId $BusId -LogDir $logDir
    $portNum = Attach-Device -Port $Port -BusId $BusId

    $null = Wait-PnpDevice -InstancePattern '*VID_1234&PID_5681*' -Class 'Camera'
    $deviceName = Get-DshowDeviceName -Kind 'video' -Keyword 'Usbipdcpp'

    $frame = Join-Path $logDir 'frame.jpg'
    Remove-Item -Force $frame -ErrorAction SilentlyContinue
    # ffmpeg 的进度/警告走 stderr，Stop 策略下会被当成终止错误，临时放宽
    $oldEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & ffmpeg -hide_banner -loglevel warning -f dshow -i "video=$deviceName" `
            -frames:v 1 -update 1 -y $frame 2>&1 | Out-Null
        $ffmpegExit = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $oldEap
    }
    if ($ffmpegExit -ne 0) {
        throw "ffmpeg 抓帧失败（exit $ffmpegExit）"
    }
    $size = (Get-Item $frame).Length
    if ($size -lt 1000) {
        throw "抓到的帧过小：$size 字节"
    }
    Write-Host "[PASS] UVC 枚举为 Camera，ffmpeg 抓帧 $size 字节"
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
