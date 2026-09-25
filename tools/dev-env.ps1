<#
.SYNOPSIS
    Loads the Lucent developer environment into the current PowerShell session.

.DESCRIPTION
    Dot-source this script (". .\tools\dev-env.ps1") before configuring or building.
    It enters the Visual Studio x64 developer environment (MSVC, Windows SDK), points
    VCPKG_ROOT at Lucent's pinned vcpkg checkout, and puts the pinned CMake/Ninja first on PATH.

    Locations can be overridden with environment variables:
      LUCENT_DEV_ROOT     root of developer tools   (default: %USERPROFILE%\dev)
      LUCENT_VCPKG_ROOT   pinned vcpkg checkout     (default: $LUCENT_DEV_ROOT\vcpkg)
      LUCENT_TOOLS_VENV   Python venv with cmake    (default: $LUCENT_DEV_ROOT\toolsenv)

    A generic VCPKG_ROOT is deliberately ignored: the Visual Studio developer shell sets it
    to the vcpkg copy bundled with Visual Studio, whose helper scripts differ from the pinned
    checkout and would silently change every package's ABI hash (defeating the binary cache
    and reproducibility).

    Compatible with Windows PowerShell 5.1 and PowerShell 7.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'

if ($env:LUCENT_DEV_ENV_LOADED -eq '1') { return }

$devRoot = if ($env:LUCENT_DEV_ROOT) { $env:LUCENT_DEV_ROOT } else { Join-Path $env:USERPROFILE 'dev' }
$lucentVcpkgRoot = if ($env:LUCENT_VCPKG_ROOT) { $env:LUCENT_VCPKG_ROOT } else { Join-Path $devRoot 'vcpkg' }
$toolsVenv = if ($env:LUCENT_TOOLS_VENV) { $env:LUCENT_TOOLS_VENV } else { Join-Path $devRoot 'toolsenv' }

# --- Visual Studio developer environment -------------------------------------------
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found. Install Visual Studio 2022 Build Tools (see docs/development/setup.md)." }
$vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsPath) { throw 'No Visual Studio installation with the MSVC x64 toolset was found.' }

$devShellDll = Join-Path $vsPath 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll'
Import-Module $devShellDll
Enter-VsDevShell -VsInstallPath $vsPath -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64 -no_logo' | Out-Null

# Must follow Enter-VsDevShell, which points VCPKG_ROOT at Visual Studio's bundled vcpkg.
$env:VCPKG_ROOT = $lucentVcpkgRoot

# --- Pinned CMake / Ninja take precedence over the copies bundled with Visual Studio ---
$prepend = @()
$venvScripts = Join-Path $toolsVenv 'Scripts'
if (Test-Path (Join-Path $venvScripts 'cmake.exe')) { $prepend += $venvScripts }
$wingetLinks = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Links'
if (Test-Path (Join-Path $wingetLinks 'ninja.exe')) { $prepend += $wingetLinks }
if ($prepend.Count -gt 0) { $env:PATH = (($prepend + @($env:PATH)) -join ';') }

if (-not (Test-Path (Join-Path $env:VCPKG_ROOT 'vcpkg.exe'))) {
    throw "vcpkg not found at $env:VCPKG_ROOT. Run tools\bootstrap-dev.ps1 first."
}

$env:VCPKG_DISABLE_METRICS = '1'
$env:LUCENT_DEV_ENV_LOADED = '1'

Write-Host ("Lucent dev environment: MSVC {0}, {1}, vcpkg at {2}" -f $env:VCToolsVersion, ((& cmake --version) | Select-Object -First 1), $env:VCPKG_ROOT)
