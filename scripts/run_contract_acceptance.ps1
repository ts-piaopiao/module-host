#Requires -Version 5.1
[CmdletBinding()]
param(
    [string]$ClPath = 'cl'
)

$ErrorActionPreference = 'Stop'
$OutputEncoding = [System.Text.UTF8Encoding]::new($false)
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $scriptDir) { $scriptDir = $PSScriptRoot }
$scriptDir = (Resolve-Path $scriptDir).Path
$projectRoot = Split-Path -Parent $scriptDir

$TimeoutSec = 60
$TailLines = 12

function Precheck {
    param([bool]$Ok, [string]$Message)
    if (-not $Ok) {
        Write-Output ('FAIL: {0}' -f $Message)
        exit 1
    }
}

$clExe = ''
if ([System.IO.Path]::IsPathRooted($ClPath)) {
    if (Test-Path -LiteralPath $ClPath -PathType Leaf) { $clExe = $ClPath }
}
else {
    $clCmd = Get-Command $ClPath -ErrorAction SilentlyContinue
    if ($clCmd -and $clCmd.CommandType -eq 'Application') { $clExe = $clCmd.Path }
}
Precheck ([bool]$clExe) 'cl.exe not found (run from Developer Command Prompt?)'

$cases = @(
    @{ Name = 'test_contract_c';   Src = 'tests\test_contract_c.c';   Obj = 'test_contract_c.obj';   Args = ('/nologo /std:c11  /I"{0}" /c "tests\test_contract_c.c"' -f $projectRoot) },
    @{ Name = 'test_contract_cpp'; Src = 'tests\test_contract_cpp.cpp'; Obj = 'test_contract_cpp.obj'; Args = ('/nologo /std:c++17 /I"{0}" /c "tests\test_contract_cpp.cpp"' -f $projectRoot) }
)

foreach ($case in $cases) {
    $srcPath = Join-Path $projectRoot $case.Src
    Precheck (Test-Path -LiteralPath $srcPath -PathType Leaf) ('source not found: {0}' -f $srcPath)
}

$tempDir = Join-Path ([System.IO.Path]::GetTempPath()) `
    ('contract_acceptance_{0}' -f ([System.IO.Path]::GetRandomFileName() -replace '\.', ''))
New-Item -ItemType Directory -Path $tempDir -Force | Out-Null

function Invoke-Compile {
    param([string]$ClArgs)

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $clExe
    $psi.Arguments = $ClArgs
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

foreach ($case in $cases) {
    $objPath = Join-Path $tempDir $case.Obj
    $run = Invoke-Compile -ClArgs ($case.Args + (' /Fo"{0}"' -f $objPath))
    $exitCode = $run.ExitCode

    if ($exitCode -eq 0) {
        Write-Output ('PASS {0} exit=0' -f $case.Name)
        $passed++
        continue
    }

    $reasons = @()
    if ($run.TimedOut) { $reasons += 'timeout' }

    $errLines = @($run.Stderr -split "`r?`n" | Where-Object { $_.Trim() })
    $outLines = @($run.Stdout -split "`r?`n" | Where-Object { $_.Trim() })
    if ($errLines.Count -gt 0) {
        $reasons += ('stderr={0}' -f $errLines[-1])
    }
    elseif ($outLines.Count -gt 0) {
        $reasons += ('stdout={0}' -f $outLines[-1])
    }
    if ($reasons.Count -eq 0) { $reasons += 'no compiler output' }

    Write-Output ('FAIL {0} exit={1} reason={2}' -f $case.Name, $exitCode, ($reasons -join '; '))

    $tail = $errLines
    if ($tail.Count -eq 0) { $tail = $outLines }
    $tail = @($tail | Select-Object -Last $TailLines)
    foreach ($line in $tail) {
        [Console]::Out.WriteLine('  ' + $line)
    }
    $failed++
}

try {
    Remove-Item -LiteralPath $tempDir -Recurse -Force -ErrorAction Stop
}
catch {
    Write-Output ('WARN: failed to remove temp dir {0}: {1}' -f $tempDir, $_.Exception.Message)
}

$total = $passed + $failed
[Console]::Out.WriteLine(('SUMMARY total={0} passed={1} failed={2}' -f $total, $passed, $failed))

if ($failed -gt 0) {
    exit 1
}
exit 0
