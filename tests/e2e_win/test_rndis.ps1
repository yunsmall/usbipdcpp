param(
    [int]$Port = 53240,
    [string]$BusId = '1-1',
    [string]$BuildDir = ''
)

# RNDIS：Windows 内置 usb8023.sys，枚举成 Remote NDIS 网卡后配 IP，
# 验证 ping 设备侧 + TCP echo 数据面。
# 注意：Windows 没有 CDC-ECM 驱动，ECM 设备在 Windows 会报错，不在此测试。

. "$PSScriptRoot/common.ps1"

Assert-Admin
$BuildDir = Resolve-BuildDir $BuildDir
$logDir = New-E2eLogDir 'rndis'
$server = $null
$portNum = $null
$adapterName = $null

try {
    $server = Start-MockServer -Exe (Join-Path $BuildDir 'mock_rndis.exe') `
        -Port $Port -BusId $BusId -LogDir $logDir
    $portNum = Attach-Device -Port $Port -BusId $BusId

    # PID 0x56E5 与 examples/mock_rndis 一致（Windows 会按 VID/PID 缓存 usbser 绑定）
    $null = Wait-PnpDevice -InstancePattern '*VID_1234&PID_56E5*' -Class 'Net'

    # 等网络适配器真正 Up（PnP 出现后 WDF 还需绑定网卡栈）
    $deadline = (Get-Date).AddSeconds(20)
    while ((Get-Date) -lt $deadline -and $null -eq $adapterName) {
        $adapter = Get-NetAdapter -ErrorAction SilentlyContinue |
            Where-Object { $_.InterfaceDescription -like '*Remote NDIS*' -and $_.Status -eq 'Up' } |
            Select-Object -First 1
        if ($null -ne $adapter) {
            $adapterName = $adapter.Name
        }
        else {
            Start-Sleep -Milliseconds 500
        }
    }
    if ($null -eq $adapterName) {
        throw 'Remote NDIS 网卡未进入 Up 状态'
    }

    Remove-NetIPAddress -InterfaceAlias $adapterName -IPAddress 192.168.53.2 `
        -Confirm:$false -ErrorAction SilentlyContinue
    New-NetIPAddress -InterfaceAlias $adapterName -IPAddress 192.168.53.2 -PrefixLength 24 | Out-Null
    Start-Sleep -Seconds 1

    if (-not (Test-Connection -ComputerName 192.168.53.1 -Count 3 -Quiet)) {
        throw 'ping 192.168.53.1 不通'
    }

    # TCP echo：mock 的 echo 后端按段原样回显
    $tcp = New-Object Net.Sockets.TcpClient
    try {
        $tcp.Connect('192.168.53.1', 5000)
        $stream = $tcp.GetStream()
        $payload = [Text.Encoding]::ASCII.GetBytes('hello')
        $stream.Write($payload, 0, $payload.Length)
        $stream.Flush()
        Start-Sleep -Milliseconds 500
        $buffer = New-Object byte[] 5
        $read = $stream.Read($buffer, 0, 5)
        $reply = [Text.Encoding]::ASCII.GetString($buffer, 0, $read)
    }
    finally {
        $tcp.Close()
    }
    if ($reply -ne 'hello') {
        throw "TCP echo 不匹配：期望 hello，实际 [$reply]"
    }
    Write-Host "[PASS] RNDIS 网卡 $adapterName ping 通，TCP echo 正确"
}
finally {
    if ($null -ne $adapterName) {
        Remove-NetIPAddress -InterfaceAlias $adapterName -IPAddress 192.168.53.2 `
            -Confirm:$false -ErrorAction SilentlyContinue
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
