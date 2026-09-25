#Requires -Version 5.1
[CmdletBinding()]
param(
    [string]$Config = 'Release',
    [string]$ProbeExe = ''
)

$ErrorActionPreference = 'Stop'
$OutputEncoding = [System.Text.UTF8Encoding]::new($false)
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $scriptDir) { $scriptDir = $PSScriptRoot }
$scriptDir = (Resolve-Path $scriptDir).Path
$projectRoot = Split-Path -Parent $scriptDir

if (-not $ProbeExe) {
    $ProbeExe = Join-Path $projectRoot ("experiments\output_probe\build\{0}\output_probe.exe" -f $Config)
}
if (-not [System.IO.Path]::IsPathRooted($ProbeExe)) {
    $ProbeExe = Join-Path $projectRoot $ProbeExe
}

function Precheck {
    param([bool]$Ok, [string]$Message)
    if (-not $Ok) {
        Write-Output ('FAIL: {0}' -f $Message)
        Write-Output 'SUMMARY total=4 passed=0 failed=4'
        exit 1
    }
}

Precheck (Test-Path -LiteralPath $ProbeExe -PathType Leaf) ('output_probe.exe not found: {0}' -f $ProbeExe)

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $ProbeExe
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
[void]$proc.WaitForExit(30000)
if (-not $proc.HasExited) {
    try { $proc.Kill($true) } catch { try { $proc.Kill() } catch {} }
    Write-Output 'FAIL exit=timeout'
    Write-Output 'SUMMARY total=4 passed=0 failed=4'
    exit 1
}

$stdout = $stdoutTask.GetAwaiter().GetResult()
$stderr = $stderrTask.GetAwaiter().GetResult()
$combined = $stdout + "`n" + $stderr
$exitCode = $proc.ExitCode

function Test-Scenario {
    param(
        [string]$Name,
        [bool]$Ok,
        [string]$Reason
    )
    if ($Ok) {
        Write-Output ('PASS {0}' -f $Name)
        return $true
    }
    Write-Output ('FAIL {0} reason={1}' -f $Name, $Reason)
    return $false
}

$passed = 0
$failed = 0

# case 1：SendScript press LEFT 应发出
$s1 = Test-Scenario -Name 'script_press' -Ok ($combined -match '\[output/mock\] mk\.press left') `
    -Reason 'missing "mk.press left"'
if ($s1) { $passed++ } else { $failed++ }

# case 2：暂停时脚本按下的键应释放
$s2 = Test-Scenario -Name 'pause_releases_keys' -Ok ($combined -match '\[output/mock\] mk\.release left') `
    -Reason 'missing "mk.release left"'
if ($s2) { $passed++ } else { $failed++ }

# case 3：暂停时 SendScript 应被丢弃（不应出现 mk.press right）
$s3 = Test-Scenario -Name 'pause_drops_script' -Ok (-not ($combined -match '\[output/mock\] mk\.press right')) `
    -Reason 'unexpected "mk.press right" while paused'
if ($s3) { $passed++ } else { $failed++ }

# case 4：SendAsync 不受暂停影响
$s4 = Test-Scenario -Name 'async_passthrough' -Ok ($combined -match '\[output/mock\] mk\.press a') `
    -Reason 'missing "mk.press a"'
if ($s4) { $passed++ } else { $failed++ }

$total = $passed + $failed
[Console]::Out.WriteLine(('SUMMARY total={0} passed={1} failed={2}' -f $total, $passed, $failed))

if ($failed -gt 0) { exit 1 }
exit 0
