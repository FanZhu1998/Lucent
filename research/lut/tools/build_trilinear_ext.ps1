<#
.SYNOPSIS
    Builds Image-Adaptive-3DLUT's CPU trilinear extension (trilinear_cpp, unmodified) with MSVC.

.DESCRIPTION
    Research only (blueprint Appendix D2 step 2). Runs the repository's own
    trilinear_cpp/setup.py, which selects CppExtension('trilinear', ['src/trilinear.cpp']) when
    CUDA is unavailable, against the pinned research\lut\.venv. The build is out of tree:
    objects and the module go to research\lut\build\trilinear_ext (gitignored) and the pinned
    checkout is verified to be unchanged afterwards.

    Produces research\lut\build\trilinear_ext\lib\trilinear.cp312-win_amd64.pyd and build.log
    (the exact compiler command lines; research\lut\tools\trilinear_parity.py records them).

    Requires Visual Studio 2022 Build Tools (MSVC x64) and Ninja, loaded through tools\dev-env.ps1.
    Compatible with Windows PowerShell 5.1 and PowerShell 7.
#>
[CmdletBinding()]
param(
    [string]$Checkout = (Join-Path $PSScriptRoot '..\..\third_party\Image-Adaptive-3DLUT'),
    [string]$OutDir = (Join-Path $PSScriptRoot '..\build\trilinear_ext')
)

$ErrorActionPreference = 'Stop'
$pinnedCommit = 'b491f6df64a588864739a157db271e5c848e1805'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$python = (Resolve-Path (Join-Path $PSScriptRoot '..\.venv\Scripts\python.exe')).Path
$Checkout = (Resolve-Path $Checkout).Path

$head = (& git -C $Checkout rev-parse HEAD).Trim()
if ($head -ne $pinnedCommit) { throw "The checkout is at $head, expected $pinnedCommit." }
$before = & git -C $Checkout status --porcelain --ignored
if ($before) { throw "The checkout has local changes before the build:`n$($before -join "`n")" }

# MSVC x64 developer environment (same loader as the product build). torch.utils.cpp_extension
# refuses an already-activated VC environment unless DISTUTILS_USE_SDK is set.
. (Join-Path $repoRoot 'tools\dev-env.ps1')
$env:DISTUTILS_USE_SDK = '1'

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path
$log = Join-Path $OutDir 'build.log'

Push-Location (Join-Path $Checkout 'trilinear_cpp')
try {
    # Windows PowerShell 5.1 turns native stderr lines into error records; collect them as text.
    $ErrorActionPreference = 'Continue'
    & $python -X utf8 setup.py build_ext --force --build-lib (Join-Path $OutDir 'lib') `
        --build-temp (Join-Path $OutDir 'temp') 2>&1 |
        ForEach-Object { if ($_ -is [System.Management.Automation.ErrorRecord]) { $_.Exception.Message } else { "$_" } } |
        Set-Content -Path $log -Encoding utf8
    $exitCode = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
} finally {
    Pop-Location
}
if ($exitCode -ne 0) { throw "setup.py build_ext failed with exit code $exitCode; see $log" }

$after = & git -C $Checkout status --porcelain --ignored
if ($after) { throw "The build modified the checkout:`n$($after -join "`n")" }

$module = Get-ChildItem -Path (Join-Path $OutDir 'lib') -Filter 'trilinear*.pyd' | Select-Object -First 1
if (-not $module) { throw "No trilinear*.pyd was produced; see $log" }
Write-Host "Built $($module.FullName) ($($module.Length) bytes); log: $log"
