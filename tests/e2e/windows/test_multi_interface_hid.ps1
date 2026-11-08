param(
    [int]$Port = 53240,
    [string]$BusId = '1-1',
    [string]$BuildDir = ''
)

# 复合 HID：一个配置里同时有相对鼠标（接口 0）和键盘（接口 1），
# Windows 应同时枚举出 Keyboard 与 Mouse 两个子设备（接口路由正确）。

. "$PSScriptRoot/common.ps1"

Assert-Admin
$BuildDir = Resolve-BuildDir $BuildDir
$logDir = New-E2eLogDir 'multi_interface_hid'
$server = $null
$portNum = $null

try {
    $server = Start-MockServer -Exe (Join-Path $BuildDir 'multi_interface_hid.exe') `
        -Port $Port -BusId $BusId -LogDir $logDir -ExtraArgs @('--no-demo')
    $portNum = Attach-Device -Port $Port -BusId $BusId

    $deadline = (Get-Date).AddSeconds(20)
    $keyboard = $null
    $mouse = $null
    while ((Get-Date) -lt $deadline -and ($null -eq $keyboard -or $null -eq $mouse)) {
        $devices = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
            Where-Object { $_.InstanceId -like '*VID_1234&PID_5679*' -and $_.Status -eq 'OK' }
        $keyboard = $devices | Where-Object { $_.Class -eq 'Keyboard' } | Select-Object -First 1
        $mouse = $devices | Where-Object { $_.Class -eq 'Mouse' } | Select-Object -First 1
        if ($null -eq $keyboard -or $null -eq $mouse) {
            Start-Sleep -Milliseconds 500
        }
    }
    if ($null -eq $keyboard) {
        throw '复合设备没有枚举出 Keyboard 接口'
    }
    if ($null -eq $mouse) {
        throw '复合设备没有枚举出 Mouse 接口'
    }
    Write-Host "[PASS] 键盘接口：$($keyboard.InstanceId)"
    Write-Host "[PASS] 鼠标接口：$($mouse.InstanceId)"
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
