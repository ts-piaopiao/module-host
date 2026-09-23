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
$badRoot = Join-Path $releaseFull 'bad_plugins'
$stubsRoot = Join-Path $releaseFull 'stubs'
$acceptanceRootFull = [System.IO.Path]::GetFullPath($AcceptanceRoot)
$scenarioRoot = Join-Path $acceptanceRootFull 'bad_plugins'

function Get-StubDll([string]$kind) {
    $name = "${kind}_plugin.dll"
    $path = Join-Path $stubsRoot $name
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Missing stub DLL: $path"
    }
    return $path
}

function Get-BadDll([string]$scenario, [string]$name) {
    $path = Join-Path (Join-Path $badRoot $scenario) $name
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Missing bad plugin DLL: $path"
    }
    return $path
}

function Copy-ToScenario([string]$destDir, [string]$fileName, [string]$sourcePath) {
    $dest = Join-Path $destDir $fileName
    Copy-Item -LiteralPath $sourcePath -Destination $dest -Force
    return $fileName
}

$captureDll = Get-StubDll 'capture'
$policyDll = Get-StubDll 'policy'
$inputDll = Get-StubDll 'input'

$plans = @(
    @{ Name = 'missing_capture'; Files = @() }
    @{ Name = 'missing_policy'; Files = @(
        @{ FileName = 'capture_plugin.dll'; Source = $captureDll },
        @{ FileName = 'input_plugin.dll'; Source = $inputDll }
    )}
    @{ Name = 'missing_input'; Files = @(
        @{ FileName = 'capture_plugin.dll'; Source = $captureDll },
        @{ FileName = 'policy_plugin.dll'; Source = $policyDll }
    )}
    @{ Name = 'missing_symbol_capture'; Files = @(
        @{ FileName = 'capture_plugin.dll'; Source = (Get-BadDll 'missing_symbol_capture' 'capture_plugin.dll') },
        @{ FileName = 'policy_plugin.dll'; Source = $policyDll },
        @{ FileName = 'input_plugin.dll'; Source = $inputDll }
    )}
    @{ Name = 'missing_symbol_policy'; Files = @(
        @{ FileName = 'capture_plugin.dll'; Source = $captureDll },
        @{ FileName = 'policy_plugin.dll'; Source = (Get-BadDll 'missing_symbol_policy' 'policy_plugin.dll') },
        @{ FileName = 'input_plugin.dll'; Source = $inputDll }
    )}
    @{ Name = 'missing_symbol_input'; Files = @(
        @{ FileName = 'capture_plugin.dll'; Source = $captureDll },
        @{ FileName = 'policy_plugin.dll'; Source = $policyDll },
        @{ FileName = 'input_plugin.dll'; Source = (Get-BadDll 'missing_symbol_input' 'input_plugin.dll') }
    )}
    @{ Name = 'abi_mismatch_capture'; Files = @(
        @{ FileName = 'capture_plugin.dll'; Source = (Get-BadDll 'abi_mismatch_capture' 'capture_plugin.dll') },
        @{ FileName = 'policy_plugin.dll'; Source = $policyDll },
        @{ FileName = 'input_plugin.dll'; Source = $inputDll }
    )}
    @{ Name = 'meta_null_capture'; Files = @(
        @{ FileName = 'capture_plugin.dll'; Source = (Get-BadDll 'meta_null_capture' 'capture_plugin.dll') },
        @{ FileName = 'policy_plugin.dll'; Source = $policyDll },
        @{ FileName = 'input_plugin.dll'; Source = $inputDll }
    )}
    @{ Name = 'meta_seg_count_capture'; Files = @(
        @{ FileName = 'capture_plugin.dll'; Source = (Get-BadDll 'meta_seg_count_capture' 'capture_plugin.dll') },
        @{ FileName = 'policy_plugin.dll'; Source = $policyDll },
        @{ FileName = 'input_plugin.dll'; Source = $inputDll }
    )}
    @{ Name = 'meta_abi_not_number_capture'; Files = @(
        @{ FileName = 'capture_plugin.dll'; Source = (Get-BadDll 'meta_abi_not_number_capture' 'capture_plugin.dll') },
        @{ FileName = 'policy_plugin.dll'; Source = $policyDll },
        @{ FileName = 'input_plugin.dll'; Source = $inputDll }
    )}
    @{ Name = 'meta_kind_mismatch_capture'; Files = @(
        @{ FileName = 'capture_plugin.dll'; Source = (Get-BadDll 'meta_kind_mismatch_capture' 'capture_plugin.dll') },
        @{ FileName = 'policy_plugin.dll'; Source = $policyDll },
        @{ FileName = 'input_plugin.dll'; Source = $inputDll }
    )}
    @{ Name = 'init_fail_capture'; Files = @(
        @{ FileName = 'capture_plugin.dll'; Source = (Get-BadDll 'init_fail_capture' 'capture_plugin.dll') },
        @{ FileName = 'policy_plugin.dll'; Source = $policyDll },
        @{ FileName = 'input_plugin.dll'; Source = $inputDll }
    )}
    @{ Name = 'stubs'; Files = @(
        @{ FileName = 'capture_plugin.dll'; Source = $captureDll },
        @{ FileName = 'policy_plugin.dll'; Source = $policyDll },
        @{ FileName = 'input_plugin.dll'; Source = $inputDll }
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
        $copied = Copy-ToScenario -destDir $dir -fileName $fileSpec.FileName -sourcePath $fileSpec.Source
        $prepared += $copied
        $totalFiles++
    }

    if ($prepared.Count -eq 0) {
        Write-Output ("[{0}] (no files)" -f $plan.Name)
    } else {
        Write-Output ("[{0}] {1}" -f $plan.Name, ($prepared -join ', '))
    }
}

Write-Output ("SUMMARY: scenarios={0} files={1} root={2}" -f $totalScenarios, $totalFiles, $scenarioRoot)
