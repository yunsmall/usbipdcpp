param(
    [int]$Port = 53240,
    [string]$BusId = '1-1',
    [string]$BuildDir = '',
    [switch]$TestInput
)

# 键盘 LED：默认只验证键盘枚举（不碰宿主机输入状态）。传 -TestInput 才切换
# Caps Lock，Windows 会向 HID 键盘下发 SET_REPORT(Output)，服务器侧
# KeyboardHandler 的 LED 状态随日志变化；测试前后恢复 Caps Lock。

. "$PSScriptRoot/common.ps1"

Assert-Admin
$BuildDir = Resolve-BuildDir $BuildDir
$logDir = New-E2eLogDir 'keyboard_led'
$server = $null
$portNum = $null
$initialCaps = [Console]::CapsLock

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
public static class NativeKeyboard {
    [DllImport("user32.dll")]
    public static extern void keybd_event(byte bVk, byte bScan, uint dwFlags, UIntPtr dwExtraInfo);
}
"@

function Toggle-CapsLock {
    # VK_CAPITAL = 0x14，KEYEVENTF_KEYUP = 0x02
    [NativeKeyboard]::keybd_event(0x14, 0, 0, [UIntPtr]::Zero)
    [NativeKeyboard]::keybd_event(0x14, 0, 2, [UIntPtr]::Zero)
}

function Get-LogLineCount {
    param([string]$Path)
    $lines = Get-Content -LiteralPath $Path -ErrorAction SilentlyContinue
    if ($null -eq $lines) { return 0 }
    return @($lines).Count
}

function Wait-NewLogPattern {
    # 只看调用基线之后新增的行：日志里已有 attach 时的旧 LED 值，
    # 直接匹配整份文件会把旧值当成这次切换的结果（假阳性）
    param([string]$Path, [string]$Pattern, [int]$BaselineLines, [int]$TimeoutSec = 10)
    $deadline = (Get-Date).AddSeconds($TimeoutSec)
    while ((Get-Date) -lt $deadline) {
        $lines = Get-Content -LiteralPath $Path -ErrorAction SilentlyContinue
        if ($null -ne $lines) {
            $new = @($lines | Select-Object -Skip $BaselineLines)
            if (($new -join "`n") -match $Pattern) {
                return $true
            }
        }
        Start-Sleep -Milliseconds 200
    }
    return $false
}

try {
    $server = Start-MockServer -Exe (Join-Path $BuildDir 'mock_keyboard.exe') `
        -Port $Port -BusId $BusId -LogDir $logDir -ExtraArgs @('--no-demo')
    $portNum = Attach-Device -Port $Port -BusId $BusId
    $null = Wait-PnpDevice -InstancePattern '*VID_1234&PID_5679*' -Class 'Keyboard'
    Write-Host '[PASS] 键盘枚举成功'

    if (-not $TestInput) {
        Write-Host '  [SKIP] 未传 -TestInput：不切换 Caps Lock（避免影响宿主机输入）'
    }
    else {
        # Windows 的 LED 报告里 Num Lock（0x01）通常常亮，Caps Lock 是 bit1（0x02）；
        # 期望值按当前 Num Lock 状态动态计算，不能硬编码 0x02/0x00
        $numLockBit = if ([Console]::NumberLock) { 0x01 } else { 0x00 }
        $capsOn = $numLockBit -bor 0x02
        $capsOff = $numLockBit

        # 先把 Caps Lock 归零，确保第一次切换是"点亮"
        if ([Console]::CapsLock) {
            $baseline = Get-LogLineCount $server.StdOut
            Toggle-CapsLock
            $offPattern = 'LED status changed: 0x' + ('{0:x2}' -f $capsOff)
            if (-not (Wait-NewLogPattern -Path $server.StdOut -Pattern $offPattern -BaselineLines $baseline)) {
                throw "Caps Lock 归零后服务器没有收到 $offPattern"
            }
        }

        $baseline = Get-LogLineCount $server.StdOut
        Toggle-CapsLock
        $onPattern = 'LED status changed: 0x' + ('{0:x2}' -f $capsOn)
        if (-not (Wait-NewLogPattern -Path $server.StdOut -Pattern $onPattern -BaselineLines $baseline)) {
            throw "服务器没有收到 Caps Lock 点亮的 SET_REPORT（期望 $onPattern）"
        }
        Write-Host "[PASS] 服务器收到 LED 点亮状态（$onPattern）"

        $baseline = Get-LogLineCount $server.StdOut
        Toggle-CapsLock
        $offPattern = 'LED status changed: 0x' + ('{0:x2}' -f $capsOff)
        if (-not (Wait-NewLogPattern -Path $server.StdOut -Pattern $offPattern -BaselineLines $baseline)) {
            throw "服务器没有收到 Caps Lock 熄灭的 SET_REPORT（期望 $offPattern）"
        }
        Write-Host "[PASS] 服务器收到 LED 熄灭状态（$offPattern）"
    }
}
finally {
    if ([Console]::CapsLock -ne $initialCaps) {
        Toggle-CapsLock
    }
    if ($null -ne $portNum) {
        try { Detach-Device -PortNum $portNum }
        catch { Write-Warning "detach 失败：$_" }
    }
    if ($null -ne $server) {
        try { Stop-ServerSafely -Server $server -BusId $BusId }
        catch { Write-Warning "停止服务器失败：$_" }
    }
}
