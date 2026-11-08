param(
    [int]$Port = 53240,
    [string]$BusId = '1-1',
    [string]$BuildDir = ''
)

# 重复导入：设备已被占用时第二个客户端 import 必须失败，且不影响第一个
# 客户端的会话；原客户端 detach 后设备应能重新导入。

. "$PSScriptRoot/common.ps1"

Assert-Admin
$BuildDir = Resolve-BuildDir $BuildDir
$logDir = New-E2eLogDir 'repeat_import'
$server = $null
$portNum = $null

try {
    $server = Start-MockServer -Exe (Join-Path $BuildDir 'mock_keyboard.exe') `
        -Port $Port -BusId $BusId -LogDir $logDir -ExtraArgs @('--no-demo')
    $portNum = Attach-Device -Port $Port -BusId $BusId
    $null = Wait-PnpDevice -InstancePattern '*VID_1234&PID_5679*' -Class 'Keyboard'
    Write-Host "[PASS] 第一个客户端导入成功（端口 $portNum）"

    # 预期失败的原生命令会往 stderr 写错误，Stop 策略下会被当成终止错误，
    # 这里临时放宽（同 common.ps1 的 ffmpeg 设备枚举）
    $oldEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $secondOutput = & usbip -t $Port attach -r 127.0.0.1 -b $BusId 2>&1 | Out-String
        $secondExit = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $oldEap
    }
    if ($secondExit -eq 0) {
        throw "重复导入没有被拒绝：$secondOutput"
    }
    Write-Host '[PASS] 重复导入被拒绝'

    $logText = Get-Content -Raw -LiteralPath $server.StdOut -ErrorAction SilentlyContinue
    if ($logText -notmatch '正在使用的设备不支持导出') {
        throw '服务器日志没有记录设备正在使用'
    }
    Write-Host '[PASS] 服务器日志记录设备正在使用'

    if (-not (Test-PortImported -PortNum $portNum)) {
        throw '第一个客户端的端口消失了'
    }
    Write-Host '[PASS] 第一个客户端的会话未受影响'

    Detach-Device -PortNum $portNum
    $portNum = $null
    $portNum = Attach-Device -Port $Port -BusId $BusId
    Write-Host "[PASS] detach 后可再次导入（端口 $portNum）"
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
