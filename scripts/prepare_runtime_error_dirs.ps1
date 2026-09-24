#Requires -Version 5.1
[CmdletBinding()]
param(
    [string]$AcceptanceRoot = '',
    [string]$ReleaseDir = ''
)

$ErrorActionPreference = 'Stop'
$OutputEncoding = [System.Text.UTF8Encoding]::new($false)
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)

$projectRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $projectRoot) { $projectRoot = $PSScriptRoot }
$projectRoot = (Resolve-Path $projectRoot).Path
$projectRoot = Split-Path -Parent $projectRoot

if (-not $ReleaseDir) {
    $ReleaseDir = Join-Path $projectRoot 'build\Release'
}
if (-not $AcceptanceRoot) {
    $AcceptanceRoot = Join-Path $projectRoot 'build\Release\acceptance'
}

$releaseFull = (Resolve-Path $ReleaseDir).Path
$normalRoot = Join-Path $releaseFull 'plugins'
$rtRoot = Join-Path $releaseFull 'runtime_errors'
$acceptanceRootFull = [System.IO.Path]::GetFullPath($AcceptanceRoot)
$scenarioRoot = Join-Path $acceptanceRootFull 'runtime_errors'

function Get-NormalDll([string]$kind) {
    $name = "${kind}_plugin.dll"
    $path = Join-Path $normalRoot $name
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Missing normal plugin DLL: $path"
    }
    return $path
}

function Get-RtDll([string]$scenario, [string]$name) {
    $path = Join-Path (Join-Path $rtRoot $scenario) $name
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Missing runtime error plugin DLL: $path"
    }
    return $path
}

$captureDll = Get-NormalDll 'capture'
$policyDll = Get-NormalDll 'policy'

$plans = @(
    @{ Name = 'capture_fail'; Files = @(
        @{ FileName = 'capture_plugin.dll'; Source = (Get-RtDll 'capture_fail' 'capture_plugin.dll') },
        @{ FileName = 'policy_plugin.dll'; Source = $policyDll }
    )}
    @{ Name = 'decide_fail'; Files = @(
        @{ FileName = 'capture_plugin.dll'; Source = $captureDll },
        @{ FileName = 'policy_plugin.dll'; Source = (Get-RtDll 'decide_fail' 'policy_plugin.dll') }
    )}
    @{ Name = 'decide_over_count'; Files = @(
        @{ FileName = 'capture_plugin.dll'; Source = $captureDll },
        @{ FileName = 'policy_plugin.dll'; Source = (Get-RtDll 'decide_over_count' 'policy_plugin.dll') }
    )}
    @{ Name = 'decide_empty'; Files = @(
        @{ FileName = 'capture_plugin.dll'; Source = $captureDll },
        @{ FileName = 'policy_plugin.dll'; Source = (Get-RtDll 'decide_empty' 'policy_plugin.dll') }
    )}
)

if (Test-Path -LiteralPath $scenarioRoot) {
    Remove-Item -LiteralPath $scenarioRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $scenarioRoot -Force | Out-Null

$totalScenarios = 0
$totalFiles = 0

foreach ($plan in $plans) {
    $totalScenarios++
    $dir = Join-Path $scenarioRoot $plan.Name
    New-Item -ItemType Directory -Path $dir -Force | Out-Null

    $prepared = @()
    foreach ($fileSpec in $plan.Files) {
        $dest = Join-Path $dir $fileSpec.FileName
        Copy-Item -LiteralPath $fileSpec.Source -Destination $dest -Force
        $prepared += $fileSpec.FileName
        $totalFiles++
    }

    Write-Output ("[{0}] {1}" -f $plan.Name, ($prepared -join ', '))
}

Write-Output ("SUMMARY: scenarios={0} files={1} root={2}" -f $totalScenarios, $totalFiles, $scenarioRoot)
