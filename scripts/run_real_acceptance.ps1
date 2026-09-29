param(
    [string]$ConfigPath = "build\Release\real_test.ini",
    [string]$Config = "Release",
    [string]$ProjectRoot = "",
    [int]$TimeoutSec = 30
)

$ErrorActionPreference = "Stop"

if ($ProjectRoot -eq "") {
    $ProjectRoot = Split-Path -Parent $PSScriptRoot
}

$coreExe = Join-Path $ProjectRoot "build\$Config\core.exe"
$pluginsRealDir = Join-Path $ProjectRoot "build\$Config\plugins_real"
$requiredDlls = @("capture_plugin.dll", "policy_plugin.dll")

if (-not [System.IO.Path]::IsPathRooted($ConfigPath)) {
    $ConfigPath = Join-Path $ProjectRoot $ConfigPath
}

function Fail($msg) {
    Write-Output "[PRECHECK FAIL] $msg"
    Write-Output "SUMMARY real_e2e=FAIL"
    exit 1
}

if (-not (Test-Path -LiteralPath $coreExe -PathType Leaf)) {
    Fail "core.exe not found: $coreExe"
}
if (-not (Test-Path -LiteralPath $pluginsRealDir -PathType Container)) {
    Fail "plugins_real dir not found: $pluginsRealDir"
}
foreach ($dll in $requiredDlls) {
    $dllPath = Join-Path $pluginsRealDir $dll
    if (-not (Test-Path -LiteralPath $dllPath -PathType Leaf)) {
        Fail "missing DLL in plugins_real: $dll"
    }
}
if (-not (Test-Path -LiteralPath $ConfigPath -PathType Leaf)) {
    Fail "config file not found: $ConfigPath"
}

Write-Output "============================================================"
Write-Output "[警告] 本脚本会通过串口向系统发送真实的键盘输入。"
Write-Output "[警告] 默认策略每帧按一次 I 键。"
Write-Output "[警告] 运行前请："
Write-Output "[警告]   1. 打开记事本，鼠标点击输入区让光标在闪"
Write-Output "[警告]   2. 确认 COM4 未被其他程序占用"
Write-Output "[警告]   3. 确认 plugins_real 里的 DLL 是本次构建的最新版"
Write-Output "============================================================"

$exitCode = -1
$timedOut = $false

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $coreExe
$psi.Arguments = "--config `"$ConfigPath`""
$psi.WorkingDirectory = $ProjectRoot
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

if (-not $proc.WaitForExit($TimeoutSec * 1000)) {
    $timedOut = $true
    try { $proc.Kill($true) } catch { try { $proc.Kill() } catch {} }
    [void]$proc.WaitForExit(5000)
}

$stdoutText = $stdoutTask.GetAwaiter().GetResult()
$stderrText = $stderrTask.GetAwaiter().GetResult()
$exitCode = $proc.ExitCode
$combined = $stdoutText + $stderrText

if ($timedOut) {
    Write-Output "FAIL real_e2e timeout after=${TimeoutSec}s"
    Write-Output "SUMMARY real_e2e=FAIL"
    exit 1
}

if ($exitCode -ne 0) {
    Write-Output "FAIL real_e2e exit=$exitCode reason=non-zero exit code"
    Write-Output "SUMMARY real_e2e=FAIL"
    exit 1
}

$missingKeyword = $null
if (-not $combined.Contains('[帧 5]')) {
    $missingKeyword = "[帧 5]"
} elseif (-not $combined.Contains('[内核] 5 帧完成')) {
    $missingKeyword = "[内核] 5 帧完成"
}
if ($null -ne $missingKeyword) {
    Write-Output "FAIL real_e2e keyword_missing=`"$missingKeyword`""
    Write-Output "SUMMARY real_e2e=FAIL"
    exit 1
}

$errorLine = $null
foreach ($line in ($combined -split "`r?`n")) {
    if ($line -match '^\[错误\]') {
        $errorLine = $line
        break
    }
}
if ($null -ne $errorLine) {
    Write-Output "FAIL real_e2e has_error error_line=`"$errorLine`""
    Write-Output "SUMMARY real_e2e=FAIL"
    exit 1
}

Write-Output "PASS real_e2e exit=0 keyword=`"[内核] 5 帧完成`" no_error=True"
Write-Output "SUMMARY real_e2e=PASS"
exit 0
