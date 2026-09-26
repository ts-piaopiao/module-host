#Requires -Version 5.1
[CmdletBinding()]
param(
    [string]$Config = 'Release'
)

$ErrorActionPreference = 'Stop'
$OutputEncoding = [System.Text.UTF8Encoding]::new($false)
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false)

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
if (-not $scriptDir) { $scriptDir = $PSScriptRoot }
$scriptDir = (Resolve-Path $scriptDir).Path
$projectRoot = Split-Path -Parent $scriptDir

$srcStubs = Join-Path $projectRoot "build\$Config\stubs"
$srcFake = Join-Path $projectRoot "build\$Config\plugins_fake_scene"
$dst = Join-Path $projectRoot "build\$Config\acceptance\fake_scene"

if (Test-Path -LiteralPath $dst) {
    Remove-Item -LiteralPath $dst -Recurse -Force
}
New-Item -ItemType Directory -Path $dst -Force | Out-Null

$cap = Join-Path $srcFake 'capture_plugin.dll'
$pol = Join-Path $srcFake 'policy_plugin.dll'

if (-not (Test-Path -LiteralPath $cap)) { throw "missing: $cap" }
if (-not (Test-Path -LiteralPath $pol)) { throw "missing: $pol" }

Copy-Item -LiteralPath $cap -Destination (Join-Path $dst 'capture_plugin.dll')
Copy-Item -LiteralPath $pol -Destination (Join-Path $dst 'policy_plugin.dll')

Write-Output "[fake_scene] dir=$dst"
Get-ChildItem -LiteralPath $dst | Select-Object Name, Length | Format-Table -AutoSize | Out-String | Write-Output
Write-Output 'EXIT=0'
