<#
.SYNOPSIS
    Configures, builds and optionally tests Lucent using a CMake preset.

.EXAMPLE
    .\tools\build.ps1                         # configure + build the "dev-core" preset
    .\tools\build.ps1 -Preset dev -Test       # full app build including the Qt UI, then run tests
    .\tools\build.ps1 -Target lucent_core_tests -Test -TestRegex Core
    .\tools\build.ps1 -Preset dev-core -Jobs 2 -BuildDir build/agent-raw

.NOTES
    -BuildDir lets several builds of the same preset coexist (for example parallel work
    in separate checkouts). -Jobs limits compiler parallelism; the default is conservative
    because each MSVC process can use close to 1 GB of RAM.
#>
[CmdletBinding()]
param(
    [ValidateSet('dev-core', 'dev', 'release')]
    [string]$Preset = 'dev-core',
    [string]$Target,
    [switch]$Test,
    [string]$TestRegex,
    [switch]$ConfigureOnly,
    [switch]$Fresh,
    [int]$Jobs = 4,
    [string]$BuildDir
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'dev-env.ps1')

Push-Location $repoRoot
try {
    $configureArgs = @('--preset', $Preset)
    if ($Fresh) { $configureArgs += '--fresh' }
    if ($BuildDir) { $configureArgs += @('-B', $BuildDir) }
    & cmake @configureArgs
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed ($LASTEXITCODE)" }
    if ($ConfigureOnly) { return }

    $binaryDir = if ($BuildDir) { $BuildDir } else { "build/$Preset" }
    $buildArgs = @('--build', $binaryDir, '--parallel', $Jobs)
    if ($Target) { $buildArgs += @('--target', $Target) }
    & cmake @buildArgs
    if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)" }

    if ($Test) {
        $ctestArgs = @('--test-dir', $binaryDir, '--output-on-failure', '--parallel', $Jobs)
        if ($TestRegex) { $ctestArgs += @('-R', $TestRegex) }
        & ctest @ctestArgs
        if ($LASTEXITCODE -ne 0) { throw "Tests failed ($LASTEXITCODE)" }
    }
}
finally {
    Pop-Location
}
