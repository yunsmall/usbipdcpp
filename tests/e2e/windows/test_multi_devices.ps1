param(
    [int]$Port = 53240,
    [string]$BusId = '1-1',
    [string]$BuildDir = ''
)

# 多设备并发：一个服务器导出 3 台设备，三台同时 import，确认主机侧三组
# 设备节点同时存在；detach 一台后其余两台仍在。

. "$PSScriptRoot/common.ps1"

Assert-Admin
$BuildDir = Resolve-BuildDir $BuildDir
$logDir = New-E2eLogDir 'multi_devices'
$server = $null
$ports = @()

try {
    # multi_devices 的 busid 由设备工厂生成（1-1/1-2/1-3），-b 参数不参与
    $server = Start-MockServer -Exe (Join-Path $BuildDir 'multi_devices.exe') `
        -Port $Port -BusId $BusId -LogDir $logDir -ExtraArgs @('-n', '3')

    foreach ($b in @('1-1', '1-2', '1-3')) {
        $ports += Attach-Device -Port $Port -BusId $b
    }
    if ($ports.Count -ne 3) {
        throw "只导入了 $($ports.Count) 台设备"
    }
    Write-Host "[PASS] 三台设备均已导入（端口 $($ports -join ',')）"

    $deadline = (Get-Date).AddSeconds(20)
    $nodes = 0
    while ((Get-Date) -lt $deadline -and $nodes -lt 3) {
        $nodes = @(Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
                Where-Object {
                    $_.InstanceId -match 'VID_123[456]&PID_567[89A]' -and $_.Status -eq 'OK'
                }).Count
        if ($nodes -lt 3) {
            Start-Sleep -Milliseconds 500
        }
    }
    if ($nodes -lt 3) {
        throw "只枚举出 $nodes 组设备节点（期望 3）"
    }
    Write-Host "[PASS] 三组设备节点同时存在（$nodes 个）"

    $first = $ports[0]
    Detach-Device -PortNum $first
    $ports = $ports | Where-Object { $_ -ne $first }
    foreach ($p in $ports) {
        if (-not (Test-PortImported -PortNum $p)) {
            throw "detach 端口 $first 后端口 $p 也消失了"
        }
    }
    Write-Host '[PASS] detach 一台后其余设备仍在'
}
finally {
    foreach ($p in $ports) {
        try { Detach-Device -PortNum $p }
        catch { Write-Warning "detach 端口 $p 失败：$_" }
    }
    if ($null -ne $server) {
        try { Stop-ServerSafely -Server $server -BusId $BusId }
        catch { Write-Warning "停止服务器失败：$_" }
    }
}
