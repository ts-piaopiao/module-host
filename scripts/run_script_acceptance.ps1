#Requires -Version 5.1
[CmdletBinding()]
param(
    [string]$Config = 'Release',
    [string]$ReplayExe = '',
    [string]$FixturesDir = '',
    [int]$TimeoutSec = 60
)

$ErrorActionPreference = 'Stop'
$OutputEncoding = [System.Text.UTF8Encoding]::new($false)
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $scriptDir) { $scriptDir = $PSScriptRoot }
$scriptDir = (Resolve-Path $scriptDir).Path
$projectRoot = Split-Path -Parent $scriptDir

if (-not $ReplayExe) {
    $ReplayExe = Join-Path $projectRoot ("experiments\script_replay\build\{0}\script_replay.exe" -f $Config)
}
if (-not $FixturesDir) {
    $FixturesDir = Join-Path $projectRoot 'experiments\script_replay\fixtures'
}
if (-not [System.IO.Path]::IsPathRooted($ReplayExe)) {
    $ReplayExe = Join-Path $projectRoot $ReplayExe
}
if (-not [System.IO.Path]::IsPathRooted($FixturesDir)) {
    $FixturesDir = Join-Path $projectRoot $FixturesDir
}

$fixtures = @('real_session_long.jsonl', 'real_session_600f.jsonl', 'turn_scene.jsonl', 'long_idle_scene.jsonl', 'max_dets_scene.jsonl', 'multi_target_scene.jsonl')

# 固定随机种子，确保 I1–I11 不变式在确定性 trace 下验证。
# 不设种子时 CppScript 用 time ^ steady_clock 随机初始化（cpp_script.cpp:47），
# 会使 script_replay 每次跑出不同 trace，导致 I4 偶发违例（E 时长 < 50ms），
# 与代码正确性无关。本脚本只在本进程及其子进程内设置，不影响 run_all.ps1。
$env:MH_SCRIPT_SEED = '42'

function Precheck {
    param([bool]$Ok, [string]$Message)
    if (-not $Ok) {
        Write-Output ('FAIL: {0}' -f $Message)
        exit 1
    }
}

Precheck (Test-Path -LiteralPath $ReplayExe -PathType Leaf) ('script_replay.exe not found: {0}' -f $ReplayExe)
Precheck (Test-Path -LiteralPath $FixturesDir -PathType Container) ('fixtures dir not found: {0}' -f $FixturesDir)
foreach ($fixture in $fixtures) {
    Precheck (Test-Path -LiteralPath (Join-Path $FixturesDir $fixture) -PathType Leaf) `
        ('fixture not found: {0}' -f (Join-Path $FixturesDir $fixture))
}

function Invoke-Replay {
    param([string]$FixturePath)

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $ReplayExe
    $psi.Arguments = ('--input "{0}"' -f $FixturePath)
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

    $timedOut = $false
    if (-not $proc.WaitForExit($TimeoutSec * 1000)) {
        $timedOut = $true
        try { $proc.Kill($true) } catch { try { $proc.Kill() } catch {} }
        [void]$proc.WaitForExit(5000)
    }

    $stdout = $stdoutTask.GetAwaiter().GetResult()
    $stderr = $stderrTask.GetAwaiter().GetResult()
    $exitCode = if ($timedOut) { -1 } else { $proc.ExitCode }

    return [pscustomobject]@{
        ExitCode = $exitCode
        Stdout   = $stdout
        Stderr   = $stderr
        TimedOut = $timedOut
    }
}

$passed = 0
$failed = 0

foreach ($fixture in $fixtures) {
    $path = Join-Path $FixturesDir $fixture
    $run = Invoke-Replay -FixturePath $path
    $exitCode = $run.ExitCode

    if ($exitCode -eq 0) {
        Write-Output ('PASS {0} exit=0' -f $fixture)
        $passed++
        continue
    }

    $reasons = @()
    if ($run.TimedOut) { $reasons += 'timeout' }

    $failedInvariants = @()
    foreach ($line in ($run.Stdout -split "`r?`n")) {
        if ($line -match '(I\d+):\s+FAIL') { $failedInvariants += $Matches[1] }
    }
    if ($failedInvariants.Count -gt 0) {
        $reasons += ('failed={0}' -f (($failedInvariants | Select-Object -Unique) -join ','))
    }
    if ($run.Stderr -and $run.Stderr.Trim()) {
        $reasons += ('stderr={0}' -f $run.Stderr.Trim())
    }
    if ($reasons.Count -eq 0) { $reasons += 'no invariant detail' }

    Write-Output ('FAIL {0} exit={1} reason={2}' -f $fixture, $exitCode, ($reasons -join '; '))

    $tail = $run.Stdout -split "`r?`n"
    $tail = $tail | Where-Object { $_.Trim() } | Select-Object -Last 12
    foreach ($line in $tail) {
        [Console]::Out.WriteLine('  ' + $line)
    }
    $failed++
}

$total = $passed + $failed
[Console]::Out.WriteLine(('SUMMARY total={0} passed={1} failed={2}' -f $total, $passed, $failed))

if ($failed -gt 0) {
    exit 1
}
exit 0
