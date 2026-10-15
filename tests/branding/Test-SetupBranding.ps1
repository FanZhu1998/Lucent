<#
.SYNOPSIS
    Compiles installer\lucent.iss with Lucent's branding and checks the Setup it makes (ADR 0025).

.DESCRIPTION
    A packaging check that needs no release build. installer\lucent.iss is compiled by Inno Setup's
    ISCC (-InnoSetupCompiler, or found as tools\package.ps1 finds it; without one the test is skipped
    with exit code 77) into a temporary folder, with the defines tools\package.ps1 passes: the branding
    asset's Windows icon and wizard images by role (assets\branding, verified against its manifest), and
    stand-ins of a few bytes for the installation folder and Microsoft's runtime installer. Then:
    1. Setup's only icon group, the one Windows shows for Setup and its uninstaller, is the branding
       asset's Windows icon, image for image (Test-ProgramIcon).
    2. Every wizard image and small wizard image is inside Setup's setup data byte for byte (the setup
       data decompressed with installer\scripts\LucentCompressedContent.cs).
    3. Without the branding defines the script does not compile: it names the missing define.

    Registered with CTest as lucent_setup_branding_tests (labels "branding" and "installer"). Exit code
    0 when every check passed, 77 when no Inno Setup compiler was found. Windows PowerShell 5.1
    compatible. Keep this file ASCII.

.PARAMETER InnoSetupCompiler
    ISCC.exe of Inno Setup 6.3 or later (default: Find-InnoSetupCompiler, as tools\package.ps1).
.PARAMETER Root
    The branding asset folder (default: assets\branding of this repository).

.EXAMPLE
    powershell -NoProfile -File tests\branding\Test-SetupBranding.ps1
#>
[CmdletBinding()]
param([string]$InnoSetupCompiler, [string]$Root)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Import-Module (Join-Path $repoRoot 'installer\scripts\LucentPackaging.psm1') -Force
Import-Module (Join-Path $repoRoot 'tools\branding\LucentBranding.psm1') -Force
if (-not $Root) { $Root = Join-Path $repoRoot 'assets\branding' }

$script:checks = 0
$script:failures = 0
function Assert-True([bool]$Condition, [string]$What) {
    $script:checks++
    if (-not $Condition) { $script:failures++; Write-Host "FAILED: $What" }
}

$iscc = $InnoSetupCompiler
if (-not $iscc) {
    try { $iscc = (Find-InnoSetupCompiler).Path } catch { $iscc = $null }
}
if (-not $iscc) {
    Write-Host '[SKIP] No Inno Setup compiler (ISCC.exe) was found: pass -InnoSetupCompiler or install Inno Setup 6.3 or later.'
    exit 77
}
Write-Host "Inno Setup compiler: $iscc"

$asset = Read-BrandingAsset -Root $Root
$icon = @(Get-BrandingFiles -Asset $asset -Role 'windows-icon')[0]
$wizardImages = @(Get-BrandingFiles -Asset $asset -Role 'setup-wizard-image')
$wizardSmallImages = @(Get-BrandingFiles -Asset $asset -Role 'setup-wizard-small-image')

$work = Join-Path ([System.IO.Path]::GetTempPath()) ('lucent-setup-branding-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path (Join-Path $work 'stage') -Force | Out-Null
try {
    # Stand-ins: the compiler only stores these files.
    [System.IO.File]::WriteAllText((Join-Path $work 'stage\readme.txt'), "A stand-in of Lucent's installation folder.`r`n")
    $redist = Join-Path $work 'vc_redist.x64.exe'
    [System.IO.File]::WriteAllText($redist, 'A stand-in of the runtime installer.')
    $associations = Join-Path $work 'associations.iss'
    [System.IO.File]::WriteAllText($associations, "; No file associations in this test.`r`n")
    $common = @(
        '/DAppVersion=0.0.1', '/DAppVersionQuad=0.0.1.0', "/DStagingDir=$(Join-Path $work 'stage')", "/DVCRedistFile=$redist",
        '/DVCRedistMajor=14', '/DVCRedistMinor=44', '/DVCRedistBuild=35211', '/DVCRuntimeDlls=msvcp140.dll,vcruntime140.dll',
        "/DAssociationsInclude=$associations")
    $branding = @("/DBrandingIcon=$icon", "/DBrandingWizardImages=$($wizardImages -join ',')", "/DBrandingWizardSmallImages=$($wizardSmallImages -join ',')")
    $script = Join-Path $repoRoot 'installer\lucent.iss'

    # 1. Setup's icon.
    $compile = Invoke-NativeCommand -FilePath $iscc -Arguments ($common + $branding + @("/O$(Join-Path $work 'out')", '/Fbranded', $script))
    $setup = Join-Path $work 'out\branded.exe'
    Assert-True ($compile.ExitCode -eq 0 -and (Test-Path -LiteralPath $setup -PathType Leaf)) "installer\lucent.iss compiles with the branding defines (exit code $($compile.ExitCode): $(@($compile.Output | Select-Object -Last 3) -join ' '))"
    if (Test-Path -LiteralPath $setup -PathType Leaf) {
        $result = Test-ProgramIcon -Path $setup -Icon $icon -First
        Assert-True ($result.Problems.Count -eq 0) "Setup's first icon group is the branding asset's Windows icon ($($result.Problems -join ' | '))"
        Assert-True (@($result.Groups).Count -eq 1) "Setup has one icon group ($((@($result.Groups) | ForEach-Object Name) -join ', '))"
        if ($result.Group) { Write-Host "Setup's icon: group $($result.Group.Name), $($result.Group.Images.Count) images, $($result.Match)" }

        # 2. The wizard images, byte for byte in the decompressed setup data.
        Initialize-CompressedContentType
        $latin1 = [System.Text.Encoding]::GetEncoding(28591)
        $setupData = @([LucentPackaging.InnoSetupContent]::Extract([System.IO.File]::ReadAllBytes($setup), 1GB) |
            Where-Object { $_.Kind -like 'Inno Setup setup data*' } | ForEach-Object { $latin1.GetString($_.Data) })
        Assert-True ($setupData.Count -gt 0) 'Setup has setup data the reader decompresses'
        foreach ($image in $wizardImages + $wizardSmallImages) {
            $needle = $latin1.GetString([System.IO.File]::ReadAllBytes($image))
            $found = @($setupData | Where-Object { $_.IndexOf($needle, [System.StringComparison]::Ordinal) -ge 0 }).Count -gt 0
            Assert-True $found "$(Split-Path -Leaf $image) is in Setup's setup data byte for byte"
        }
    }

    # 3. Without the branding defines, no Setup.
    $unbranded = Invoke-NativeCommand -FilePath $iscc -Arguments ($common + @("/O$(Join-Path $work 'out')", '/Funbranded', $script))
    Assert-True ($unbranded.ExitCode -ne 0 -and -not (Test-Path -LiteralPath (Join-Path $work 'out\unbranded.exe'))) "installer\lucent.iss does not compile without the branding defines (exit code $($unbranded.ExitCode))"
    Assert-True (@($unbranded.Output | Where-Object { $_ -match 'BrandingIcon is not defined' }).Count -gt 0) 'and says which define is missing'
}
finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "$($script:checks) checks, $($script:failures) failed"
if ($script:failures -gt 0) { exit 1 }
exit 0
