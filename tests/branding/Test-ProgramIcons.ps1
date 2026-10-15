<#
.SYNOPSIS
    Checks that the built programs carry Lucent's icon and version information (ADR 0025).

.DESCRIPTION
    For each program (lucent-worker.exe, and Lucent.exe when the build has the user interface), read as a
    data file with LoadLibraryEx (nothing of it runs):
    - its first and only icon group (RT_GROUP_ICON) is IDI_ICON1, the name Qt's Windows platform plugin
      loads for the window class, and the group Explorer, shortcuts, the taskbar and "<program>,0"
      references (Setup's UninstallDisplayIcon, the "Open with" DefaultIcon) show;
    - that group holds exactly the images of the branding asset's Windows icon
      (assets\branding\windows\lucent.ico), image for image and byte for byte (Test-ProgramIcon);
    - its version information names Lucent: ProductName and CompanyName "Lucent", a FileDescription,
      the file's own name as OriginalFilename, the build's version as FileVersion and at the start of
      ProductVersion, and Lucent's MIT copyright.
    Then the review evidence of the programs' icons (New-ProgramIconSheet) is drawn into a temporary PNG.

    Registered with CTest as lucent_program_icon_tests (label "branding") with the build tree's programs.
    Exit code 0 when every check passed. Windows PowerShell 5.1 compatible. Keep this file ASCII.

.PARAMETER Worker
    The built lucent-worker.exe.
.PARAMETER Application
    The built Lucent.exe (optional: a build without the user interface has none).
.PARAMETER Version
    The build's version (the CMake project version), for example 0.1.0.
.PARAMETER Root
    The branding asset folder (default: assets\branding of this repository).

.EXAMPLE
    powershell -NoProfile -File tests\branding\Test-ProgramIcons.ps1 -Worker build\dev\bin\lucent-worker.exe -Application build\dev\bin\Lucent.exe -Version 0.1.0
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Worker,
    [string]$Application,
    [Parameter(Mandatory)][string]$Version,
    [string]$Root
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Import-Module (Join-Path $repoRoot 'tools\branding\LucentBranding.psm1') -Force
if (-not $Root) { $Root = Join-Path $repoRoot 'assets\branding' }

$script:checks = 0
$script:failures = 0
function Assert-True([bool]$Condition, [string]$What) {
    $script:checks++
    if (-not $Condition) { $script:failures++; Write-Host "FAILED: $What" }
}

$asset = Read-BrandingAsset -Root $Root
$icon = @(Get-BrandingFiles -Asset $asset -Role 'windows-icon')[0]
$programs = @($Worker) + @(if ($Application) { $Application })
foreach ($program in $programs) {
    $path = [System.IO.Path]::GetFullPath($program)
    $name = Split-Path -Leaf $path
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { Assert-True $false "$path exists (build it first)"; continue }

    $result = Test-ProgramIcon -Path $path -Icon $icon -GroupName 'IDI_ICON1' -First
    Assert-True ($result.Problems.Count -eq 0) "$name carries the branding icon as its first icon group, IDI_ICON1 ($($result.Problems -join ' | '))"
    Assert-True (@($result.Groups).Count -eq 1) "$name has exactly one icon group ($((@($result.Groups) | ForEach-Object Name) -join ', '))"
    if ($result.Group) {
        Write-Host ("{0}: icon group {1} with {2} images ({3}), {4}" -f $name, $result.Group.Name, $result.Group.Images.Count,
            ((@($result.Group.Images) | ForEach-Object { "$($_.Width) px $(if ($_.IsPng) { 'PNG' } else { 'BMP' })" }) -join ', '), $result.Match)
    }

    $info = [System.Diagnostics.FileVersionInfo]::GetVersionInfo($path)
    Assert-True ($info.ProductName -ceq 'Lucent') "$name's ProductName is Lucent ('$($info.ProductName)')"
    Assert-True ($info.CompanyName -ceq 'Lucent') "$name's CompanyName is Lucent ('$($info.CompanyName)')"
    Assert-True ([bool]$info.FileDescription) "$name has a FileDescription"
    Assert-True ($info.OriginalFilename -ceq $name) "$name's OriginalFilename is its own name ('$($info.OriginalFilename)')"
    Assert-True ($info.FileVersion -ceq $Version) "$name's FileVersion is $Version ('$($info.FileVersion)')"
    Assert-True ($info.ProductVersion -and $info.ProductVersion.StartsWith("$Version ")) "$name's ProductVersion starts with $Version ('$($info.ProductVersion)')"
    Assert-True ($info.LegalCopyright -like '*Fan Zhu*MIT*') "$name's LegalCopyright is Lucent's ('$($info.LegalCopyright)')"
    Write-Host "${name}: $($info.FileDescription), $($info.ProductVersion)"
}

# The review evidence (LucentBrandingEvidence.cs, compiled only when it is drawn) compiles and draws
# these programs' icons as Windows draws them.
$sheet = Join-Path ([System.IO.Path]::GetTempPath()) ('lucent-program-icons-' + [guid]::NewGuid().ToString('N') + '.png')
try {
    New-ProgramIconSheet -Executables @($programs | ForEach-Object { [System.IO.Path]::GetFullPath($_) }) -Path $sheet
    $sheetBytes = [System.IO.File]::ReadAllBytes($sheet)
    Assert-True ($sheetBytes.Length -gt 8 -and $sheetBytes[0] -eq 137 -and $sheetBytes[1] -eq 80 -and $sheetBytes[2] -eq 78 -and $sheetBytes[3] -eq 71) "the program icon sheet is written as a PNG ($($sheetBytes.Length) bytes)"
}
catch { Assert-True $false "the program icon sheet is drawn ($($_.Exception.Message))" }
finally { Remove-Item -LiteralPath $sheet -Force -ErrorAction SilentlyContinue }

Write-Host "$($script:checks) checks, $($script:failures) failed"
if ($script:failures -gt 0) { exit 1 }
exit 0
