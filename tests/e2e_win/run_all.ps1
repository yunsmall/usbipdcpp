param(
    [int]$Port = 53240,
    [string]$BusId = '1-1',
    [string]$BuildDir = ''
)

# Windows e2e 总入口：逐个运行 test_*.ps1，失败不中断，最后汇总。
# 每个子脚本自己负责 attach → 验证 → detach → 停服务器。

$failed = @()

Get-ChildItem -Path $PSScriptRoot -Filter 'test_*.ps1' | Sort-Object Name | ForEach-Object {
    $name = $_.BaseName
    Write-Host ''
    Write-Host "==================== $name ===================="

    # 子进程隔离：一个测试的异常/变量不会污染下一个
    # 空 BuildDir 不能作为参数传给子脚本（powershell -File 会当成缺少参数值）
    $childArgs = @('-NoProfile', '-File', $_.FullName, '-Port', $Port, '-BusId', $BusId)
    if ($BuildDir) {
        $childArgs += @('-BuildDir', $BuildDir)
    }
    & powershell @childArgs
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[$name] FAILED"
        $failed += $name
    }
    else {
        Write-Host "[$name] OK"
    }
}

Write-Host ''
Write-Host '=============================================='
if ($failed.Count -gt 0) {
    Write-Host "FAILED: $($failed -join ' ')"
    exit 1
}
Write-Host 'All Windows e2e tests passed'
exit 0
