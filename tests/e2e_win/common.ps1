# Windows 端 e2e 公共函数
#
# 核心约束：attach 之后必须先 `usbip detach` 并确认端口消失，才允许停止服务器。
# 直接断 socket（杀服务器/断网）会让 usbip-win2 驱动处于异常状态，极易蓝屏。

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-Admin {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw '需要管理员权限（usbip attach/detach 写系统设备树）'
    }
}

function Wait-TcpPort {
    param([int]$Port, [int]$TimeoutSec = 15)
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        $client = New-Object Net.Sockets.TcpClient
        try {
            $client.Connect('127.0.0.1', $Port)
            $client.Close()
            return
        }
        catch {
            $client.Dispose()
        }
        Start-Sleep -Milliseconds 200
    }
    throw "端口 $Port 未监听（服务器没起来？）"
}

function Start-MockServer {
    param(
        [string]$Exe,
        [int]$Port,
        [string]$BusId,
        [string]$LogDir,
        [string[]]$ExtraArgs = @()
    )
    if (-not (Test-Path $Exe)) {
        throw "找不到服务器程序：$Exe（先编译？）"
    }
    $name = [IO.Path]::GetFileNameWithoutExtension($Exe)
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
    $stdout = Join-Path $LogDir "$stamp-$name.out.log"
    $stderr = Join-Path $LogDir "$stamp-$name.err.log"
    $serverArgs = @('-p', $Port, '-b', $BusId) + $ExtraArgs
    $proc = Start-Process -FilePath $Exe `
        -ArgumentList $serverArgs `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr `
        -PassThru -NoNewWindow
    Wait-TcpPort -Port $Port
    return [pscustomobject]@{
        Process = $proc
        StdOut  = $stdout
        StdErr  = $stderr
    }
}

function Attach-Device {
    param([int]$Port, [string]$BusId)
    $out = & usbip -t $Port attach -r 127.0.0.1 -b $BusId 2>&1 | Out-String
    $match = [regex]::Match($out, 'port\s+(\d+)')
    if (-not $match.Success) {
        throw "attach 失败：$out"
    }
    return [int]$match.Groups[1].Value
}

function Test-PortImported {
    param([int]$PortNum)
    $text = & usbip port 2>&1 | Out-String
    return ($text -match "(?m)^Port\s+0*$PortNum\s*:")
}

function Detach-Device {
    param([int]$PortNum)
    $detached = $false
    for ($i = 0; $i -lt 5; $i++) {
        $out = & usbip detach -p $PortNum 2>&1 | Out-String
        if ($LASTEXITCODE -eq 0) {
            $detached = $true
            break
        }
        Start-Sleep -Seconds 1
    }
    if (-not $detached) {
        throw "detach 端口 $PortNum 失败（重试 5 次）"
    }
    # detach 返回成功不代表设备树已完全移除，轮询确认端口真的消失
    $deadline = (Get-Date).AddSeconds(10)
    while ((Get-Date) -lt $deadline) {
        if (-not (Test-PortImported -PortNum $PortNum)) {
            return
        }
        Start-Sleep -Milliseconds 200
    }
    throw "detach 后端口 $PortNum 仍存在，拒绝继续操作"
}

function Stop-ServerSafely {
    param($Server, [string]$BusId)
    # 最后一道防线：服务器进程只能在设备完全 detach 后停止。
    # 否则 socket 被直接断开，usbip-win2 驱动可能蓝屏。
    $text = & usbip port 2>&1 | Out-String
    if ($text -match [regex]::Escape($BusId)) {
        throw "设备 $BusId 仍被导入，拒绝停止服务器"
    }
    if ($null -ne $Server -and -not $Server.Process.HasExited) {
        Stop-Process -Id $Server.Process.Id -Force
        $null = $Server.Process.WaitForExit(5000)
    }
}

function Wait-PnpDevice {
    param(
        [string]$InstancePattern,
        [string]$Class,
        [int]$TimeoutSec = 20
    )
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        $devices = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
            Where-Object {
                $_.InstanceId -like $InstancePattern -and
                $_.Status -eq 'OK' -and
                ($null -eq $Class -or $_.Class -eq $Class)
            }
        if ($devices) {
            return $devices
        }
        Start-Sleep -Milliseconds 500
    }
    throw "未等到 PnP 设备：$InstancePattern (Class=$Class)"
}

function Get-DshowDeviceName {
    param([string]$Kind, [string]$Keyword)
    # ffmpeg 把设备列表写到 stderr；Stop 策略下原生命令的 stderr 会被当成
    # 终止错误，这里临时放宽，否则脚本在列出设备阶段就中断
    $oldEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $text = & ffmpeg -hide_banner -list_devices true -f dshow -i dummy 2>&1 | Out-String
    }
    finally {
        $ErrorActionPreference = $oldEap
    }
    $pattern = '"([^"]*' + [regex]::Escape($Keyword) + '[^"]*)"\s*\(' + $Kind + '\)'
    $match = [regex]::Match($text, $pattern)
    if (-not $match.Success) {
        throw "dshow 中找不到包含 $Keyword 的 $Kind 设备"
    }
    return $match.Groups[1].Value
}

function New-E2eLogDir {
    param([string]$TestName)
    $dir = Join-Path $env:TEMP "usbip_e2e_win\$TestName"
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    return $dir
}

function Resolve-BuildDir {
    param([string]$BuildDir)
    if ($BuildDir) {
        return (Resolve-Path $BuildDir).Path
    }
    $root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
    return (Join-Path $root 'build')
}
