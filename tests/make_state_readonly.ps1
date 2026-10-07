#!/usr/bin/env pwsh
# 16-1 落盘失败验证辅助脚本（不是文件样本，是运行期权限条件）
#
# 用法：
#   pwsh make_state_readonly.ps1 -BinDir "F:\programs\Lilith Reader\bin\Release" -Mode readonly
#   pwsh make_state_readonly.ps1 -BinDir "..." -Mode writable
#
# 步骤：
#   1) 用 -Mode readonly 把 reader_state.bin 设为只读（不存在则先建一个空文件再设只读）
#   2) 启动 LilithReader.exe，翻页 / 加书签触发落盘
#   3) 应看到状态栏告警色「阅读数据保存失败」+ 首次一次 toast，日志出现 persist 写失败记录
#   4) 用 -Mode writable 恢复可写，再触发一次保存，告警应消失
param(
    [string]$BinDir = "bin\Release",
    [ValidateSet("readonly", "writable")][string]$Mode = "readonly"
)

$stateFile = Join-Path $BinDir "reader_state.bin"
if (-not (Test-Path $stateFile)) {
    New-Item -ItemType File -Path $stateFile -Force | Out-Null
    Write-Host "已创建空状态文件: $stateFile"
}

if ($Mode -eq "readonly") {
    Set-ItemProperty -Path $stateFile -Name IsReadOnly -Value $true
    Write-Host "已设为只读 -> 现在启动程序并翻页/加书签触发落盘，应出现『阅读数据保存失败』告警"
} else {
    Set-ItemProperty -Path $stateFile -Name IsReadOnly -Value $false
    Write-Host "已恢复可写 -> 再触发一次保存，告警应消失"
}
