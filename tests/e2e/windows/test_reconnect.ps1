param(
    [int]$Port = 53240,
    [string]$BusId = '1-1',
    [string]$BuildDir = ''
)

# 重连：同一设备 detach 后重新 attach、服务器停止重启后重新 attach，
# 每次都用串口回显验证数据面。Windows 重新枚举设备比 Linux 慢，全部用轮询。

. "$PSScriptRoot/common.ps1"

Assert-Admin
$BuildDir = Resolve-BuildDir $BuildDir
$logDir = New-E2eLogDir 'reconnect'
$server = $null
$portNum = $null

function Get-ComPort {
    $deadline = (Get-Date).AddSeconds(20)
    while ((Get-Date) -lt $deadline) {
        $com = Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue |
            Where-Object { $_.PNPDeviceID -like '*VID_1234&PID_5680*' } |
            Select-Object -First 1
        if ($com) {
            return $com.DeviceID
        }
        Start-Sleep -Milliseconds 500
    }
    throw '未找到 mock_cdc_acm 对应的 COM 口'
}

function Invoke-SerialEcho {
    param([string]$ComName, [string]$Message)
    $serial = New-Object System.IO.Ports.SerialPort(
        $ComName, 115200, [System.IO.Ports.Parity]::None, 8, [System.IO.Ports.StopBits]::One)
    $serial.ReadTimeout = 3000
    $serial.Open()
    try {
        $serial.DiscardInBuffer()
        $serial.Write($Message)
        Start-Sleep -Milliseconds 800
        return $serial.ReadExisting()
    }
    finally {
        $serial.Close()
    }
}

try {
    $server = Start-MockServer -Exe (Join-Path $BuildDir 'mock_cdc_acm.exe') `
        -Port $Port -BusId $BusId -LogDir $logDir
    $portNum = Attach-Device -Port $Port -BusId $BusId
    $null = Wait-PnpDevice -InstancePattern '*VID_1234&PID_5680*' -Class 'Ports'
    $echo1 = Invoke-SerialEcho -ComName (Get-ComPort) -Message 'RECONNECT_1'
    if ($echo1 -ne 'RECONNECT_1') {
        throw "首次回显不匹配：[$echo1]"
    }
    Write-Host '[PASS] 首次 attach 回显正确'

    Detach-Device -PortNum $portNum
    $portNum = $null
    Write-Host '[PASS] detach 后端口消失'

    $portNum = Attach-Device -Port $Port -BusId $BusId
    $null = Wait-PnpDevice -InstancePattern '*VID_1234&PID_5680*' -Class 'Ports'
    $echo2 = Invoke-SerialEcho -ComName (Get-ComPort) -Message 'RECONNECT_2'
    if ($echo2 -ne 'RECONNECT_2') {
        throw "重新 attach 后回显不匹配：[$echo2]"
    }
    Write-Host '[PASS] 重新 attach 后回显正确'

    Detach-Device -PortNum $portNum
    $portNum = $null
    Stop-ServerSafely -Server $server -BusId $BusId
    $server = $null

    $server = Start-MockServer -Exe (Join-Path $BuildDir 'mock_cdc_acm.exe') `
        -Port $Port -BusId $BusId -LogDir $logDir
    $portNum = Attach-Device -Port $Port -BusId $BusId
    $null = Wait-PnpDevice -InstancePattern '*VID_1234&PID_5680*' -Class 'Ports'
    $echo3 = Invoke-SerialEcho -ComName (Get-ComPort) -Message 'RECONNECT_3'
    if ($echo3 -ne 'RECONNECT_3') {
        throw "服务器重启后回显不匹配：[$echo3]"
    }
    Write-Host '[PASS] 服务器重启后重新 attach 回显正确'
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
