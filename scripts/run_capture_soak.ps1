#Requires -Version 5.1
[CmdletBinding()]
param(
    [int]$Frames = 20000,
    [int]$SampleSec = 15,
    [string]$Config = 'Release',
    [string]$PluginSet = 'plugins_real',
    [int]$TimeoutSec = 1800
)

$ErrorActionPreference = 'Stop'
$OutputEncoding = [System.Text.UTF8Encoding]::new($false)
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $scriptDir) { $scriptDir = $PSScriptRoot }
$scriptDir = (Resolve-Path $scriptDir).Path
$projectRoot = Split-Path -Parent $scriptDir

$coreExe = Join-Path $projectRoot "build\$Config\core.exe"
$pluginDir = Join-Path $projectRoot "build\$Config\$PluginSet"
$modelPath = 'D:\dev\module-host\models\yolo11s.onnx'

function Fail($msg) {
    Write-Output "FAIL: $msg"
    Write-Output 'SUMMARY soak=FAIL'
    exit 1
}

if (-not (Test-Path -LiteralPath $coreExe -PathType Leaf)) { Fail "core.exe not found: $coreExe" }
if (-not (Test-Path -LiteralPath $pluginDir -PathType Container)) { Fail "plugin dir not found: $pluginDir" }

$timestamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$reportDir = Join-Path (Join-Path $projectRoot "build\$Config") 'soak'
if (-not (Test-Path -LiteralPath $reportDir)) { New-Item -ItemType Directory -Path $reportDir -Force | Out-Null }

$configFile = Join-Path $reportDir "long_run_$timestamp.ini"
$stdoutFile = Join-Path $reportDir "stdout_$timestamp.txt"
$stderrFile = Join-Path $reportDir "stderr_$timestamp.txt"
$csvFile = Join-Path $reportDir "memory_$timestamp.csv"
$reportFile = Join-Path $reportDir "report_$timestamp.txt"

$configLines = @(
    "plugins_dir = $pluginDir",
    "frames = $Frames",
    "capture_device = 0",
    "capture_width = 1920",
    "capture_height = 1080",
    "capture_fps = 30",
    "capture_format = auto",
    "input_port = none"
)
[IO.File]::WriteAllLines($configFile, $configLines, (New-Object System.Text.UTF8Encoding($false)))

Write-Output ('[soak] 配置: ' + $configFile)
Write-Output ('[soak] 预计: ' + [math]::Round($Frames / 30.0 / 60.0, 1) + ' 分钟')
Write-Output '[soak] 启动 core.exe ...'

$coreArgs = "--config `"$configFile`""
$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $coreExe
$psi.Arguments = $coreArgs
$psi.WorkingDirectory = $projectRoot
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.CreateNoWindow = $true
$psi.StandardOutputEncoding = [System.Text.UTF8Encoding]::new($false)
$psi.StandardErrorEncoding = [System.Text.UTF8Encoding]::new($false)

$proc = New-Object System.Diagnostics.Process
$proc.StartInfo = $psi
[void]$proc.Start()

$stdoutTask = $proc.StandardOutput.ReadToEndAsync()
$stderrTask = $proc.StandardError.ReadToEndAsync()

Add-Content -LiteralPath $csvFile -Value 'timestamp_iso,elapsed_sec,private_bytes,working_set_bytes'
$startTime = Get-Date
$samples = 0
$memStart = 0
$memEnd = 0
$memPeak = 0
$memSamples = @()

while (-not $proc.HasExited) {
    Start-Sleep -Seconds $SampleSec
    $elapsed = (Get-Date) - $startTime
    if ($elapsed.TotalSeconds -gt $TimeoutSec) {
        Write-Output ('[soak] 超时 ' + $TimeoutSec + ' 秒，终止')
        try { $proc.Kill($true) } catch { try { $proc.Kill() } catch {} }
        break
    }
    try {
        $proc.Refresh()
        if ($proc.HasExited) { break }
        $mem = $proc.PrivateMemorySize64
        $ws = $proc.WorkingSet64
        if ($samples -eq 0) { $memStart = $mem }
        $memEnd = $mem
        if ($mem -gt $memPeak) { $memPeak = $mem }
        $memSamples += $mem
        $line = ('{0},{1},{2},{3}' -f (Get-Date -Format o), [int]$elapsed.TotalSeconds, $mem, $ws)
        Add-Content -LiteralPath $csvFile -Value $line
        $samples++
        Write-Output ('[soak] t=' + [int]$elapsed.TotalSeconds + 's mem=' + [math]::Round($mem / 1MB, 1) + 'MB ws=' + [math]::Round($ws / 1MB, 1) + 'MB')
    } catch {}
}

$endTime = Get-Date
$durationSec = ($endTime - $startTime).TotalSeconds
if (-not $proc.HasExited) {
    try { $proc.Kill($true) } catch { try { $proc.Kill() } catch {} }
}
try { $proc.WaitForExit(10000) | Out-Null } catch {}
$exitCode = if ($proc.HasExited) { $proc.ExitCode } else { -2 }

$stdout = $stdoutTask.GetAwaiter().GetResult()
$stderr = $stderrTask.GetAwaiter().GetResult()
[IO.File]::WriteAllText($stdoutFile, $stdout, (New-Object System.Text.UTF8Encoding($false)))
[IO.File]::WriteAllText($stderrFile, $stderr, (New-Object System.Text.UTF8Encoding($false)))
$combined = "$stdout`n$stderr"

Write-Output ''
Write-Output ('[soak] 运行结束，exit=' + $exitCode + '，耗时 ' + [math]::Round($durationSec, 1) + 's')

$hasComplete = $combined -match '\[内核\]\s*\d+\s*帧完成'
$errorLines = @()
foreach ($ln in ($combined -split "`r?`n")) {
    if ($ln -match '^\[错误\]') { $errorLines += $ln }
}

$frameCount = 0  # 不再从 jsonl 数帧；使用 [内核] N 帧完成 关键字验证

$memStartMB = [math]::Round($memStart / 1MB, 1)
$memEndMB = [math]::Round($memEnd / 1MB, 1)
$memPeakMB = [math]::Round($memPeak / 1MB, 1)
$growthMB = [math]::Round(($memEnd - $memStart) / 1MB, 1)

$report = @()
$report += '=== capture soak report ==='
$report += ('timestamp: ' + $timestamp)
$report += ('duration: ' + [math]::Round($durationSec, 1) + 's')
$report += ('exit_code: ' + $exitCode)
$report += ('frames_requested: ' + $Frames)
$report += ('frames_recorded: ' + $frameCount)
$report += ('has_complete_keyword: ' + $hasComplete)
$report += ('error_lines: ' + $errorLines.Count)
if ($errorLines.Count -gt 0) {
    foreach ($e in $errorLines) { $report += ('  ' + $e) }
}
$report += ''
$report += ('mem_start_mb: ' + $memStartMB)
$report += ('mem_end_mb: ' + $memEndMB)
$report += ('mem_peak_mb: ' + $memPeakMB)
$report += ('mem_growth_mb: ' + $growthMB)
$report += ('samples: ' + $samples)
$report += ''
$report += ('stdout: ' + $stdoutFile)
$report += ('stderr: ' + $stderrFile)
$report += ('csv: ' + $csvFile)

[IO.File]::WriteAllLines($reportFile, $report, (New-Object System.Text.UTF8Encoding($false)))
foreach ($l in $report) { Write-Output $l }

$ok = ($exitCode -eq 0) -and $hasComplete -and ($errorLines.Count -eq 0)
if ($ok) {
    Write-Output 'SUMMARY soak=PASS'
    exit 0
} else {
    Write-Output 'SUMMARY soak=FAIL'
    exit 1
}
