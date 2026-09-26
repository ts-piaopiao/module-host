#Requires -Version 5.1
[CmdletBinding()]
param(
    [int]$Frames = 3000,
    [string]$Config = 'Release'
)

$ErrorActionPreference = 'Stop'
$OutputEncoding = [System.Text.UTF8Encoding]::new($false)
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $scriptDir) { $scriptDir = $PSScriptRoot }
$projectRoot = Split-Path -Parent (Resolve-Path $scriptDir).Path

$coreExe = Join-Path $projectRoot "build\$Config\core.exe"
$analyzerExe = Join-Path $projectRoot "experiments\send_log_analyzer\build\$Config\send_log_analyzer.exe"
$fakeSceneDir = Join-Path $projectRoot "build\$Config\acceptance\fake_scene"
$fakeAttackDir = Join-Path $projectRoot "build\$Config\acceptance\fake_scene_attack"

$minHoldMs = 50
$minGapMs = 30
$holdToleranceMs = 1.0

function Abort {
    param([string]$Msg)
    Write-Output "FAIL: $Msg"
    Write-Output 'SUMMARY total=2 passed=0 failed=2'
    exit 1
}

if (-not (Test-Path -LiteralPath $coreExe)) { Abort "core.exe not found: $coreExe" }
if (-not (Test-Path -LiteralPath $analyzerExe)) { Abort "send_log_analyzer.exe not found: $analyzerExe" }
if (-not (Test-Path -LiteralPath $fakeSceneDir)) { Abort "fake_scene dir not found: $fakeSceneDir (run scripts\prepare_fake_scene_dir.ps1)" }
if (-not (Test-Path -LiteralPath $fakeAttackDir)) { Abort "fake_scene_attack dir not found: $fakeAttackDir (run scripts\prepare_fake_scene_attack_dir.ps1)" }

$tmpRoot = Join-Path $env:TEMP ("jitter_test_" + [IO.Path]::GetRandomFileName())
New-Item -ItemType Directory -Path $tmpRoot -Force | Out-Null

function Invoke-Core {
    param([string]$PluginDir, [string]$Tag, [string[]]$ExtraLines)
    $ini = Join-Path $tmpRoot "$Tag.ini"
    $jsonl = Join-Path $tmpRoot "$Tag.jsonl"
    $lines = @("plugins_dir = $PluginDir", "frames = $Frames", "input_port = none")
    if ($ExtraLines) { $lines += $ExtraLines }
    [IO.File]::WriteAllLines($ini, $lines, (New-Object System.Text.UTF8Encoding($false)))

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $coreExe
    $psi.Arguments = "--config `"$ini`" --record `"$jsonl`""
    $psi.WorkingDirectory = $projectRoot
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $psi
    [void]$proc.Start()
    $soTask = $proc.StandardOutput.ReadToEndAsync()
    $seTask = $proc.StandardError.ReadToEndAsync()
    [void]$proc.WaitForExit(600000)
    if (-not $proc.HasExited) {
        try { $proc.Kill($true) } catch { try { $proc.Kill() } catch {} }
        [void]$proc.WaitForExit(5000)
    }
    $so = $soTask.GetAwaiter().GetResult()
    $se = $seTask.GetAwaiter().GetResult()
    [IO.File]::WriteAllText((Join-Path $tmpRoot "$Tag.out.txt"), ($so + $se), (New-Object System.Text.UTF8Encoding($false)))

    $ev = Get-ChildItem -Path $jsonl -Recurse -Filter 'events.jsonl' -ErrorAction SilentlyContinue | Select-Object -First 1
    return @{
        ExitCode   = $proc.ExitCode
        EventsFile = if ($ev) { $ev.FullName } else { $null }
    }
}

# === Test 1: jitter uniformity ===
Write-Output '[test 1/2] fake_scene jitter uniformity ...'
$r1 = Invoke-Core -PluginDir $fakeSceneDir -Tag 'jitter' -ExtraLines $null
$jitterPass = $false
$jitterDetail = ''

if ($r1.ExitCode -ne 0 -or -not $r1.EventsFile) {
    $jitterDetail = "core exit=$($r1.ExitCode) eventsfile=$($r1.EventsFile)"
} else {
    $out = & $analyzerExe --input $r1.EventsFile 2>&1 | Out-String
    $chi2 = @{}
    foreach ($m in @(5, 7, 15)) {
        if ($out -match "mod-$m uniformity: chi2=([\d.]+)") {
            $chi2[$m] = [double]$Matches[1]
        }
    }
    $bad = @()
    foreach ($m in @(5, 7, 15)) {
        $thr = 3.0 * ($m - 1)
        if (-not $chi2.ContainsKey($m)) {
            $bad += "mod$m missing"
        } elseif ($chi2[$m] -gt $thr) {
            $bad += "mod$m chi2=$($chi2[$m]) > $thr"
        }
    }
    if ($bad.Count -eq 0) {
        $jitterPass = $true
        $jitterDetail = "mod5=$($chi2[5]) mod7=$($chi2[7]) mod15=$($chi2[15])"
    } else {
        $jitterDetail = ($bad -join '; ')
    }
}

if ($jitterPass) {
    Write-Output "PASS fake_scene_jitter ($jitterDetail)"
} else {
    Write-Output "FAIL fake_scene_jitter reason=$jitterDetail"
}

# === Test 2: min-hold constraint ===
Write-Output '[test 2/2] fake_scene_attack min-hold ...'
$extra = @(
    "output_min_hold_ms = $minHoldMs",
    "output_min_gap_ms = $minGapMs",
    'combat_e_common_min_ms = 20',
    'combat_e_common_max_ms = 20',
    'combat_e_common_prob = 100'
)
$r2 = Invoke-Core -PluginDir $fakeAttackDir -Tag 'attack' -ExtraLines $extra
$holdPass = $false
$holdDetail = ''

if ($r2.ExitCode -ne 0 -or -not $r2.EventsFile) {
    $holdDetail = "core exit=$($r2.ExitCode) eventsfile=$($r2.EventsFile)"
} else {
    $eLines = Get-Content -LiteralPath $r2.EventsFile -Encoding UTF8 |
              Where-Object { $_ -match '"type":"snd"' -and $_ -match '"a":69' }
    $prevPressUs = -1
    $pairCount = 0
    $badPairs = 0
    $minPair = [double]::MaxValue
    foreach ($line in $eLines) {
        $o = $line | ConvertFrom-Json
        if ($o.b -eq 1) {
            $prevPressUs = $o.t
        } elseif ($o.b -eq 0 -and $prevPressUs -gt 0) {
            $dtMs = ($o.t - $prevPressUs) / 1000.0
            $pairCount++
            if ($dtMs -lt $minPair) { $minPair = $dtMs }
            if ($dtMs -lt ($minHoldMs - $holdToleranceMs)) { $badPairs++ }
            $prevPressUs = -1
        }
    }
    if ($pairCount -lt 10) {
        $holdDetail = "too few E-pairs: $pairCount"
    } elseif ($badPairs -eq 0) {
        $holdPass = $true
        $holdDetail = "E-pairs=$pairCount min=$([math]::Round($minPair,2))ms"
    } else {
        $holdDetail = "E-pairs=$pairCount bad=$badPairs min=$([math]::Round($minPair,2))ms"
    }
}

if ($holdPass) {
    Write-Output "PASS fake_scene_attack_minhold ($holdDetail)"
} else {
    Write-Output "FAIL fake_scene_attack_minhold reason=$holdDetail"
}

$passed = 0
$failed = 0
if ($jitterPass) { $passed++ } else { $failed++ }
if ($holdPass) { $passed++ } else { $failed++ }

Write-Output ("SUMMARY total=2 passed=$passed failed=$failed")
if ($failed -gt 0) { exit 1 }
exit 0
