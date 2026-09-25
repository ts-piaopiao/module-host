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
    Write-Output "SUMMARY: contract=FAIL off_build=FAIL off_stubs=FAIL off_stage1=FAIL on_build=FAIL script=FAIL output_probe=FAIL on_normal=FAIL on_stage2=FAIL result=FAIL"
    exit 1
}

$buildDir = Join-Path $ProjectRoot 'build'
$coreExe = Join-Path (Join-Path $buildDir $Config) 'core.exe'
$stubsDir = Join-Path (Join-Path $buildDir $Config) 'stubs'
$pluginsDir = Join-Path (Join-Path $buildDir $Config) 'plugins'
$prepareBad = Join-Path (Join-Path $ProjectRoot 'scripts') 'prepare_bad_plugin_dirs.ps1'
$prepareRt = Join-Path (Join-Path $ProjectRoot 'scripts') 'prepare_runtime_error_dirs.ps1'
$acceptance = Join-Path (Join-Path $ProjectRoot 'scripts') 'run_acceptance.ps1'
$contractAcceptance = Join-Path (Join-Path $ProjectRoot 'scripts') 'run_contract_acceptance.ps1'
$scriptAcceptance = Join-Path (Join-Path $ProjectRoot 'scripts') 'run_script_acceptance.ps1'
$scriptReplayDir = Join-Path $ProjectRoot 'experiments\script_replay'
$outputProbeDir = Join-Path $ProjectRoot 'experiments\output_probe'
$outputProbeAcceptance = Join-Path (Join-Path $ProjectRoot 'scripts') 'run_output_probe.ps1'

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
    $args = ('--plugins-dir "{0}" --input-port none' -f $PluginsDir)
    $result = Invoke-External -FilePath $coreExe -ArgumentList $args
    $ok = $true
    # mock 模式自检：run_all 的所有 core.exe 调用都传 --input-port none，
    # 若输出里没有 mock 模式日志，说明 mock 分支失效（可能回退到真串口）
    if (-not $result.Output.Contains('[output] mock 模式')) { $ok = $false }
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

$contract = 'FAIL'
$script = 'FAIL'
$output_probe = 'FAIL'
$off_build = 'FAIL'
$off_stubs = 'FAIL'
$off_stage1 = 'FAIL'
$on_build = 'FAIL'
$on_normal = 'FAIL'
$on_stage2 = 'FAIL'
$anyFail = $false

# STEP 1/11
[Console]::Out.WriteLine('[STEP 1/11] contract compile (C11 + C++17) ...')
$inner = ('call "{0}" -arch=x64 -host_arch=x64 && powershell -NoProfile -ExecutionPolicy Bypass -File "{1}"' -f $vsdevcmd, $contractAcceptance)
$r = Invoke-CmdShell -InnerCmd $inner
if ($r.ExitCode -eq 0 -and $r.Output.Contains('failed=0')) {
    $contract = 'PASS'
    [Console]::Out.WriteLine('[STEP 1/11] contract compile (C11 + C++17) ... OK')
    if ($r.Output) { [Console]::Out.WriteLine($r.Output.TrimEnd()) }
} else {
    $anyFail = $true
    [Console]::Out.WriteLine(('[STEP 1/11] contract compile (C11 + C++17) ... FAIL (exit={0})' -f $r.ExitCode))
    Show-StepFailure -Label 'contract compile' -Command $inner -Output $r.Output -ExitCode $r.ExitCode
}

# STEP 2/11
[Console]::Out.WriteLine('[STEP 2/11] OFF configure+build ...')
$r = Invoke-CMakeConfigureBuild -Stage2Flag 'OFF'
if ($r.ExitCode -eq 0) {
    $off_build = 'PASS'
    [Console]::Out.WriteLine('[STEP 2/11] OFF configure+build ... OK')
} else {
    $anyFail = $true
    [Console]::Out.WriteLine(('[STEP 2/11] OFF configure+build ... FAIL (exit={0})' -f $r.ExitCode))
    Show-StepFailure -Label 'OFF configure+build' -Command $r.Command -Output $r.Output -ExitCode $r.ExitCode
}

# STEP 3/11
[Console]::Out.WriteLine('[STEP 3/11] OFF stubs load ...')
$r = Test-CoreRun -PluginsDir $stubsDir -Keywords @('[内核] 加载成功') -RequireExitZero $true
if ($r.Ok) {
    $off_stubs = 'PASS'
    [Console]::Out.WriteLine('[STEP 3/11] OFF stubs load ... OK')
} else {
    $anyFail = $true
    [Console]::Out.WriteLine(('[STEP 3/11] OFF stubs load ... FAIL (exit={0})' -f $r.ExitCode))
    Show-StepFailure -Label 'OFF stubs load' -Command $r.Command -Output $r.Output -ExitCode $r.ExitCode
}

# STEP 4/11
[Console]::Out.WriteLine('[STEP 4/11] prepare_bad_plugin_dirs ...')
$r = Invoke-PrepareScript -ScriptPath $prepareBad
if ($r.Ok) {
    [Console]::Out.WriteLine('[STEP 4/11] prepare_bad_plugin_dirs ... OK')
} else {
    $anyFail = $true
    [Console]::Out.WriteLine(('[STEP 4/11] prepare_bad_plugin_dirs ... FAIL (exit={0})' -f $r.ExitCode))
    Show-StepFailure -Label 'prepare_bad_plugin_dirs' -Command $r.Command -Output $r.Output -ExitCode $r.ExitCode
}

# STEP 5/11
[Console]::Out.WriteLine('[STEP 5/11] OFF stage1 acceptance ...')
$r = Test-Acceptance -Stage 'stage1'
if ($r.Ok) {
    $off_stage1 = 'PASS'
    [Console]::Out.WriteLine('[STEP 5/11] OFF stage1 acceptance ... OK')
    if ($r.Output) { [Console]::Out.WriteLine($r.Output.TrimEnd()) }
} else {
    $anyFail = $true
    $off_stage1 = 'FAIL'
    [Console]::Out.WriteLine(('[STEP 5/11] OFF stage1 acceptance ... FAIL (exit={0})' -f $r.ExitCode))
    Show-StepFailure -Label 'OFF stage1 acceptance' -Command $r.Command -Output $r.Output -ExitCode $r.ExitCode
}

# STEP 6/11
[Console]::Out.WriteLine('[STEP 6/11] ON configure+build ...')
$r = Invoke-CMakeConfigureBuild -Stage2Flag 'ON'
if ($r.ExitCode -eq 0) {
    $on_build = 'PASS'
    [Console]::Out.WriteLine('[STEP 6/11] ON configure+build ... OK')
} else {
    $anyFail = $true
    $on_build = 'FAIL'
    [Console]::Out.WriteLine(('[STEP 6/11] ON configure+build ... FAIL (exit={0})' -f $r.ExitCode))
    Show-StepFailure -Label 'ON configure+build' -Command $r.Command -Output $r.Output -ExitCode $r.ExitCode
}

# STEP 7/11
[Console]::Out.WriteLine('[STEP 7/11] script replay build + acceptance ...')
$inner = ('call "{0}" -arch=x64 -host_arch=x64 && cd /d "{1}" && cmake -S "{2}" -B "{2}\build" && cmake --build "{2}\build" --config {3}' -f $vsdevcmd, $ProjectRoot, $scriptReplayDir, $Config)
$rBuild = Invoke-CmdShell -InnerCmd $inner
if ($rBuild.ExitCode -ne 0) {
    $anyFail = $true
    [Console]::Out.WriteLine(('[STEP 7/11] script replay build + acceptance ... FAIL (build exit={0})' -f $rBuild.ExitCode))
    Show-StepFailure -Label 'script replay build' -Command $inner -Output $rBuild.Output -ExitCode $rBuild.ExitCode
} else {
    $rAccept = Invoke-External -FilePath 'powershell.exe' -ArgumentList (
        ('-NoProfile -ExecutionPolicy Bypass -File "{0}"' -f $scriptAcceptance)
    )
    if ($rAccept.ExitCode -eq 0 -and $rAccept.Output.Contains('failed=0')) {
        $script = 'PASS'
        [Console]::Out.WriteLine('[STEP 7/11] script replay build + acceptance ... OK')
        if ($rAccept.Output) { [Console]::Out.WriteLine($rAccept.Output.TrimEnd()) }
    } else {
        $anyFail = $true
        [Console]::Out.WriteLine(('[STEP 7/11] script replay build + acceptance ... FAIL (acceptance exit={0})' -f $rAccept.ExitCode))
        Show-StepFailure -Label 'script replay acceptance' -Command ('powershell -File "{0}"' -f $scriptAcceptance) -Output $rAccept.Output -ExitCode $rAccept.ExitCode
    }
}

# STEP 8/11
[Console]::Out.WriteLine('[STEP 8/11] output probe build + acceptance ...')
$inner = ('call "{0}" -arch=x64 -host_arch=x64 && cd /d "{1}" && cmake -S "{2}" -B "{2}\build" && cmake --build "{2}\build" --config {3}' -f $vsdevcmd, $ProjectRoot, $outputProbeDir, $Config)
$rBuild = Invoke-CmdShell -InnerCmd $inner
if ($rBuild.ExitCode -ne 0) {
    $anyFail = $true
    [Console]::Out.WriteLine(('[STEP 8/11] output probe build + acceptance ... FAIL (build exit={0})' -f $rBuild.ExitCode))
    Show-StepFailure -Label 'output probe build' -Command $inner -Output $rBuild.Output -ExitCode $rBuild.ExitCode
} else {
    $rAccept = Invoke-External -FilePath 'powershell.exe' -ArgumentList (
        ('-NoProfile -ExecutionPolicy Bypass -File "{0}"' -f $outputProbeAcceptance)
    )
    if ($rAccept.ExitCode -eq 0 -and $rAccept.Output.Contains('failed=0')) {
        $output_probe = 'PASS'
        [Console]::Out.WriteLine('[STEP 8/11] output probe build + acceptance ... OK')
        if ($rAccept.Output) { [Console]::Out.WriteLine($rAccept.Output.TrimEnd()) }
    } else {
        $anyFail = $true
        [Console]::Out.WriteLine(('[STEP 8/11] output probe build + acceptance ... FAIL (acceptance exit={0})' -f $rAccept.ExitCode))
        Show-StepFailure -Label 'output probe acceptance' -Command ('powershell -File "{0}"' -f $outputProbeAcceptance) -Output $rAccept.Output -ExitCode $rAccept.ExitCode
    }
}

# STEP 9/11
[Console]::Out.WriteLine('[STEP 9/11] ON normal plugins run ...')
$r = Test-CoreRun -PluginsDir $pluginsDir -Keywords @('[帧 5]', '[内核] 5 帧完成') -RequireExitZero $true
if ($r.Ok) {
    $on_normal = 'PASS'
    [Console]::Out.WriteLine('[STEP 9/11] ON normal plugins run ... OK')
} else {
    $anyFail = $true
    $on_normal = 'FAIL'
    [Console]::Out.WriteLine(('[STEP 9/11] ON normal plugins run ... FAIL (exit={0})' -f $r.ExitCode))
    Show-StepFailure -Label 'ON normal plugins run' -Command $r.Command -Output $r.Output -ExitCode $r.ExitCode
}

# STEP 10/11
[Console]::Out.WriteLine('[STEP 10/11] prepare_runtime_error_dirs ...')
$r = Invoke-PrepareScript -ScriptPath $prepareRt
if ($r.Ok) {
    [Console]::Out.WriteLine('[STEP 10/11] prepare_runtime_error_dirs ... OK')
} else {
    $anyFail = $true
    [Console]::Out.WriteLine(('[STEP 10/11] prepare_runtime_error_dirs ... FAIL (exit={0})' -f $r.ExitCode))
    Show-StepFailure -Label 'prepare_runtime_error_dirs' -Command $r.Command -Output $r.Output -ExitCode $r.ExitCode
}

# STEP 11/11
[Console]::Out.WriteLine('[STEP 11/11] ON stage2 acceptance ...')
$r = Test-Acceptance -Stage 'stage2'
if ($r.Ok) {
    $on_stage2 = 'PASS'
    [Console]::Out.WriteLine('[STEP 11/11] ON stage2 acceptance ... OK')
    if ($r.Output) { [Console]::Out.WriteLine($r.Output.TrimEnd()) }
} else {
    $anyFail = $true
    $on_stage2 = 'FAIL'
    [Console]::Out.WriteLine(('[STEP 11/11] ON stage2 acceptance ... FAIL (exit={0})' -f $r.ExitCode))
    Show-StepFailure -Label 'ON stage2 acceptance' -Command $r.Command -Output $r.Output -ExitCode $r.ExitCode
}

$result = if ($anyFail) { 'FAIL' } else { 'PASS' }
[Console]::Out.WriteLine(('SUMMARY: contract={0} off_build={1} off_stubs={2} off_stage1={3} on_build={4} script={5} output_probe={6} on_normal={7} on_stage2={8} result={9}' -f `
    $contract, $off_build, $off_stubs, $off_stage1, $on_build, $script, $output_probe, $on_normal, $on_stage2, $result))

if ($anyFail) { exit 1 }
exit 0
