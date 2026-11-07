param(
    [int]$Port = 53240,
    [string]$BusId = '1-1',
    [string]$BuildDir = ''
)

# CDC ACM：Windows 枚举成 COM 口后做真实串口回显读写。
# 覆盖 uusb 严格枚举 + usbser.sys 数据面。

. "$PSScriptRoot/common.ps1"

Assert-Admin
$BuildDir = Resolve-BuildDir $BuildDir
$logDir = New-E2eLogDir 'cdc_acm'
$server = $null
$portNum = $null

try {
    $server = Start-MockServer -Exe (Join-Path $BuildDir 'mock_cdc_acm.exe') `
        -Port $Port -BusId $BusId -LogDir $logDir
    $portNum = Attach-Device -Port $Port -BusId $BusId

    $null = Wait-PnpDevice -InstancePattern '*VID_1234&PID_5680*' -Class 'Ports'
    $com = Get-CimInstance Win32_SerialPort |
        Where-Object { $_.PNPDeviceID -like '*VID_1234&PID_5680*' } |
        Select-Object -First 1
    if (-not $com) {
        throw 'PnP 出现 Ports 设备，但 Win32_SerialPort 没有对应 COM 口'
    }

    $serial = New-Object System.IO.Ports.SerialPort(
        $com.DeviceID, 115200, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
    $serial.ReadTimeout = 3000
    $serial.Open()
    try {
        $serial.DiscardInBuffer()
        $serial.Write('hello-e2e')
        Start-Sleep -Milliseconds 800
        $read = $serial.ReadExisting()
    }
    finally {
        $serial.Close()
    }

    if ($read -ne 'hello-e2e') {
        throw "串口回显不匹配：期望 hello-e2e，实际 [$read]"
    }
    Write-Host "[PASS] CDC ACM 枚举为 $($com.DeviceID)（$($com.Name)），回显正确"
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
