#Requires -Version 5.1
[CmdletBinding()]
param(
    [string]$ProjectRoot = '',
    [string]$Config = 'Release'
)

$ErrorActionPreference = 'Continue'
$OutputEncoding = [System.Text.UTF8Encoding]::new($false)
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $scriptDir) { $scriptDir = $PSScriptRoot }
$scriptDir = (Resolve-Path $scriptDir).Path
if (-not $ProjectRoot) {
    $ProjectRoot = Split-Path -Parent $scriptDir
}
$ProjectRoot = [System.IO.Path]::GetFullPath($ProjectRoot)

$vsdevcmd = $env:VSDEVCMD
if (-not $vsdevcmd) {
    $vsdevcmd = 'C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat'
}
if (-not (Test-Path -LiteralPath $vsdevcmd)) {
    Write-Output ("[ERROR] VsDevCmd not found: {0}" -f $vsdevcmd)
    Write-Output "SUMMARY: off_build=FAIL off_stubs=FAIL off_stage1=FAIL on_build=FAIL on_normal=FAIL on_stage2=FAIL result=FAIL"
    exit 1
}

$buildDir = Join-Path $ProjectRoot 'build'
$coreExe = Join-Path (Join-Path $buildDir $Config) 'core.exe'
$stubsDir = Join-Path (Join-Path $buildDir $Config) 'stubs'
$pluginsDir = Join-Path (Join-Path $buildDir $Config) 'plugins'
$prepareBad = Join-Path (Join-Path $ProjectRoot 'scripts') 'prepare_bad_plugin_dirs.ps1'
$prepareRt = Join-Path (Join-Path $ProjectRoot 'scripts') 'prepare_runtime_error_dirs.ps1'
$acceptance = Join-Path (Join-Path $ProjectRoot 'scripts') 'run_acceptance.ps1'

function Invoke-External {
    param(
        [string]$FilePath,
        [string]$ArgumentList,
        [string]$WorkingDirectory = $ProjectRoot
    )
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $FilePath
    $psi.Arguments = $ArgumentList
    $psi.WorkingDirectory = $WorkingDirectory
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $psi.StandardOutputEncoding = [System.Text.UTF8Encoding]::new($false)
    $psi.StandardErrorEncoding = [System.Text.UTF8Encoding]::new($false)
    $proc = [System.Diagnostics.Process]::Start($psi)
    $stdout = $proc.StandardOutput.ReadToEnd()
    $stderr = $proc.StandardError.ReadToEnd()
    $proc.WaitForExit()
    return @{
        ExitCode = $proc.ExitCode
        Output   = ($stdout + $stderr)
    }
}

function Invoke-CmdShell {
    param([string]$InnerCmd)
    return Invoke-External -FilePath 'cmd.exe' -ArgumentList ('/c ' + $InnerCmd)
}

function Show-StepFailure {
    param(
        [string]$Label,
        [string]$Command,
        [string]$Output,
        [int]$ExitCode
    )
    [Console]::Out.WriteLine(("FAIL: {0} (exit={1})" -f $Label, $ExitCode))
    [Console]::Out.WriteLine('--- command ---')
    [Console]::Out.WriteLine($Command)
    [Console]::Out.WriteLine('--- output ---')
    [Console]::Out.WriteLine($Output)
    [Console]::Out.WriteLine('--- end ---')
}

function Invoke-CMakeConfigureBuild {
    param([string]$Stage2Flag)
    $cmakeCfg = ('cmake -S "{0}" -B "{1}" -DBUILD_STAGE2_PLUGINS={2}' -f $ProjectRoot, $buildDir, $Stage2Flag)
    $cmakeBuild = ('cmake --build "{0}" --config {1}' -f $buildDir, $Config)
    $inner = ('call "{0}" -arch=x64 -host_arch=x64 && cd /d "{1}" && {2} && {3}' -f $vsdevcmd, $ProjectRoot, $cmakeCfg, $cmakeBuild)
    $result = Invoke-CmdShell -InnerCmd $inner
    return @{
        ExitCode = $result.ExitCode
        Output   = $result.Output
        Command  = $inner
    }
}

function Test-CoreRun {
    param(
        [string]$PluginsDir,
        [string[]]$Keywords,
        [bool]$RequireExitZero = $true
    )
    if (-not (Test-Path -LiteralPath $coreExe)) {
        return @{ Ok = $false; ExitCode = -1; Output = ("core not found: {0}" -f $coreExe); Command = $coreExe }
    }
    $args = ('--plugins-dir "{0}"' -f $PluginsDir)
    $result = Invoke-External -FilePath $coreExe -ArgumentList $args
    $ok = $true
    if ($RequireExitZero -and $result.ExitCode -ne 0) { $ok = $false }
    foreach ($kw in $Keywords) {
        if (-not $result.Output.Contains($kw)) { $ok = $false }
    }
    return @{
        Ok       = $ok
        ExitCode = $result.ExitCode
        Output   = $result.Output
        Command  = ('{0} {1}' -f $coreExe, $args)
    }
}

function Test-Acceptance {
    param([string]$Stage)
    $result = Invoke-External -FilePath 'powershell.exe' -ArgumentList (
        ('-NoProfile -ExecutionPolicy Bypass -File "{0}" -Stage {1}' -f $acceptance, $Stage)
    )
    $ok = ($result.ExitCode -eq 0 -and $result.Output.Contains('failed=0'))
    if (-not $ok) { $ok = $false }
    return @{
        Ok       = $ok
        ExitCode = $result.ExitCode
        Output   = $result.Output
        Command  = ('powershell -NoProfile -ExecutionPolicy Bypass -File "{0}" -Stage {1}' -f $acceptance, $Stage)
    }
}

function Invoke-PrepareScript {
    param([string]$ScriptPath)
    $result = Invoke-External -FilePath 'powershell.exe' -ArgumentList (
        ('-NoProfile -ExecutionPolicy Bypass -File "{0}"' -f $ScriptPath)
    )
    return @{
        Ok       = ($result.ExitCode -eq 0)
        ExitCode = $result.ExitCode
        Output   = $result.Output
        Command  = ('powershell -NoProfile -ExecutionPolicy Bypass -File "{0}"' -f $ScriptPath)
    }
}

$off_build = 'FAIL'
$off_stubs = 'FAIL'
$off_stage1 = 'FAIL'
$on_build = 'FAIL'
$on_normal = 'FAIL'
$on_stage2 = 'FAIL'
$anyFail = $false

# STEP 1/8
[Console]::Out.WriteLine('[STEP 1/8] OFF configure+build ...')
$r = Invoke-CMakeConfigureBuild -Stage2Flag 'OFF'
if ($r.ExitCode -eq 0) {
    $off_build = 'PASS'
    [Console]::Out.WriteLine('[STEP 1/8] OFF configure+build ... OK')
} else {
    $anyFail = $true
    [Console]::Out.WriteLine(('[STEP 1/8] OFF configure+build ... FAIL (exit={0})' -f $r.ExitCode))
    Show-StepFailure -Label 'OFF configure+build' -Command $r.Command -Output $r.Output -ExitCode $r.ExitCode
}

# STEP 2/8
[Console]::Out.WriteLine('[STEP 2/8] OFF stubs load ...')
$r = Test-CoreRun -PluginsDir $stubsDir -Keywords @('[内核] 加载成功') -RequireExitZero $true
if ($r.Ok) {
    $off_stubs = 'PASS'
    [Console]::Out.WriteLine('[STEP 2/8] OFF stubs load ... OK')
} else {
    $anyFail = $true
    [Console]::Out.WriteLine(('[STEP 2/8] OFF stubs load ... FAIL (exit={0})' -f $r.ExitCode))
    Show-StepFailure -Label 'OFF stubs load' -Command $r.Command -Output $r.Output -ExitCode $r.ExitCode
}

# STEP 3/8
[Console]::Out.WriteLine('[STEP 3/8] prepare_bad_plugin_dirs ...')
$r = Invoke-PrepareScript -ScriptPath $prepareBad
if ($r.Ok) {
    [Console]::Out.WriteLine('[STEP 3/8] prepare_bad_plugin_dirs ... OK')
} else {
    $anyFail = $true
    [Console]::Out.WriteLine(('[STEP 3/8] prepare_bad_plugin_dirs ... FAIL (exit={0})' -f $r.ExitCode))
    Show-StepFailure -Label 'prepare_bad_plugin_dirs' -Command $r.Command -Output $r.Output -ExitCode $r.ExitCode
}

# STEP 4/8
[Console]::Out.WriteLine('[STEP 4/8] OFF stage1 acceptance ...')
$r = Test-Acceptance -Stage 'stage1'
if ($r.Ok) {
    $off_stage1 = 'PASS'
    [Console]::Out.WriteLine('[STEP 4/8] OFF stage1 acceptance ... OK')
    if ($r.Output) { [Console]::Out.WriteLine($r.Output.TrimEnd()) }
} else {
    $anyFail = $true
    $off_stage1 = 'FAIL'
    [Console]::Out.WriteLine(('[STEP 4/8] OFF stage1 acceptance ... FAIL (exit={0})' -f $r.ExitCode))
    Show-StepFailure -Label 'OFF stage1 acceptance' -Command $r.Command -Output $r.Output -ExitCode $r.ExitCode
}

# STEP 5/8
[Console]::Out.WriteLine('[STEP 5/8] ON configure+build ...')
$r = Invoke-CMakeConfigureBuild -Stage2Flag 'ON'
if ($r.ExitCode -eq 0) {
    $on_build = 'PASS'
    [Console]::Out.WriteLine('[STEP 5/8] ON configure+build ... OK')
} else {
    $anyFail = $true
    $on_build = 'FAIL'
    [Console]::Out.WriteLine(('[STEP 5/8] ON configure+build ... FAIL (exit={0})' -f $r.ExitCode))
    Show-StepFailure -Label 'ON configure+build' -Command $r.Command -Output $r.Output -ExitCode $r.ExitCode
}

# STEP 6/8
[Console]::Out.WriteLine('[STEP 6/8] ON normal plugins run ...')
$r = Test-CoreRun -PluginsDir $pluginsDir -Keywords @('[帧 5]', '[内核] 5 帧完成') -RequireExitZero $true
if ($r.Ok) {
    $on_normal = 'PASS'
    [Console]::Out.WriteLine('[STEP 6/8] ON normal plugins run ... OK')
} else {
    $anyFail = $true
    $on_normal = 'FAIL'
    [Console]::Out.WriteLine(('[STEP 6/8] ON normal plugins run ... FAIL (exit={0})' -f $r.ExitCode))
    Show-StepFailure -Label 'ON normal plugins run' -Command $r.Command -Output $r.Output -ExitCode $r.ExitCode
}

# STEP 7/8
[Console]::Out.WriteLine('[STEP 7/8] prepare_runtime_error_dirs ...')
$r = Invoke-PrepareScript -ScriptPath $prepareRt
if ($r.Ok) {
    [Console]::Out.WriteLine('[STEP 7/8] prepare_runtime_error_dirs ... OK')
} else {
    $anyFail = $true
    [Console]::Out.WriteLine(('[STEP 7/8] prepare_runtime_error_dirs ... FAIL (exit={0})' -f $r.ExitCode))
    Show-StepFailure -Label 'prepare_runtime_error_dirs' -Command $r.Command -Output $r.Output -ExitCode $r.ExitCode
}

# STEP 8/8
[Console]::Out.WriteLine('[STEP 8/8] ON stage2 acceptance ...')
$r = Test-Acceptance -Stage 'stage2'
if ($r.Ok) {
    $on_stage2 = 'PASS'
    [Console]::Out.WriteLine('[STEP 8/8] ON stage2 acceptance ... OK')
    if ($r.Output) { [Console]::Out.WriteLine($r.Output.TrimEnd()) }
} else {
    $anyFail = $true
    $on_stage2 = 'FAIL'
    [Console]::Out.WriteLine(('[STEP 8/8] ON stage2 acceptance ... FAIL (exit={0})' -f $r.ExitCode))
    Show-StepFailure -Label 'ON stage2 acceptance' -Command $r.Command -Output $r.Output -ExitCode $r.ExitCode
}

$result = if ($anyFail) { 'FAIL' } else { 'PASS' }
[Console]::Out.WriteLine(('SUMMARY: off_build={0} off_stubs={1} off_stage1={2} on_build={3} on_normal={4} on_stage2={5} result={6}' -f `
    $off_build, $off_stubs, $off_stage1, $on_build, $on_normal, $on_stage2, $result))

if ($anyFail) { exit 1 }
exit 0
