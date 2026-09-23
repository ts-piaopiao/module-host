#Requires -Version 5.1
[CmdletBinding()]
param(
    [ValidateSet('bad_plugins', 'stubs', 'runtime_errors', 'all')]
    [string]$Suite = 'bad_plugins',
    [ValidateSet('stage1', 'stage2', 'all')]
    [string]$Stage = 'all',
    [string]$Config = 'Release',
    [string]$CorePath = '',
    [string]$AcceptanceRoot = ''
)

$ErrorActionPreference = 'Stop'
$OutputEncoding = [System.Text.UTF8Encoding]::new($false)
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $scriptDir) { $scriptDir = $PSScriptRoot }
$scriptDir = (Resolve-Path $scriptDir).Path
$projectRoot = Split-Path -Parent $scriptDir

if (-not $CorePath) {
    $CorePath = Join-Path $projectRoot "build\$Config\core.exe"
}
if (-not $AcceptanceRoot) {
    $AcceptanceRoot = Join-Path $projectRoot 'build\Release\acceptance'
}

$coreFull = [System.IO.Path]::GetFullPath($CorePath)
$acceptanceFull = [System.IO.Path]::GetFullPath($AcceptanceRoot)
$badPluginsRoot = Join-Path $acceptanceFull 'bad_plugins'
$runtimeRoot = Join-Path $acceptanceFull 'runtime_errors'
$stubsPluginsDir = Join-Path (Join-Path $projectRoot (Join-Path 'build' $Config)) 'stubs'
$normalPluginsDir = Join-Path (Join-Path $projectRoot (Join-Path 'build' $Config)) 'plugins'

if (-not (Test-Path -LiteralPath $coreFull)) {
    Write-Output ("FAIL: core not found: {0}" -f $coreFull)
    exit 1
}

$expectations = [ordered]@{
    'missing_capture'              = @{ Keyword = '缺失 DLL: capture_plugin.dll'; ExitZero = $false }
    'missing_policy'               = @{ Keyword = '缺失 DLL: policy_plugin.dll'; ExitZero = $false }
    'missing_input'                = @{ Keyword = '缺失 DLL: input_plugin.dll'; ExitZero = $false }
    'missing_symbol_capture'       = @{ Keyword = '缺失符号: plugin_capture'; ExitZero = $false }
    'missing_symbol_policy'        = @{ Keyword = '缺失符号: plugin_decide'; ExitZero = $false }
    'missing_symbol_input'         = @{ Keyword = '缺失符号: plugin_execute'; ExitZero = $false }
    'abi_mismatch_capture'         = @{ Keyword = 'ABI 不匹配'; ExitZero = $false }
    'meta_null_capture'            = @{ Keyword = '元数据无效: NULL'; ExitZero = $false }
    'meta_seg_count_capture'       = @{ Keyword = '元数据无效: 段数不是 4'; ExitZero = $false }
    'meta_abi_not_number_capture'  = @{ Keyword = '元数据无效: ABI 非数字'; ExitZero = $false }
    'meta_kind_mismatch_capture'   = @{ Keyword = '元数据无效: kind 不匹配'; ExitZero = $false }
    'init_fail_capture'            = @{ Keyword = '初始化失败'; ExitZero = $false }
    'stubs'                        = @{ Keyword = '加载成功'; ExitZero = $true }
    'capture_fail'                 = @{ Keyword = '捕获失败'; ExitZero = $false }
    'decide_fail'                  = @{ Keyword = '决策失败'; ExitZero = $false }
    'decide_over_count'            = @{ Keyword = 'out_count 违约'; ExitZero = $false }
    'decide_empty'                 = @{ Keyword = '无意图'; ExitZero = $true }
    'execute_fail'                 = @{ Keyword = '执行失败'; ExitZero = $false }
}

function Invoke-CoreRun {
    param(
        [string]$PluginsDir
    )

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $coreFull
    $psi.Arguments = ('--plugins-dir "{0}"' -f $PluginsDir)
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
    $exitCode = $proc.ExitCode

    return @{
        ExitCode = $exitCode
        Combined = ($stdout + $stderr)
        Stdout   = $stdout
        Stderr   = $stderr
    }
}

function Write-ScenarioLine {
    param(
        [string]$Status,
        [string]$ScenarioName,
        [int]$ExitCode,
        [string]$Keyword,
        [bool]$KeywordOk,
        [string]$ReasonText
    )
    $line = '{0} {1} exit={2} keyword="{3}" match={4} reason={5}' -f `
        $Status, $ScenarioName, $ExitCode, $Keyword, $KeywordOk, $ReasonText
    [Console]::Out.WriteLine($line)
}

function Invoke-CoreScenario {
    param(
        [string]$ScenarioName,
        [string]$PluginsDir
    )

    $run = Invoke-CoreRun -PluginsDir $PluginsDir
    $exitCode = $run.ExitCode
    $combined = $run.Combined
    $stdout = $run.Stdout
    $stderr = $run.Stderr

    $expectation = $expectations[$ScenarioName]
    $keyword = $expectation.Keyword
    $exitZero = [bool]$expectation.ExitZero

    $keywordOk = $combined.Contains($keyword)
    $exitOk = $false
    if ($exitZero) {
        $exitOk = ($exitCode -eq 0)
    } else {
        $exitOk = ($exitCode -ne 0)
    }

    $pass = $keywordOk -and $exitOk
    $reasons = @()
    if (-not $keywordOk) {
        $reasons += ('missing keyword "{0}"' -f $keyword)
    }
    if (-not $exitOk) {
        if ($exitZero) {
            $reasons += ('expected exit 0 got {0}' -f $exitCode)
        } else {
            $reasons += ('expected non-zero exit got {0}' -f $exitCode)
        }
    }

    $status = if ($pass) { 'PASS' } else { 'FAIL' }
    $reasonText = if ($pass) { 'ok' } else { ($reasons -join '; ') }
    Write-ScenarioLine -Status $status -ScenarioName $ScenarioName -ExitCode $exitCode `
        -Keyword $keyword -KeywordOk $keywordOk -ReasonText $reasonText
    if (-not $pass) {
        if ($stdout) { [Console]::Out.WriteLine('  stdout: ' + $stdout.Trim()) }
        if ($stderr) { [Console]::Out.WriteLine('  stderr: ' + $stderr.Trim()) }
    }

    return $pass
}

function Invoke-Stage1BuildGate {
    $keyword = '加载成功'
    $stubsDir = Join-Path $badPluginsRoot 'stubs'

    if (-not (Test-Path -LiteralPath $stubsDir)) {
        Write-ScenarioLine -Status 'FAIL' -ScenarioName 'stage1_build_gate' -ExitCode 1 `
            -Keyword $keyword -KeywordOk $false `
            -ReasonText ('stubs dir not found: {0}' -f $stubsDir)
        [Console]::Out.WriteLine('[门禁] stage1 要求 core.exe 为 OFF 构建，当前检测到 stubs 场景未输出“加载成功”')
        return $false
    }

    $run = Invoke-CoreRun -PluginsDir $stubsDir
    $exitCode = $run.ExitCode
    $keywordOk = $run.Combined.Contains($keyword)
    $exitOk = ($exitCode -eq 0)
    $pass = $keywordOk -and $exitOk

    $reasons = @()
    if (-not $keywordOk) {
        $reasons += ('missing keyword "{0}"' -f $keyword)
    }
    if (-not $exitOk) {
        $reasons += ('expected exit 0 got {0}' -f $exitCode)
    }

    $status = if ($pass) { 'PASS' } else { 'FAIL' }
    $reasonText = if ($pass) { 'ok' } else { ($reasons -join '; ') }
    Write-ScenarioLine -Status $status -ScenarioName 'stage1_build_gate' -ExitCode $exitCode `
        -Keyword $keyword -KeywordOk $keywordOk -ReasonText $reasonText
    if (-not $pass) {
        if ($run.Stdout) { [Console]::Out.WriteLine('  stdout: ' + $run.Stdout.Trim()) }
        if ($run.Stderr) { [Console]::Out.WriteLine('  stderr: ' + $run.Stderr.Trim()) }
        [Console]::Out.WriteLine('[门禁] stage1 要求 core.exe 为 OFF 构建，当前检测到 stubs 场景未输出“加载成功”')
    }

    return $pass
}

function Invoke-Stage2BuildGate {
    $keyword = '[帧 5]'

    if (-not (Test-Path -LiteralPath $normalPluginsDir)) {
        Write-ScenarioLine -Status 'FAIL' -ScenarioName 'stage2_build_gate' -ExitCode 1 `
            -Keyword $keyword -KeywordOk $false `
            -ReasonText ('plugins dir not found: {0}' -f $normalPluginsDir)
        [Console]::Out.WriteLine('[门禁] stage2 要求 core.exe 为 ON 构建')
        return $false
    }

    $run = Invoke-CoreRun -PluginsDir $normalPluginsDir
    $exitCode = $run.ExitCode
    $keywordOk = $run.Combined.Contains($keyword)
    $exitOk = ($exitCode -eq 0)
    $pass = $keywordOk -and $exitOk

    $reasons = @()
    if (-not $keywordOk) {
        $reasons += ('missing keyword "{0}"' -f $keyword)
    }
    if (-not $exitOk) {
        $reasons += ('expected exit 0 got {0}' -f $exitCode)
    }

    $status = if ($pass) { 'PASS' } else { 'FAIL' }
    $reasonText = if ($pass) { 'ok' } else { ($reasons -join '; ') }
    Write-ScenarioLine -Status $status -ScenarioName 'stage2_build_gate' -ExitCode $exitCode `
        -Keyword $keyword -KeywordOk $keywordOk -ReasonText $reasonText
    if (-not $pass) {
        if ($run.Stdout) { [Console]::Out.WriteLine('  stdout: ' + $run.Stdout.Trim()) }
        if ($run.Stderr) { [Console]::Out.WriteLine('  stderr: ' + $run.Stderr.Trim()) }
        [Console]::Out.WriteLine('[门禁] stage2 要求 core.exe 为 ON 构建')
    }

    return $pass
}

$effectiveSuite = $Suite
if ($Stage -eq 'stage1') {
    $effectiveSuite = 'bad_plugins'
} elseif ($Stage -eq 'stage2') {
    $effectiveSuite = 'runtime_errors'
}

$passed = 0
$failed = 0

if ($Stage -eq 'stage1') {
    $gateOk = Invoke-Stage1BuildGate
    if ($gateOk) { $passed++ } else { $failed++ }
} elseif ($Stage -eq 'stage2') {
    $gateOk = Invoke-Stage2BuildGate
    if ($gateOk) { $passed++ } else { $failed++ }
}

$scenarios = @()

if ($effectiveSuite -eq 'bad_plugins' -or $effectiveSuite -eq 'all') {
    if (-not (Test-Path -LiteralPath $badPluginsRoot)) {
        Write-Output ("FAIL: bad_plugins root not found: {0}" -f $badPluginsRoot)
        exit 1
    }
    $dirs = Get-ChildItem -LiteralPath $badPluginsRoot -Directory | Sort-Object Name
    foreach ($dir in $dirs) {
        if ($Stage -eq 'stage1' -and $dir.Name -eq 'stubs') {
            continue
        }
        $scenarios += @{ Name = $dir.Name; PluginsDir = $dir.FullName }
    }
}

if ($effectiveSuite -eq 'stubs') {
    if (-not (Test-Path -LiteralPath $stubsPluginsDir)) {
        Write-Output ("FAIL: stubs dir not found: {0}" -f $stubsPluginsDir)
        exit 1
    }
    $scenarios += @{ Name = 'stubs'; PluginsDir = $stubsPluginsDir }
}

if ($effectiveSuite -eq 'runtime_errors' -or $effectiveSuite -eq 'all') {
    if (-not (Test-Path -LiteralPath $runtimeRoot)) {
        Write-Output ("FAIL: runtime_errors root not found: {0}" -f $runtimeRoot)
        exit 1
    }
    $dirs = Get-ChildItem -LiteralPath $runtimeRoot -Directory | Sort-Object Name
    foreach ($dir in $dirs) {
        $scenarios += @{ Name = $dir.Name; PluginsDir = $dir.FullName }
    }
}

foreach ($scenario in $scenarios) {
    $name = $scenario.Name
    if (-not $expectations.Contains($name)) {
        Write-Output ('FAIL {0} reason=no expectation' -f $name)
        $failed++
        continue
    }

    $ok = Invoke-CoreScenario -ScenarioName $name -PluginsDir $scenario.PluginsDir
    if ($ok) {
        $passed++
    } else {
        $failed++
    }
}

$total = $passed + $failed
[Console]::Out.WriteLine(('SUMMARY total={0} passed={1} failed={2}' -f $total, $passed, $failed))

if ($failed -gt 0) {
    exit 1
}
exit 0
