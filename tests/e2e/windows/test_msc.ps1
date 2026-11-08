param(
    [int]$Port = 53240,
    [string]$BusId = '1-1',
    [string]$BuildDir = ''
)

# MSC：Windows 枚举成 USB 磁盘后校验容量与序列号。
# 本脚本只读：不写盘、不挂载文件系统，避免误伤物理磁盘。

. "$PSScriptRoot/common.ps1"

Assert-Admin
$BuildDir = Resolve-BuildDir $BuildDir
$logDir = New-E2eLogDir 'msc'
$server = $null
$portNum = $null

try {
    $server = Start-MockServer -Exe (Join-Path $BuildDir 'mock_msc.exe') `
        -Port $Port -BusId $BusId -LogDir $logDir
    $portNum = Attach-Device -Port $Port -BusId $BusId

    # DiskDrive 节点的 InstanceId 是 USBSTOR\DISK&VEN_USBIPDCP&PROD_...，
    # 不含 USB 层的 VID/PID，所以按厂商串匹配
    $null = Wait-PnpDevice -InstancePattern '*VEN_USBIPDCP*' -Class 'DiskDrive'

    $disk = $null
    $deadline = (Get-Date).AddSeconds(20)
    while ((Get-Date) -lt $deadline -and $null -eq $disk) {
        $disk = Get-Disk | Where-Object { $_.BusType -eq 'USB' -and $_.SerialNumber -like 'SBIPDCPSN*' } |
            Select-Object -First 1
        if ($null -eq $disk) { Start-Sleep -Milliseconds 500 }
    }
    if ($null -eq $disk) {
        throw '未找到 mock_msc 的 USB 磁盘'
    }
    if ($disk.Size -ne 2097152) {
        throw "磁盘容量不符：期望 2097152，实际 $($disk.Size)"
    }
    Write-Host "[PASS] MSC 枚举为磁盘 $($disk.Number)（$($disk.FriendlyName)，$($disk.Size) 字节）"
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
