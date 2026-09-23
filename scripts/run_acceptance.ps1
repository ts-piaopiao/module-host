#Requires -Version 5.1
[CmdletBinding()]
param(
    [ValidateSet('bad_plugins', 'stubs', 'all')]
    [string]$Suite = 'bad_plugins',
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
$stubsPluginsDir = Join-Path (Join-Path $projectRoot (Join-Path 'build' $Config)) 'stubs'

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
}

function Invoke-CoreScenario {
    param(
        [string]$ScenarioName,
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

    $combined = $stdout + $stderr
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
    $line = '{0} {1} exit={2} keyword="{3}" match={4} reason={5}' -f `
        $status, $ScenarioName, $exitCode, $keyword, $keywordOk, $reasonText
    [Console]::Out.WriteLine($line)
    if (-not $pass) {
        if ($stdout) { [Console]::Out.WriteLine('  stdout: ' + $stdout.Trim()) }
        if ($stderr) { [Console]::Out.WriteLine('  stderr: ' + $stderr.Trim()) }
    }

    return $pass
}

$scenarios = @()

if ($Suite -eq 'bad_plugins' -or $Suite -eq 'all') {
    if (-not (Test-Path -LiteralPath $badPluginsRoot)) {
        Write-Output ("FAIL: bad_plugins root not found: {0}" -f $badPluginsRoot)
        exit 1
    }
    $dirs = Get-ChildItem -LiteralPath $badPluginsRoot -Directory | Sort-Object Name
    foreach ($dir in $dirs) {
        $scenarios += @{ Name = $dir.Name; PluginsDir = $dir.FullName }
    }
}

if ($Suite -eq 'stubs') {
    if (-not (Test-Path -LiteralPath $stubsPluginsDir)) {
        Write-Output ("FAIL: stubs dir not found: {0}" -f $stubsPluginsDir)
        exit 1
    }
    $scenarios += @{ Name = 'stubs'; PluginsDir = $stubsPluginsDir }
}

$passed = 0
$failed = 0

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
