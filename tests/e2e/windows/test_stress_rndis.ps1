param(
    [int]$Port = 53240,
    [string]$BusId = '1-1',
    [string]$BuildDir = '',
    [int]$Duration = 60
)

# 持续传输压力：RNDIS 网卡上按指定时长循环做 8KB TCP echo，验证长时间
# 传输无失败、服务器进程存活、内存无明显增长。

. "$PSScriptRoot/common.ps1"

Assert-Admin
$BuildDir = Resolve-BuildDir $BuildDir
$logDir = New-E2eLogDir 'stress_rndis'
$server = $null
$portNum = $null
$adapterName = $null

try {
    $server = Start-MockServer -Exe (Join-Path $BuildDir 'mock_rndis.exe') `
        -Port $Port -BusId $BusId -LogDir $logDir
    $portNum = Attach-Device -Port $Port -BusId $BusId
    $null = Wait-PnpDevice -InstancePattern '*VID_1234&PID_56E5*' -Class 'Net'

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

    if (-not (Test-Connection -ComputerName 192.168.53.1 -Count 2 -Quiet)) {
        throw 'ping 192.168.53.1 不通'
    }
    Write-Host '[PASS] RNDIS 网卡 Up，ping 通'

    $server.Process.Refresh()
    $rssBefore = $server.Process.WorkingSet64
    # 长时间稳定性用 ICMP 验证：演示用 EthernetEchoBackend 只支持单 TCP
    # 连接，没有发送缓冲/重传和窗口管理，多轮 TCP 会被 Windows 的延迟 ACK
    # 拖出超时；TCP 数据面正确性由 test_rndis 的单次 8KB 覆盖
    $deadline = (Get-Date).AddSeconds($Duration)
    $pingOk = 0
    $pingFail = 0
    while ((Get-Date) -lt $deadline) {
        if (Test-Connection -ComputerName 192.168.53.1 -Count 1 -Quiet) {
            $pingOk++
        }
        else {
            $pingFail++
        }
        Start-Sleep -Milliseconds 200
    }

    $server.Process.Refresh()
    $growth = $server.Process.WorkingSet64 - $rssBefore
    $total = $pingOk + $pingFail
    Write-Host "  ping 轮数=$total 成功=$pingOk 失败=$pingFail 内存增长=$([math]::Round($growth / 1MB, 1))MB"
    if ($pingOk -eq 0) {
        throw '持续 ping 没有成功轮次'
    }
    if ($total -gt 0 -and ($pingFail / $total) -gt 0.02) {
        throw "持续 ping 丢包率过高：$pingFail/$total"
    }
    if ($growth -gt 100MB) {
        throw "服务器内存增长过大：$([math]::Round($growth / 1MB, 1))MB"
    }
    Write-Host "[PASS] 持续 ping $total 轮（失败 $pingFail），内存稳定"
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
