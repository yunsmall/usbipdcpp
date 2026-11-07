param(
    [int]$Port = 53240,
    [string]$BusId = '1-1',
    [string]$BuildDir = ''
)

# HID：键盘/鼠标/游戏手柄在 Windows 上枚举成对应的 HID 设备。
# 只用 --no-demo 模式验证枚举：keyboard 会自动敲 A 键、mouse 会移动光标，
# 自动化测试里绝不能向宿主机注入输入。

. "$PSScriptRoot/common.ps1"

Assert-Admin
$BuildDir = Resolve-BuildDir $BuildDir
$logDir = New-E2eLogDir 'hid'

# 设备表：VID/PID 与 examples 里 UsbDevice::make 保持一致
$devices = @(
    [pscustomobject]@{
        Name    = 'keyboard'
        Exe     = 'mock_keyboard.exe'
        Pattern = '*VID_1234&PID_5679*'
        Class   = 'Keyboard'
    },
    [pscustomobject]@{
        Name    = 'mouse'
        Exe     = 'mock_mouse.exe'
        Pattern = '*VID_1234&PID_5678*'
        Class   = 'Mouse'
    },
    [pscustomobject]@{
        Name    = 'gamepad'
        Exe     = 'mock_gamepad.exe'
        # 用 HID 子节点而不是 USB 层节点，确认 HID 报告描述符被系统接受
        Pattern = 'HID\VID_1234&PID_5680*'
        Class   = 'HIDClass'
    }
)

foreach ($dev in $devices) {
    $server = $null
    $portNum = $null
    try {
        $server = Start-MockServer -Exe (Join-Path $BuildDir $dev.Exe) `
            -Port $Port -BusId $BusId -LogDir $logDir -ExtraArgs @('--no-demo')
        $portNum = Attach-Device -Port $Port -BusId $BusId

        # 等到出现对应类别的子设备（USB 层节点是 HIDClass，键盘/鼠标是子节点）
        $deadline = (Get-Date).AddSeconds(20)
        $node = $null
        while ((Get-Date) -lt $deadline -and $null -eq $node) {
            $node = Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
                Where-Object {
                    $_.InstanceId -like $dev.Pattern -and
                    $_.Class -eq $dev.Class -and
                    $_.Status -eq 'OK'
                } |
                Select-Object -First 1
            if ($null -eq $node) { Start-Sleep -Milliseconds 500 }
        }
        if ($null -eq $node) {
            throw "$($dev.Name) 未枚举出 Class=$($dev.Class) 的 HID 设备"
        }
        Write-Host "[PASS] HID $($dev.Name)：$($node.InstanceId)（$($node.FriendlyName)）"
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
}
