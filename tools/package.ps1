<#
.SYNOPSIS
    Builds the Lucent installer, Lucent-Setup-<version>-x64.exe (docs/adr/0009-windows-packaging.md).

.DESCRIPTION
    1. Builds the release preset with tools\build.ps1 (optionally from scratch, optionally with tests).
       Outputs of an earlier run for the same version (installer, checksum, manifest, SBOM copy) are
       then deleted, so they can never be paired with a different layout.
    2. Lays out the installation folder with `cmake --install`: Lucent.exe, lucent-worker.exe (the
       isolated export worker it starts, W7 / ADR 0012), Qt deployed by windeployqt,
       every other runtime DLL (Lensfun and the GLib libraries it uses among them, W13 / ADR 0023),
       an installed-layout qt.conf and the bundled data assets (assets\<id>: the camera support
       database, W10 / ADR 0018; the Lensfun lens database, W13 / ADR 0023)
       (cmake/LucentPackaging.cmake).
    3. Audits that folder with dumpbin: every import of every DLL and executable must resolve to the
       folder, to Windows or to the Visual C++ runtime installed by Microsoft's redistributable, every
       DLL in the folder must be imported, the required Qt plugins and every QML module Lucent uses
       (with its plugin DLL) must be present, and every file must be Lucent's build output or an
       unmodified file of a vcpkg port; the only executables are Lucent.exe and lucent-worker.exe,
       both GUI-subsystem programs identical to the build. Every bundled asset is verified as
       Lucent.exe verifies it: its manifest has the SHA-256 the build pinned, and every listed file
       its listed size and SHA-256, with nothing unlisted beside them. Then it starts the staged
       Lucent.exe with a Windows-only PATH (smoke run): Qt loads plugins and QML modules by name,
       which no import table shows, and Lucent must report the camera support database of the
       folder as active and the update channel it checks, which must be the one the build tree
       recorded (W12 review SEC-02; see -UpdateChannel). Any error stops the packaging.
       Lucent.exe and lucent-worker.exe must carry Lucent's logo (ADR 0025): their first and only icon
       group is IDI_ICON1 with exactly the images of the branding asset's Windows icon
       (assets\branding, verified against its manifest first).
    4. Signs Lucent.exe and lucent-worker.exe when signing is configured (see NOTES; off by default),
       and verifies each signature: the configured certificate's thumbprint, an offline
       WinVerifyTrust result, the timestamp.
    5. Writes licenses\: Lucent's licence, the copyright file of every shipped vcpkg port, licence
       texts vcpkg lacks, the Qt LGPL-3.0 and GPL-3.0 texts, the licence text of every third-party data
       asset (the lens database's CC BY-SA 3.0, installer\license-policy.json "assets"),
       THIRD-PARTY-NOTICES.txt (including the third-party code inside Qt from Qt's own SBOM and the
       libraries linked statically into Lucent's executables).
       Then the corresponding source (W12, ADR 0024): Lucent-<version>-corresponding-source.zip beside
       the installer, holding for every shipped library whose licence needs a source provision (the
       LGPL libraries; installer\license-policy.json "lgpl"/"correspondingSource") the upstream
       source archives from vcpkg's downloads folder (SHA-512 as vcpkg verified it and as the recipe
       pins it at the registry baseline), the vcpkg recipe as built (registry port or Lucent's overlay
       port, every file with the SHA-256 vcpkg recorded), vcpkg's build record, the triplet, Lucent's
       vcpkg manifest and a README on rebuilding and relinking. The archive is deterministic (sorted,
       stored entries with a fixed time) and is checked again by reading it back
       (Test-CorrespondingSourcePackage). Packaging stops when a shipped port's licence leaves no
       choice but a copyleft licence and the policy provides no source for it, or when a provided port
       is missing from the archive. LGPL-NOTICE.txt then names the archive with its size and SHA-256.
       Last come Lucent-files.txt (the installed-file list that the next version's Setup uses to
       upgrade) and the SBOM Lucent.spdx.json (SPDX 2.3, checked structurally, and with pyspdxtools
       when it is available).
    6. Scans every byte of every file of the folder for the build machine's folders (W12): the user
       profile, the repository, the build tree, the vcpkg root, installed tree, buildtrees, packages
       and downloads folders, and any X:\Users\<name>\ path, in 8-bit and UTF-16 text, either
       separator, any letter case; and what the files compress (W12 review SEC-07): every zlib and
       gzip stream (Qt's rcc resources, Qt's MIME database), decompressed and scanned, two levels
       deep; a zstd frame, which the scan cannot decompress, is an error. One hit (a PDB path in a PE
       debug directory, a __FILE__ string, a path in a text file or a compressed resource) stops the
       packaging, naming the file, the compressed part and the byte offset (builder-paths.txt).
       Setup (with its LZMA-compressed setup data, setup engine and installed files decompressed),
       the package manifest and the corresponding-source archive's own files (README, MANIFEST.json,
       recipes, triplet, build records; the upstream archives are verified by SHA-512 instead) are
       scanned the same way.
    7. Compiles installer\lucent.iss with Inno Setup 6 (unless -StageOnly; Setup and the uninstaller
       are signed when signing is configured, and Setup's signature is verified; the compiler's
       licence line is recorded (Get-InnoSetupLicence: the whole banner), and a distribution the
       policy calls commercial needs a compiler whose banner states a commercial licence: a
       non-commercial or unrecognised one is refused), with the branding asset's Windows icon as
       Setup's icon and its wizard images (ADR 0025; Setup's first icon group must then be that icon,
       image for image), and writes the package manifest (schema 5: version, git commit,
       signing, the distribution decision, the update channel, every file with its SHA-256, the
       corresponding-source archive with every file in it, the installer's size and SHA-256, or no
       installer with -StageOnly).

    The update channel (W12 review SEC-02; ADR 0022): the channel compiled into Lucent.exe (the
    LUCENT_UPDATE_* values of the build's first configure) is printed, checked against what the
    staged Lucent.exe reports and recorded in the package manifest. A Setup must declare it with
    -UpdateChannel: 'none' for a build that checks no channel, or the name of an entry of
    installer\update-channels.json, which the build must match exactly (manifest URL, release-
    manifest key ids, publisher certificate thumbprints; the entry must name at least one).

    Outputs go to <BuildDir>\package unless -OutputDir is given:
      stage\                                  the installation folder that Setup installs
      Lucent-Setup-<version>-x64.exe           the installer
      Lucent-<version>-corresponding-source.zip the corresponding source of the LGPL libraries,
                                               published with the installer (ADR 0024)
      Lucent-Setup-<version>-x64.manifest.json package manifest
      Lucent-Setup-<version>-x64.spdx.json     copy of the SBOM
      Lucent-Setup-<version>-x64.exe.sha256    checksum line
      audit.txt, audit.json, builder-paths.txt, builder-paths.json, install.log, smoke\ (the smoke
      run), iscc.log (and windeployqt.log in <BuildDir>\packaging)
      rehearsal\                              with -VersionRehearsal only: a test installer of the
                                               next patch version (never published)

    What a release publishes, and the gates before it, are in docs\release\checklist.md.

.PARAMETER BuildDir
    Release build tree (default build\release).
.PARAMETER Jobs
    Parallel compile jobs passed to tools\build.ps1 (default 4).
.PARAMETER Fresh
    Reconfigure the build tree from scratch.
.PARAMETER RunTests
    Build every target and run the tests before packaging (otherwise only Lucent.exe is built).
.PARAMETER SkipBuild
    Package the existing build tree without building.
.PARAMETER StageOnly
    Stop after the folder, audit, smoke run, licences and SBOM; do not compile the installer. The
    manifest then records no installer.
.PARAMETER OutputDir
    Where outputs go (default <BuildDir>\package).
.PARAMETER Iscc
    Path of ISCC.exe; it must be valid (no fallback). Otherwise LUCENT_ISCC (also strict), PATH, the
    uninstall registry and the standard Inno Setup folders are searched.
.PARAMETER VCRedist
    Path of Microsoft's vc_redist.x64.exe (default: the one the build recorded from Visual Studio).
.PARAMETER SpdxValidator
    Path of pyspdxtools.exe (spdx-tools) to validate the SBOM. Default: LUCENT_PYSPDXTOOLS, then
    pyspdxtools on PATH; without one, only the built-in structural check runs.
.PARAMETER UpdateChannel
    The update channel the package's Lucent.exe must check: 'none', or the name of an entry of
    installer\update-channels.json (-UpdateChannels). Required unless -StageOnly.
.PARAMETER UpdateChannels
    The expected update channels (default installer\update-channels.json); a rehearsal of a test
    channel names its own file.
.PARAMETER VersionRehearsal
    Also compile <OutputDir>\rehearsal\Lucent-Setup-<next patch>-x64-VERSION-REHEARSAL.exe: the same
    installation folder registered as the next patch version (W12 review SEC-04, part 2), for
    installer\tests\Test-InstalledPackage.ps1 -NewerInstaller, which upgrades to it and checks that
    the package's Setup refuses to go back. A test installer: unsigned, not in the package manifest,
    never published. Not with -StageOnly.

.EXAMPLE
    powershell -NoProfile -File tools\package.ps1 -UpdateChannel none
.EXAMPLE
    powershell -NoProfile -File tools\package.ps1 -Fresh -RunTests -Jobs 2 -UpdateChannel stable
.EXAMPLE
    powershell -NoProfile -File tools\package.ps1 -SkipBuild -StageOnly

.NOTES
    Lucent is distributed free under its MIT licence (installer\license-policy.json "distribution",
    ADR 0024): Inno Setup's non-commercial terms apply, and releases are unsigned until a
    code-signing certificate exists.

    Authenticode signing is off by default, and builds made so far are unsigned. To sign Lucent.exe,
    lucent-worker.exe, Setup and the uninstaller, set before running:
      LUCENT_SIGN_CERT_SHA1      thumbprint of a code-signing certificate with its private key in the
                                 current user's personal store (Cert:\CurrentUser\My), checked
                                 before the build: present, valid now, usable for code signing
      LUCENT_SIGN_TIMESTAMP_URL  RFC 3161 timestamp server of the certificate's issuer (required)
      LUCENT_SIGNTOOL            optional path of signtool.exe (default: signtool.exe on PATH, which
                                 the Visual Studio environment provides)
      LUCENT_SIGN_TEST=1         a test signature (a self-signed certificate only, for rehearsing
                                 the signing path): the one verification failure accepted is the
                                 untrusted root, LUCENT_SIGN_TIMESTAMP_URL may be empty (no
                                 timestamp: the signature then expires with the certificate), the
                                 outputs are named Lucent-Setup-<version>-x64-TEST-SIGNED.* and the
                                 manifest records signing.test = true. Never for distribution:
                                 New-LucentReleaseManifest (installer\scripts\LucentUpdate.psm1)
                                 refuses such a package (W12 review SEC-03).
    Every signature made is verified: signed by that thumbprint, verifying offline with
    WinVerifyTrust (digest and chain, no revocation check), timestamped when a timestamp server is
    set. The uninstaller is signed by Inno Setup inside Setup; installer\tests\Test-InstalledPackage.ps1
    -SignerThumbprint checks it after installing. Keys and certificates never live in the repository.

    Windows PowerShell 5.1 compatible. Keep this file ASCII.
#>
[CmdletBinding()]
param(
    [string]$BuildDir = 'build\release',
    [ValidateRange(1, 64)]
    [int]$Jobs = 4,
    [switch]$Fresh,
    [switch]$RunTests,
    [switch]$SkipBuild,
    [switch]$StageOnly,
    [string]$OutputDir,
    [string]$Iscc,
    [string]$VCRedist,
    [string]$SpdxValidator,
    [string]$UpdateChannel,
    [string]$UpdateChannels,
    [switch]$VersionRehearsal
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0
$repoRoot = Split-Path -Parent $PSScriptRoot
$installerDir = Join-Path $repoRoot 'installer'
Import-Module (Join-Path $installerDir 'scripts\LucentPackaging.psm1') -Force
Import-Module (Join-Path $PSScriptRoot 'branding\LucentBranding.psm1') -Force

function Write-Step([string]$Text) { Write-Host "==> $Text" -ForegroundColor Cyan }

function Resolve-RepoPath([string]$Path) {
    # Full path without a trailing separator (a trailing backslash breaks native argument quoting).
    $full = if ([System.IO.Path]::IsPathRooted($Path)) { [System.IO.Path]::GetFullPath($Path) } else { [System.IO.Path]::GetFullPath((Join-Path $repoRoot $Path)) }
    if ($full.Length -gt 3) { $full = $full.TrimEnd('\', '/') }
    return $full
}

if ($SkipBuild -and ($Fresh -or $RunTests)) { throw '-SkipBuild cannot be combined with -Fresh or -RunTests.' }
if ($VersionRehearsal -and $StageOnly) { throw '-VersionRehearsal compiles a second Setup: it cannot be combined with -StageOnly.' }
# Checked again against the build below; a Setup must say which update channel it checks (SEC-02).
if (-not $StageOnly -and -not $UpdateChannel) {
    throw 'Pass -UpdateChannel <name> (an entry of installer\update-channels.json) or -UpdateChannel none: a Setup declares which update channel its Lucent.exe checks.'
}
if ($UpdateChannel -and $UpdateChannel -ne 'none' -and $UpdateChannel -cnotmatch '^[a-z0-9]+(-[a-z0-9]+)*$') { throw "-UpdateChannel '$UpdateChannel' is not a channel name or 'none'." }
$buildPath = Resolve-RepoPath $BuildDir
if (-not $OutputDir) { $OutputDir = Join-Path $buildPath 'package' }
$OutputDir = Resolve-RepoPath $OutputDir
$stage = Join-Path $OutputDir 'stage'
$started = Get-Date

# Signing configuration, checked before the slow steps (see NOTES): the certificate must be usable,
# and a release signature needs a timestamp and a certificate that is not self-signed.
$signing = $null
if ($env:LUCENT_SIGN_CERT_SHA1) {
    $certificate = Get-CodeSigningCertificate -Thumbprint $env:LUCENT_SIGN_CERT_SHA1
    $testSigning = ($env:LUCENT_SIGN_TEST -eq '1')
    $timestampUrl = if ($env:LUCENT_SIGN_TIMESTAMP_URL) { $env:LUCENT_SIGN_TIMESTAMP_URL.Trim() } else { '' }
    if ($timestampUrl -and $timestampUrl -notmatch '^https?://[^\s"]+$') { throw "LUCENT_SIGN_TIMESTAMP_URL '$timestampUrl' is not an http(s) URL." }
    if (-not $testSigning -and -not $timestampUrl) {
        throw 'LUCENT_SIGN_TIMESTAMP_URL must be set when LUCENT_SIGN_CERT_SHA1 is: a signature without a timestamp stops verifying when the certificate expires. (LUCENT_SIGN_TEST=1 makes an untimestamped test signature.)'
    }
    if (-not $testSigning -and $certificate.SelfSigned) {
        throw "The certificate $($certificate.Thumbprint) ($($certificate.Subject)) is self-signed: a test certificate. Release signatures need a certificate from a code-signing authority; set LUCENT_SIGN_TEST=1 to rehearse signing with it."
    }
    # A test signature accepts an untrusted root; only for a certificate that is its own root (SEC-03).
    if ($testSigning -and -not $certificate.SelfSigned) {
        throw "LUCENT_SIGN_TEST=1 rehearses signing with a self-signed test certificate only; $($certificate.Thumbprint) ($($certificate.Subject)) was issued by $($certificate.Issuer). Sign without LUCENT_SIGN_TEST with a certificate from a code-signing authority."
    }
    $signing = [pscustomobject]@{ Certificate = $certificate; Test = $testSigning; TimestampUrl = $timestampUrl; SignTool = $null; Arguments = @() }
    Write-Host ("Signing with {0} ({1}, valid until {2:yyyy-MM-dd}){3}" -f $certificate.Thumbprint, $certificate.Subject, $certificate.NotAfter,
        $(if ($testSigning) { ': TEST SIGNATURE, not for distribution' } else { '' }))
}

# --- 1. Build ------------------------------------------------------------------------------------

if (-not $SkipBuild) {
    Write-Step "Building the release preset in $buildPath"
    $buildParameters = @{ Preset = 'release'; BuildDir = $buildPath; Jobs = $Jobs }
    if ($Fresh) { $buildParameters.Fresh = $true }
    if ($RunTests) { $buildParameters.Test = $true } else { $buildParameters.Target = 'Lucent' }
    # In this process, so parameters need no command-line quoting; build.ps1 throws on failure.
    Set-StrictMode -Off
    try { & (Join-Path $PSScriptRoot 'build.ps1') @buildParameters }
    finally { Set-StrictMode -Version 3.0 }
}

# The MSVC environment (cmake, signtool) and VCPKG_ROOT for the source archives.
Set-StrictMode -Off
. (Join-Path $PSScriptRoot 'dev-env.ps1')
Set-StrictMode -Version 3.0

$info = Read-LucentPackageInfo -BuildDir $buildPath
if ($info.buildType -ne 'Release') { throw "$buildPath is a $($info.buildType) build; package the release preset." }

# The update channel compiled into Lucent.exe (W12 review SEC-02): shown, checked against the declared
# and expected channels here, against the staged Lucent.exe's own report after the smoke run, and
# recorded in the package manifest.
$channelsPath = if ($UpdateChannels) { Resolve-RepoPath $UpdateChannels } else { Join-Path $installerDir 'update-channels.json' }
$expectedChannels = @(Read-ExpectedUpdateChannels -Path $channelsPath)
$compiledChannel = Get-LucentUpdateChannelRecord -Info $info
Write-Host ("Update channel of this build: {0}" -f (Format-UpdateChannel $compiledChannel)) -ForegroundColor Yellow
$channelPolicy = Test-LucentUpdateChannelPolicy -Compiled $compiledChannel -Declared $UpdateChannel -Expected $expectedChannels -StageOnly:$StageOnly
foreach ($warning in $channelPolicy.Warnings) { Write-Warning $warning }
if ($channelPolicy.Errors.Count -gt 0) {
    foreach ($problem in $channelPolicy.Errors) { Write-Host "    ERROR: $problem" -ForegroundColor Red }
    throw "The package's update channel is not the declared one ($channelsPath)."
}
if (-not $info.runtimeDeployed) { throw "$buildPath was configured with LUCENT_INSTALL_RUNTIME=OFF; reconfigure with it ON." }
$policy = Get-Content -LiteralPath (Join-Path $installerDir 'license-policy.json') -Raw | ConvertFrom-Json
$vcpkgConfiguration = Get-Content -LiteralPath (Join-Path $repoRoot 'vcpkg-configuration.json') -Raw | ConvertFrom-Json
$vcpkgBaseline = $vcpkgConfiguration.'default-registry'.baseline
if ($vcpkgBaseline -notmatch '^[0-9a-f]{40}$') { throw "vcpkg-configuration.json names no registry baseline commit." }

# The distribution decision (ADR 0024): how Lucent is distributed decides which terms apply to the
# tools and what each release publishes with its installer.
$distribution = if ($policy.PSObject.Properties.Name -contains 'distribution') { $policy.distribution } else { $null }
if (-not $distribution -or $distribution.commercial -isnot [bool] -or -not $distribution.lucentLicence -or
    -not $distribution.correspondingSource -or $distribution.correspondingSource.provision -ne 'archive-beside-installer' -or
    -not $distribution.signing -or -not $distribution.installerTool) {
    throw 'installer\license-policy.json records no complete distribution decision (distribution: commercial, lucentLicence, correspondingSource.provision "archive-beside-installer", signing, installerTool; ADR 0024).'
}
Write-Host ("Distribution: {0} ({1}), {2}; corresponding source beside the installer; release signing: {3}" -f $distribution.model,
    $distribution.lucentLicence, $(if ($distribution.commercial) { 'commercial' } else { 'non-commercial' }), $distribution.signing.release)
# The policy's signing status and this package agree (ADR 0024): once releases are signed, an unsigned
# Setup is a mistake; a release signature while the policy still says unsigned is worth a reminder.
if ($distribution.signing.release -notin 'unsigned', 'signed') { throw "installer\license-policy.json: distribution.signing.release is '$($distribution.signing.release)', not 'unsigned' or 'signed'." }
$releaseSigned = [bool]($signing -and -not $signing.Test)
if (-not $StageOnly -and $distribution.signing.release -eq 'signed' -and -not $releaseSigned) {
    throw 'installer\license-policy.json says releases are signed (distribution.signing.release), but this Setup would not carry a release signature: set LUCENT_SIGN_CERT_SHA1 and LUCENT_SIGN_TIMESTAMP_URL (see NOTES).'
}
if ($releaseSigned -and $distribution.signing.release -ne 'signed') {
    Write-Warning "This package is signed for release, but installer\license-policy.json still says releases are $($distribution.signing.release): set distribution.signing.release to 'signed' in the release commit."
}

# One version everywhere: vcpkg.json's version is Lucent's version, as the CMake project's is (the
# corresponding-source archive ships vcpkg.json; docs\release\checklist.md, version rules).
$vcpkgManifest = Get-Content -LiteralPath (Join-Path $repoRoot 'vcpkg.json') -Raw | ConvertFrom-Json
if ($vcpkgManifest.'version-semver' -cne $info.version) {
    throw "vcpkg.json has version $($vcpkgManifest.'version-semver'), but the build is Lucent $($info.version) (CMakeLists.txt project VERSION). Change both together."
}
$msvc = if ($info.dumpbin -match '/MSVC/([0-9.]+)/') { $Matches[1] } else { 'unknown' }

# Inno Setup is located before the slow steps, so a wrong -Iscc or LUCENT_ISCC fails at once.
$isccInfo = $null
if (-not $StageOnly) {
    $isccInfo = Find-InnoSetupCompiler -Path $Iscc
    if (-not $isccInfo.Standard) {
        Write-Warning "ISCC.exe is not in a standard Inno Setup 6 folder ($($isccInfo.Path), found via $($isccInfo.From)); pass -Iscc or set LUCENT_ISCC to choose the compiler explicitly. Its path and SHA-256 are recorded in the manifest."
    }
}

# Lucent's logo (ADR 0025): the branding asset, its manifest and every file's size and SHA-256 verified
# (Read-BrandingAsset), before the slow steps. The programs must carry its Windows icon, and Setup takes
# that icon and the wizard images by role, as the build does. Inno Setup separates image files by
# commas, so a path with a comma cannot be passed.
$branding = Read-BrandingAsset -Root (Join-Path $repoRoot 'assets\branding')
$brandingIcon = @(Get-BrandingFiles -Asset $branding -Role 'windows-icon')
if ($brandingIcon.Count -ne 1) { throw "The branding asset has $($brandingIcon.Count) windows-icon derivatives; Setup needs exactly one." }
$brandingIcon = $brandingIcon[0]
$brandingWizardImages = @(Get-BrandingFiles -Asset $branding -Role 'setup-wizard-image')
$brandingWizardSmallImages = @(Get-BrandingFiles -Asset $branding -Role 'setup-wizard-small-image')
foreach ($file in @($brandingIcon) + $brandingWizardImages + $brandingWizardSmallImages) {
    if ($file.Contains(',')) { throw "Inno Setup cannot take the branding file ${file}: its path holds a comma." }
}
Write-Host ("Branding {0}: {1}, {2} wizard images, {3} small wizard images (assets\branding, every file as its manifest lists)" -f $branding.Version,
    (Split-Path -Leaf $brandingIcon), $brandingWizardImages.Count, $brandingWizardSmallImages.Count)

# Never leave an earlier run's installer, checksum, manifest or SBOM copy next to this run's layout.
# A test-signed package says so in its file names (SEC-03).
$releaseBaseName = "Lucent-Setup-$($info.version)-x64"
$baseName = $releaseBaseName
if ($signing -and $signing.Test) { $baseName = "$releaseBaseName-TEST-SIGNED" }
$installerPath = Join-Path $OutputDir "$baseName.exe"
$manifestPath = Join-Path $OutputDir "$baseName.manifest.json"
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
foreach ($name in @($releaseBaseName, "$releaseBaseName-TEST-SIGNED")) {
    foreach ($stale in @((Join-Path $OutputDir "$name.exe"), (Join-Path $OutputDir "$name.exe.sha256"), (Join-Path $OutputDir "$name.manifest.json"),
                         (Join-Path $OutputDir "$name.spdx.json"))) {
        if (Test-Path -LiteralPath $stale) { Remove-Item -LiteralPath $stale -Force }
    }
}
$sourcePackagePath = Join-Path $OutputDir "Lucent-$($info.version)-corresponding-source.zip"
foreach ($stale in @((Join-Path $OutputDir 'lucent-associations.iss'), (Join-Path $OutputDir 'builder-paths.txt'), (Join-Path $OutputDir 'builder-paths.json'),
                     $sourcePackagePath, "$sourcePackagePath.partial")) {
    if (Test-Path -LiteralPath $stale) { Remove-Item -LiteralPath $stale -Force }
}
# An earlier run's version rehearsal (-VersionRehearsal) belongs to that run's layout.
if (Test-Path -LiteralPath (Join-Path $OutputDir 'rehearsal')) { Remove-Item -LiteralPath (Join-Path $OutputDir 'rehearsal') -Recurse -Force }

$gitHead = Invoke-NativeCommand -FilePath 'git' -Arguments @('-C', $repoRoot, 'rev-parse', 'HEAD')
if ($gitHead.ExitCode -ne 0) { throw 'git rev-parse HEAD failed.' }
$gitCommit = $gitHead.Output[0].Trim()
$gitDirty = @((Invoke-NativeCommand -FilePath 'git' -Arguments @('-C', $repoRoot, 'status', '--porcelain')).Output |
    Where-Object { $_.Trim() }).Count -gt 0
if (-not $gitCommit.StartsWith($info.gitCommitAtConfigure)) {
    Write-Warning "The build was configured at commit $($info.gitCommitAtConfigure) but HEAD is $gitCommit; Lucent.exe reports the configure-time commit. Rebuild without -SkipBuild."
}
if ($gitDirty) { Write-Warning 'The working tree has uncommitted changes; the manifest records gitDirty = true.' }

# --- 2. Installation folder ----------------------------------------------------------------------

Write-Step "Laying out the installation folder in $stage"
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
$install = Invoke-NativeCommand -FilePath 'cmake' -LogPath (Join-Path $OutputDir 'install.log') -Arguments @(
    '--install', $buildPath, '--prefix', $stage, '--component', $info.installComponent)
if ($install.ExitCode -ne 0) {
    $install.Output | Select-Object -Last 40 | ForEach-Object { Write-Host $_ }
    throw "cmake --install failed ($($install.ExitCode)); see $(Join-Path $OutputDir 'install.log')."
}
$install.Output | Where-Object { $_ -match '^-- (Copied|Removed)' } | ForEach-Object { Write-Host "    $($_.Substring(3))" }

# --- 3. Audit and smoke run ----------------------------------------------------------------------

Write-Step 'Auditing the installation folder (provenance, dumpbin /dependents, Qt plugins, QML modules)'
$report = Invoke-LayoutAudit -StageDir $stage -Info $info -Policy $policy
Write-AuditReport -Report $report -TextPath (Join-Path $OutputDir 'audit.txt') -JsonPath (Join-Path $OutputDir 'audit.json')
Write-Host ("    {0} files, {1} PE files, ports: {2}" -f $report.Files.Count, $report.PeFiles.Count, ($report.Ports -join ', '))
Write-Host ("    Visual C++ runtime imports: {0}" -f ($report.VcRuntimeImports -join ', '))
Write-Host ("    Windows DLL imports: {0}" -f (($report.WindowsDlls | ForEach-Object Name) -join ', '))
Write-Host ("    QML modules checked: {0}" -f ($report.QmlModules -join ', '))
Write-Host ("    Bundled assets verified: {0}" -f ((@($report.Assets) | ForEach-Object { "$($_.Id) $($_.Version)" }) -join ', '))
foreach ($warning in $report.Warnings) { Write-Warning $warning }
if ($report.Errors.Count -gt 0) {
    foreach ($problem in $report.Errors) { Write-Host "    ERROR: $problem" -ForegroundColor Red }
    throw "The dependency audit found $($report.Errors.Count) error(s); see $(Join-Path $OutputDir 'audit.txt')."
}
if ($report.VcRuntimeImports.Count -eq 0) { throw 'No Visual C++ runtime import was found; the runtime check in Setup would test nothing.' }

Write-Step "Smoke run: $($info.executable) from the folder with PATH = Windows only"
$smoke = Invoke-SmokeRun -StageDir $stage -Executable $info.executable -WorkDir (Join-Path $OutputDir 'smoke')
if ($smoke.Errors.Count -gt 0) {
    foreach ($problem in $smoke.Errors) { Write-Host "    ERROR: $problem" -ForegroundColor Red }
    throw "The smoke run of the installation folder failed; see $($smoke.Log)."
}
Write-Host "    Exit code 0, screenshot $($smoke.Screenshot), no Qt or QML loading errors"
Write-Host "    $($smoke.CameraSupport)"
$reportedChannel = @(Compare-UpdateChannel -Expected $compiledChannel -Actual $smoke.UpdateChannel)
if ($reportedChannel.Count -gt 0) {
    throw "The staged $($info.executable) checks another update channel than the build tree records ($($reportedChannel -join '; ')); rebuild without -SkipBuild."
}
Write-Host "    Update channel reported by $($info.executable): $(Format-UpdateChannel $smoke.UpdateChannel)"

# Lucent's logo in the shipped programs (ADR 0025): the icon group Windows shows for each is IDI_ICON1,
# image for image the branding asset's Windows icon.
Write-Step "Checking Lucent's icon in $(Get-LucentExecutablesText $info) (branding $($branding.Version))"
foreach ($name in @(@($info.executable) + @($info.helperExecutables) | Where-Object { $_ })) {
    $iconCheck = Test-ProgramIcon -Path (Join-Path $stage $name) -Icon $brandingIcon -GroupName 'IDI_ICON1' -First
    if ($iconCheck.Problems.Count -gt 0) {
        foreach ($problem in $iconCheck.Problems) { Write-Host "    ERROR: $problem" -ForegroundColor Red }
        throw "$name does not carry Lucent's icon; rebuild it (the build takes the icon from assets\branding)."
    }
    Write-Host ("    {0}: icon group {1}, {2} images, {3}" -f $name, $iconCheck.Group.Name, $iconCheck.Group.Images.Count, $iconCheck.Match)
}

$fileIndex = Get-VcpkgFileIndex -InstalledDir ($info.vcpkgInstalledDir.Replace('/', '\')) -Triplet $info.vcpkgTriplet
$staticLibraries = @(Get-StaticVcpkgLibraries -Info $info -FileIndex $fileIndex)
Write-Host ("    Static vcpkg libraries in {0}: {1}" -f (Get-LucentExecutablesText $info), $(if ($staticLibraries.Count) { ($staticLibraries | ForEach-Object { "$($_.Path) ($($_.Port), linked into $(@($_.Executables) -join ', '))" }) -join '; ' } else { 'none' }))

# --- 4. Optional signing of Lucent.exe and lucent-worker.exe -----------------------------------

$signCommand = $null
$signatures = New-Object System.Collections.Generic.List[object]
function Add-SignatureRecord([string]$Name, $Check) {
    $signatures.Add([ordered]@{ file = $Name; signerSha1 = $Check.Signer; trust = $Check.Trust; winVerifyTrust = $Check.TrustResult; timestamped = $Check.Timestamped })
}
if ($signing) {
    $signing.SignTool = if ($env:LUCENT_SIGNTOOL) { $env:LUCENT_SIGNTOOL } else {
        $found = Get-Command 'signtool.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
        if (-not $found) { throw 'signtool.exe was not found; set LUCENT_SIGNTOOL.' }
        $found.Source
    }
    if (-not (Test-Path -LiteralPath $signing.SignTool -PathType Leaf)) { throw "signtool '$($signing.SignTool)' does not exist." }
    $signing.Arguments = @('sign', '/fd', 'sha256', '/sha1', $signing.Certificate.Thumbprint)
    if ($signing.TimestampUrl) { $signing.Arguments += @('/tr', $signing.TimestampUrl, '/td', 'sha256') }
    # Lucent.exe and its helper executables (lucent-worker.exe): every program Lucent ships. Each
    # signature is verified: by the configured certificate, verifying offline (digest and chain; a
    # test certificate's untrusted root is the one failure accepted with LUCENT_SIGN_TEST), and
    # timestamped when a timestamp server is set.
    foreach ($name in @(@($info.executable) + @($info.helperExecutables) | Where-Object { $_ })) {
        $exe = Join-Path $stage $name
        Write-Step "Signing $name"
        $signed = Invoke-NativeCommand -FilePath $signing.SignTool -Arguments ($signing.Arguments + @('/d', 'Lucent', $exe))
        if ($signed.ExitCode -ne 0) { $signed.Output | ForEach-Object { Write-Host $_ }; throw "signtool failed on $name ($($signed.ExitCode))." }
        $check = Test-AuthenticodeFile -Path $exe -Thumbprint $signing.Certificate.Thumbprint -AllowUntrustedRoot:$signing.Test -RequireTimestamp:([bool]$signing.TimestampUrl)
        if (-not $check.Accepted) { throw "$name $($check.Problem) after signing." }
        Add-SignatureRecord $name $check
        Write-Host ("    Signed by {0}: {1} (WinVerifyTrust {2}), {3}" -f $check.Signer, $check.Trust, $check.TrustResult, $(if ($check.Timestamped) { 'timestamped' } else { 'no timestamp' }))
    }
    # Inno Setup signs Setup and the uninstaller with the same command. In a Sign Tool command $q is
    # a quote, $f the quoted file name and $$ a dollar sign, so a $ of our own is doubled.
    $escape = { param([string]$Text) $Text.Replace('$', '$$') }
    $signCommand = '$q' + (& $escape $signing.SignTool) + '$q ' + ((@($signing.Arguments) | ForEach-Object { & $escape $_ }) -join ' ') + ' /d $qLucent Setup$q $f'
}
else {
    Write-Host '    Signing is not configured (LUCENT_SIGN_CERT_SHA1): the package is unsigned.'
}

# --- 5. Licences, installed-file list and SBOM ---------------------------------------------------

$vcRedistPath = if ($VCRedist) { Resolve-RepoPath $VCRedist } else { [System.IO.Path]::GetFullPath($info.vcRedist.Replace('/', '\')) }
if (-not (Test-Path -LiteralPath $vcRedistPath -PathType Leaf)) { throw "The Visual C++ redistributable was not found ($vcRedistPath)." }
if ((Split-Path -Leaf $vcRedistPath) -ne 'vc_redist.x64.exe') { throw "Expected Microsoft's vc_redist.x64.exe, got $vcRedistPath." }
$vcSignature = Get-AuthenticodeSignature -LiteralPath $vcRedistPath
if ($vcSignature.Status -ne 'Valid' -or $vcSignature.SignerCertificate.Subject -notmatch '(^|, )O=Microsoft Corporation(,|$)') {
    throw "$vcRedistPath is not validly signed by Microsoft ($($vcSignature.Status))."
}
$vcVersionText = (Get-Item -LiteralPath $vcRedistPath).VersionInfo.FileVersion
if ($vcVersionText -notmatch '^(\d+)\.(\d+)\.(\d+)') { throw "Unexpected redistributable version '$vcVersionText'." }
$vc = [pscustomobject]@{
    Path = $vcRedistPath; Version = $vcVersionText; Major = [int]$Matches[1]; Minor = [int]$Matches[2]; Build = [int]$Matches[3]
    Sha256 = (Get-Sha256 $vcRedistPath); Sha1 = (Get-Sha1 $vcRedistPath) }

Write-Step 'Writing licences, notices, the installed-file list and the SBOM'
$downloads = if ($env:VCPKG_DOWNLOADS) { $env:VCPKG_DOWNLOADS } else { Join-Path $env:VCPKG_ROOT 'downloads' }
$assetLicences = @(Get-AssetLicences -StageDir $stage -Info $info -Policy $policy)
$components = New-LicenseBundle -StageDir $stage -Info $info -Report $report -Policy $policy -RepoRoot $repoRoot `
    -DownloadsDir $downloads -NoticesDir (Join-Path $installerDir 'notices') -StaticLibraries $staticLibraries `
    -AssetLicences $assetLicences -VcpkgRoot $env:VCPKG_ROOT -Baseline $vcpkgBaseline -GitCommit $gitCommit

# The corresponding source of the LGPL libraries (W12, ADR 0024): written, then read back and checked
# against vcpkg's record of every library that needs it, before the LGPL notice names it.
Write-Step 'Writing the corresponding source of the LGPL libraries'
$sourcePackage = New-CorrespondingSourcePackage -Path $sourcePackagePath -Info $info -Components $components -Report $report -Policy $policy `
    -RepoRoot $repoRoot -DownloadsDir $downloads -NoticesDir (Join-Path $installerDir 'notices') -VcpkgRoot $env:VCPKG_ROOT `
    -Baseline $vcpkgBaseline -GitCommit $gitCommit -GitDirty $gitDirty -Msvc $msvc
$provision = Get-SourceProvisionPlan -Components $components -Policy $policy
$sourceCheck = Test-CorrespondingSourcePackage -Path $sourcePackagePath -Version $info.version `
    -Expected @($provision.Components | ForEach-Object { Get-SourceProvisionExpectation -Component $_ })
$sourceProblems = @($provision.Errors) + @($sourceCheck.Errors)
if ($sourceCheck.Sha256 -ne $sourcePackage.Sha256) { $sourceProblems += "The archive changed after it was written ($($sourceCheck.Sha256), written $($sourcePackage.Sha256))." }
if ($sourceProblems.Count -gt 0) {
    foreach ($problem in $sourceProblems) { Write-Host "    ERROR: $problem" -ForegroundColor Red }
    Remove-Item -LiteralPath $sourcePackagePath -Force
    throw "$(Split-Path -Leaf $sourcePackagePath) is incomplete or wrong ($($sourceProblems.Count) problem(s)); it was deleted."
}
Write-Host ("    {0}: {1:N1} MiB ({2:N0} bytes), {3} files, SHA-256 {4}" -f $sourcePackage.File, ($sourcePackage.Size / 1MB), $sourcePackage.Size,
    @($sourcePackage.Entries).Count, $sourcePackage.Sha256)
foreach ($record in $sourcePackage.Components) {
    Write-Host ("    {0} {1} ({2}): {3}; recipe {4}" -f $record.Port, $record.Version, $record.Licence,
        ((@($record.Archives) | ForEach-Object { "$($_.file) ($(if ($_.size -ge 1MB) { '{0:N1} MiB' -f ($_.size / 1MB) } else { '{0:N0} KiB' -f ($_.size / 1KB) }))" }) -join ', '),
        $(if ($record.IsOverlay) { "Lucent overlay ($($record.Recipe.lucentSource))" } else { 'vcpkg registry port at the baseline' }))
}
foreach ($tool in $sourcePackage.BuildTools) { Write-Host "    Build tool recipe: $($tool.Port) $($tool.Version) (Lucent overlay $($tool.LucentSource))" }
Write-Host "    Read back with System.IO.Compression: every expected archive and recipe file present, every hash as recorded"
Write-LgplNotice -StageDir $stage -Info $info -Components $components -NoticesDir (Join-Path $installerDir 'notices') `
    -SourcePackage $sourcePackage -Baseline $vcpkgBaseline -GitCommit $gitCommit

$sbomRelative = 'licenses\Lucent.spdx.json'
$sbomPath = Join-Path $stage $sbomRelative
$listed = Write-InstalledFileList -StageDir $stage -Version $info.version -AlsoList @($sbomRelative)
$sbom = New-PackageSbom -Path $sbomPath -Info $info -Components $components -Report $report -StageDir $stage `
    -GitCommit $gitCommit -VcRedist $vc -Policy $policy -AssetLicences $assetLicences -VcpkgRoot $env:VCPKG_ROOT
Test-InstalledFileList -StageDir $stage
$attributions = @($components | Where-Object { $_.QtSbom } | ForEach-Object { $_.QtSbom.Attributions }).Count
Write-Host ("    {0} components ({1})" -f $components.Count, (($components | ForEach-Object { "$($_.Port) $($_.Version)" }) -join ', '))
Write-Host ("    {0} third-party components inside the shipped Qt files (from Qt's SBOMs)" -f $attributions)
Write-Host ("    Data assets: {0}" -f ((@($assetLicences) | ForEach-Object { "$($_.Id) $($_.Version) ($($_.Concluded))" }) -join ', '))
Write-Host ("    Installed-file list: {0} files" -f $listed)
Write-Host ("    SBOM: {0} packages, {1} files, {2} relationships; structural check passed" -f $sbom.Packages, $sbom.Files, $sbom.Relationships)
if ($sbom.DroppedRecipeFiles -gt 0) { Write-Warning "$($sbom.DroppedRecipeFiles) vcpkg recipe file(s) left out of the SBOM: not identical in the vcpkg ports tree." }

$validator = if ($SpdxValidator) { $SpdxValidator } elseif ($env:LUCENT_PYSPDXTOOLS) { $env:LUCENT_PYSPDXTOOLS } else {
    $command = Get-Command 'pyspdxtools' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command) { $command.Source } else { $null }
}
$sbomValidated = $false
if ($validator) {
    if (-not (Test-Path -LiteralPath $validator -PathType Leaf)) { throw "The SPDX validator '$validator' does not exist." }
    $validation = Invoke-NativeCommand -FilePath $validator -Arguments @('--infile', $sbomPath, '--version', 'SPDX-2.3')
    if ($validation.ExitCode -ne 0) {
        $validation.Output | Select-Object -First 30 | ForEach-Object { Write-Host "    $_" }
        throw "pyspdxtools rejected $sbomPath ($($validation.ExitCode))."
    }
    $sbomValidated = $true
    Write-Host "    SBOM validated by $validator"
}
else {
    Write-Host '    pyspdxtools was not found (-SpdxValidator, LUCENT_PYSPDXTOOLS, PATH): the SBOM was checked structurally only.'
}

Copy-Item -LiteralPath $sbomPath -Destination (Join-Path $OutputDir "$baseName.spdx.json") -Force

# --- 6. The build machine's folders in shipped files ----------------------------------------------

Write-Step 'Scanning every file of the folder, and what it compresses, for the build machine''s folders (user profile, repository, build tree, vcpkg)'
$pathScanner = New-BuilderPathScanner -Rules (Get-BuilderPathRules -Info $info -RepoRoot $repoRoot)
$pathReports = New-Object System.Collections.Generic.List[object]
$pathReportText = Join-Path $OutputDir 'builder-paths.txt'
$pathReportJson = Join-Path $OutputDir 'builder-paths.json'
$pathReports.Add((Invoke-BuilderPathAudit -Scanner $pathScanner -Root $stage -Label 'Installation folder'))
Write-BuilderPathReport -Reports $pathReports.ToArray() -Scanner $pathScanner -TextPath $pathReportText -JsonPath $pathReportJson
$folderScan = $pathReports[0]
Write-Host ("    {0} files, {1:N1} MiB in {2} s: {3} hit(s); {4}" -f $folderScan.Files, ($folderScan.Bytes / 1MB), $folderScan.Seconds, @($folderScan.Hits).Count,
    (Format-CompressedContentTally $folderScan.Compressed))
if (@($folderScan.Errors).Count -gt 0) {
    foreach ($problem in ($folderScan.Errors | Select-Object -First 40)) { Write-Host "    ERROR: $problem" -ForegroundColor Red }
    throw "$(@($folderScan.Hits).Count) place(s) in $(@($folderScan.HitFiles).Count) shipped file(s) name a folder of the build machine; see $pathReportText."
}
# The corresponding-source archive is published too: its own files (README, MANIFEST.json, recipes,
# triplet, build records) must not name the build machine's folders either. The upstream archives
# are the upstream projects' bytes, verified by SHA-512 instead.
$sourceScan = Invoke-BuilderPathAudit -Scanner $pathScanner -Blobs $sourcePackage.Texts -Label 'Corresponding source (files other than the upstream archives)'
$pathReports.Add($sourceScan)
Write-BuilderPathReport -Reports $pathReports.ToArray() -Scanner $pathScanner -TextPath $pathReportText -JsonPath $pathReportJson
Write-Host ("    {0}: {1} files, {2:N1} KiB: {3} hit(s)" -f $sourcePackage.File, $sourceScan.Files, ($sourceScan.Bytes / 1KB), @($sourceScan.Hits).Count)
if (@($sourceScan.Errors).Count -gt 0) {
    foreach ($problem in ($sourceScan.Errors | Select-Object -First 40)) { Write-Host "    ERROR: $problem" -ForegroundColor Red }
    Remove-Item -LiteralPath $sourcePackagePath -Force
    throw "$($sourcePackage.File) names a folder of the build machine; it was deleted. See $pathReportText."
}

# --- 7. Installer --------------------------------------------------------------------------------

# "Open with Lucent" covers exactly the RAW extensions the importer recognises.
$importSource = Get-Content -LiteralPath (Join-Path $repoRoot 'src\application\src\import.cpp') -Raw
if ($importSource -notmatch '(?s)kRawExtensions\s*=\s*\{(.*?)\};') { throw 'kRawExtensions was not found in src\application\src\import.cpp.' }
$extensions = @([regex]::Matches($Matches[1], 'L"([A-Za-z0-9]+)"') | ForEach-Object { $_.Groups[1].Value.ToLowerInvariant() } | Sort-Object -Unique)
if ($extensions.Count -lt 10) { throw "Only $($extensions.Count) RAW extensions were found in import.cpp." }

$installerEntry = $null
if (-not $StageOnly) {
    # Lucent is listed for every extension through Applications\Lucent.exe\SupportedTypes. The
    # OpenWithProgids value is added only where the extension already has a default program, so it
    # never changes what a double-click opens (ExtensionHasDefaultProgram in lucent.iss).
    $associations = New-Object System.Collections.Generic.List[string]
    $associations.Add('; Generated by tools\package.ps1 from kRawExtensions in src\application\src\import.cpp.')
    foreach ($extension in $extensions) {
        $associations.Add("Root: HKA; Subkey: ""Software\Classes\.$extension""; Flags: uninsdeletekeyifempty; Tasks: openwith; Check: ExtensionHasDefaultProgram('.$extension')")
        $associations.Add("Root: HKA; Subkey: ""Software\Classes\.$extension\OpenWithProgids""; ValueType: string; ValueName: ""{#ProgId}""; ValueData: """"; Flags: uninsdeletevalue uninsdeletekeyifempty; Tasks: openwith; Check: ExtensionHasDefaultProgram('.$extension')")
        $associations.Add("Root: HKA; Subkey: ""Software\Classes\Applications\{#AppExe}\SupportedTypes""; ValueType: string; ValueName: "".$extension""; ValueData: """"; Tasks: openwith")
    }
    $associationsPath = Join-Path $OutputDir 'lucent-associations.iss'
    Write-TextFile -Path $associationsPath -Text (($associations -join "`n") + "`n")

    Write-Step "Compiling the installer with $($isccInfo.Path) (found via $($isccInfo.From))"
    # Lucent's logo (ADR 0025): Setup's icon and the wizard images, every image-area size of each.
    $brandingArgs = @(
        "/DBrandingIcon=$brandingIcon",
        "/DBrandingWizardImages=$($brandingWizardImages -join ',')",
        "/DBrandingWizardSmallImages=$($brandingWizardSmallImages -join ',')")
    $isccArgs = @(
        "/DAppVersion=$($info.version)",
        "/DAppVersionQuad=$($info.version).0",
        "/DStagingDir=$stage",
        "/DVCRedistFile=$($vc.Path)",
        "/DVCRedistMajor=$($vc.Major)",
        "/DVCRedistMinor=$($vc.Minor)",
        "/DVCRedistBuild=$($vc.Build)",
        "/DVCRuntimeDlls=$($report.VcRuntimeImports -join ',')",
        "/DAssociationsInclude=$associationsPath") + $brandingArgs + @(
        "/O$OutputDir",
        "/F$baseName")
    if ($signCommand) { $isccArgs += @("/Slucent=$signCommand", '/DLucentSign') }
    $isccArgs += (Join-Path $installerDir 'lucent.iss')
    $compile = Invoke-NativeCommand -FilePath $isccInfo.Path -Arguments $isccArgs -LogPath (Join-Path $OutputDir 'iscc.log')
    if ($compile.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $installerPath)) {
        $compile.Output | Select-Object -Last 30 | ForEach-Object { Write-Host $_ }
        throw "ISCC failed ($($compile.ExitCode)); see $(Join-Path $OutputDir 'iscc.log')."
    }
    $engine = @($compile.Output | ForEach-Object { if ($_ -match '^Compiler engine version: (.+)$') { $Matches[1] } }) | Select-Object -First 1
    if ($engine) { $isccInfo | Add-Member -NotePropertyName Version -NotePropertyValue $engine.Trim() }
    # The compiler states its licence in its banner ("Non-commercial use only" for the free edition).
    # Its non-commercial terms cover Lucent's free distribution (ADR 0024), not a commercial one, which
    # needs a licence the banner positively states as commercial (W12 review LIC-6: an unrecognised
    # banner fails a commercial distribution instead of passing it).
    $isccLicenceRecord = Get-InnoSetupLicence -Output ([string[]]@($compile.Output))
    $isccLicence = $isccLicenceRecord.Line
    $isccInfo | Add-Member -NotePropertyName Licence -NotePropertyValue $isccLicence
    $licenceCheck = Test-InnoSetupLicence -Licence $isccLicenceRecord -Commercial ([bool]$distribution.commercial)
    foreach ($warning in $licenceCheck.Warnings) { Write-Warning $warning }
    if (@($licenceCheck.Errors).Count -gt 0) {
        Remove-Item -LiteralPath $installerPath -Force
        throw (@($licenceCheck.Errors) -join ' ')
    }
    Write-Host "    Compiler: $(if ($engine) { $engine.Trim() } else { 'Inno Setup 6' })$(if ($isccLicence) { ", $isccLicence" }) (Lucent's distribution: $(if ($distribution.commercial) { 'commercial' } else { 'free, non-commercial' }))"
    # A Setup that failed a check is deleted, so no unchecked installer is left next to the folder.
    # Setup's own icon (Explorer, the download, the uninstaller) is Lucent's: its first icon group holds
    # exactly the images of the branding asset's Windows icon (ADR 0025).
    $setupIcon = Test-ProgramIcon -Path $installerPath -Icon $brandingIcon -First
    if ($setupIcon.Problems.Count -gt 0) {
        foreach ($problem in $setupIcon.Problems) { Write-Host "    ERROR: $problem" -ForegroundColor Red }
        Remove-Item -LiteralPath $installerPath -Force
        throw "Setup does not carry Lucent's icon; see $(Join-Path $OutputDir 'iscc.log')."
    }
    Write-Host ("    Setup's icon: group {0}, {1} images of {2}, {3}" -f $setupIcon.Group.Name, $setupIcon.Group.Images.Count, (Split-Path -Leaf $brandingIcon), $setupIcon.Match)
    if ($signing) {
        $check = Test-AuthenticodeFile -Path $installerPath -Thumbprint $signing.Certificate.Thumbprint -AllowUntrustedRoot:$signing.Test -RequireTimestamp:([bool]$signing.TimestampUrl)
        if (-not $check.Accepted) { Remove-Item -LiteralPath $installerPath -Force; throw "$baseName.exe $($check.Problem); see $(Join-Path $OutputDir 'iscc.log')." }
        Add-SignatureRecord "$baseName.exe" $check
        Write-Host ("    Setup signed by {0}: {1} (WinVerifyTrust {2}), {3}; the uninstaller is signed inside it (SignedUninstaller)" -f $check.Signer, $check.Trust, $check.TrustResult, $(if ($check.Timestamped) { 'timestamped' } else { 'no timestamp' }))
    }
    # Setup must not name the build machine's folders either: its loader, version information and
    # signature, and (W12 review SEC-07) what it compresses: the setup data (the compiled script,
    # messages, file names; LZMA), the setup engine (LZMA) and the installed files (LZMA2: the folder
    # scanned in step 6 and Microsoft's runtime installer), each decompressed and scanned.
    $setupScan = Invoke-BuilderPathAudit -Scanner $pathScanner -Path $installerPath -Label 'Setup' -InnoSetup
    $pathReports.Add($setupScan)
    Write-BuilderPathReport -Reports $pathReports.ToArray() -Scanner $pathScanner -TextPath $pathReportText -JsonPath $pathReportJson
    Write-Host ("    Setup scanned in {0} s: {1} hit(s); {2}" -f $setupScan.Seconds, @($setupScan.Hits).Count, (Format-CompressedContentTally $setupScan.Compressed))
    if (@($setupScan.Errors).Count -gt 0) {
        foreach ($problem in $setupScan.Errors) { Write-Host "    ERROR: $problem" -ForegroundColor Red }
        Remove-Item -LiteralPath $installerPath -Force
        throw "Setup names a folder of the build machine; see $pathReportText."
    }
    $installerItem = Get-Item -LiteralPath $installerPath
    $installerEntry = [ordered]@{
        file = $installerItem.Name; size = $installerItem.Length; sha256 = (Get-Sha256 $installerPath)
        signed = [bool]$signing }
    Write-TextFile -Path "$installerPath.sha256" -Text "$($installerEntry.sha256) *$($installerItem.Name)`n" -KeepLineEndings

    # The version rehearsal (W12 review SEC-04, part 2): the same installation folder as the next patch
    # version, so that installer\tests\Test-InstalledPackage.ps1 -NewerInstaller can upgrade to it and
    # see the package's Setup refuse to go back. Unsigned, named as a rehearsal, in its own folder, never
    # recorded in the package manifest and never published.
    if ($VersionRehearsal) {
        if ($info.version -notmatch '^(\d+)\.(\d+)\.(\d+)$') { throw "The version '$($info.version)' is not MAJOR.MINOR.PATCH." }
        $rehearsalVersion = '{0}.{1}.{2}' -f $Matches[1], $Matches[2], ([int64]$Matches[3] + 1)
        $rehearsalDir = Join-Path $OutputDir 'rehearsal'
        if (Test-Path -LiteralPath $rehearsalDir) { Remove-Item -LiteralPath $rehearsalDir -Recurse -Force }
        New-Item -ItemType Directory -Path $rehearsalDir | Out-Null
        $rehearsalBase = "Lucent-Setup-$rehearsalVersion-x64-VERSION-REHEARSAL"
        Write-Step "Compiling the version rehearsal $rehearsalBase.exe (Lucent $($info.version)'s files as $rehearsalVersion; never published)"
        $rehearsalArgs = @(
            "/DAppVersion=$rehearsalVersion", "/DAppVersionQuad=$rehearsalVersion.0", "/DVersionRehearsalOf=$($info.version)",
            "/DStagingDir=$stage", "/DVCRedistFile=$($vc.Path)", "/DVCRedistMajor=$($vc.Major)", "/DVCRedistMinor=$($vc.Minor)",
            "/DVCRedistBuild=$($vc.Build)", "/DVCRuntimeDlls=$($report.VcRuntimeImports -join ',')",
            "/DAssociationsInclude=$associationsPath") + $brandingArgs + @("/O$rehearsalDir", "/F$rehearsalBase", (Join-Path $installerDir 'lucent.iss'))
        $rehearsalCompile = Invoke-NativeCommand -FilePath $isccInfo.Path -Arguments $rehearsalArgs -LogPath (Join-Path $rehearsalDir 'iscc.log')
        $rehearsalPath = Join-Path $rehearsalDir "$rehearsalBase.exe"
        if ($rehearsalCompile.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $rehearsalPath)) {
            $rehearsalCompile.Output | Select-Object -Last 30 | ForEach-Object { Write-Host $_ }
            throw "ISCC failed on the version rehearsal ($($rehearsalCompile.ExitCode)); see $(Join-Path $rehearsalDir 'iscc.log')."
        }
        Write-TextFile -Path (Join-Path $rehearsalDir 'README.txt') -Text ((@(
            "$rehearsalBase.exe is a TEST INSTALLER, never to be published: the files of Lucent $($info.version)",
            "($baseName.exe) registered as version $rehearsalVersion, made by tools\package.ps1 -VersionRehearsal so that",
            'installer\tests\Test-InstalledPackage.ps1 -NewerInstaller can upgrade to a newer version and check that',
            "the package's Setup refuses to install over it (W12 review SEC-04).") -join "`n") + "`n")
        Write-Host ("    {0} ({1:N1} MiB, SHA-256 {2})" -f $rehearsalPath, ((Get-Item -LiteralPath $rehearsalPath).Length / 1MB), (Get-Sha256 $rehearsalPath))
    }
}

# --- 8. Manifest ---------------------------------------------------------------------------------

# The manifest is published with Setup, so it names no folder of the build machine either: a tool
# below the user profile is recorded as %USERPROFILE%\..., and the text is scanned before it is written.
function ConvertTo-PublishedPath([string]$Path) {
    if (-not $Path) { return $null }
    $profileRoot = $env:USERPROFILE.TrimEnd('\') + '\'
    if ($Path.StartsWith($profileRoot, [System.StringComparison]::OrdinalIgnoreCase)) { return '%USERPROFILE%\' + $Path.Substring($profileRoot.Length) }
    return $Path
}
$files = Get-FileInventory -Root $stage
$sourceProvisionPorts = @($provision.Components | ForEach-Object Port)
$manifest = [ordered]@{
    schema = 5
    product = 'Lucent'
    version = $info.version
    architecture = 'x64'
    gitCommit = $gitCommit
    gitDirty = $gitDirty
    created = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    installer = $installerEntry
    # The corresponding source of the LGPL libraries, published beside the installer (ADR 0024).
    correspondingSource = [ordered]@{
        file = $sourcePackage.File; size = $sourcePackage.Size; sha256 = $sourcePackage.Sha256
        root = $sourcePackage.Root; format = 'zip, stored entries sorted by name, each dated 1980-02-01 00:00'
        components = @($sourcePackage.Components | ForEach-Object { [ordered]@{
            port = $_.Port; version = $_.Version; licence = $_.Licence
            archives = @($_.Archives | ForEach-Object { [ordered]@{ file = $_.file; url = $_.url; size = $_.size; sha512 = $_.sha512; sha256 = $_.sha256 } })
            recipe = [ordered]@{ folder = $_.Recipe.folder; origin = $_.Recipe.origin; files = @($_.Recipe.files) } } })
        buildTools = @($sourcePackage.BuildTools | ForEach-Object { [ordered]@{ port = $_.Port; version = $_.Version; recipe = $_.Recipe.folder } })
        files = @($sourcePackage.Entries) }
    distribution = [ordered]@{
        model = $distribution.model; lucentLicence = $distribution.lucentLicence; commercial = $distribution.commercial
        decided = $distribution.decided; releaseSigning = $distribution.signing.release
        installerTool = "$($distribution.installerTool.name), $($distribution.installerTool.use) use" }
    signed = [bool]$signing
    signing = $(if ($signing) { [ordered]@{
        test = $signing.Test
        certificateSha1 = $signing.Certificate.Thumbprint; subject = $signing.Certificate.Subject; issuer = $signing.Certificate.Issuer
        notAfter = $signing.Certificate.NotAfter.ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
        digest = 'sha256'; timestampUrl = $(if ($signing.TimestampUrl) { $signing.TimestampUrl } else { $null }); timestamped = [bool]$signing.TimestampUrl
        uninstaller = 'signed by Inno Setup inside Setup (SignedUninstaller); installer\tests\Test-InstalledPackage.ps1 -SignerThumbprint verifies it after installing'
        signatures = $signatures.ToArray() } } else { $null })
    # The channel the package's Lucent.exe checks (null: none) and what -UpdateChannel declared (SEC-02).
    updateChannel = $(if ($compiledChannel) { [ordered]@{ name = $compiledChannel.name; manifestUrl = $compiledChannel.manifestUrl
        keyIds = @($compiledChannel.keyIds); publisherThumbprints = @($compiledChannel.publisherThumbprints) } } else { $null })
    updateChannelDeclared = $(if ($UpdateChannel) { $UpdateChannel } else { $null })
    vcRedistributable = [ordered]@{ file = 'vc_redist.x64.exe'; version = $vc.Version; sha256 = $vc.Sha256 }
    toolchain = [ordered]@{
        msvc = $msvc; qt = $info.qtVersion; vcpkgTriplet = $info.vcpkgTriplet
        vcpkgBaseline = $vcpkgBaseline
        innoSetup = $(if ($isccInfo) { ConvertTo-PublishedPath $isccInfo.Path } else { $null })
        innoSetupVersion = $(if ($isccInfo -and ($isccInfo.PSObject.Properties.Name -contains 'Version')) { $isccInfo.Version } else { $null })
        innoSetupSha256 = $(if ($isccInfo) { $isccInfo.Sha256 } else { $null })
        innoSetupLicence = $(if ($isccInfo -and ($isccInfo.PSObject.Properties.Name -contains 'Licence')) { $isccInfo.Licence } else { $null })
        innoSetupStandardInstall = $(if ($isccInfo) { $isccInfo.Standard } else { $null }) }
    audit = [ordered]@{
        errors = $report.Errors.Count; warnings = @($report.Warnings)
        vcRuntimeImports = @($report.VcRuntimeImports)
        windowsImports = @($report.WindowsDlls | ForEach-Object Name)
        qmlModules = @($report.QmlModules)
        requiredPlugins = @($info.requiredPlugins)
        smokeRunExitCode = $smoke.ExitCode
        builderPaths = [ordered]@{
            searched = @($pathScanner.Groups | ForEach-Object Rule | Select-Object -Unique)
            filesScanned = ($pathReports | Measure-Object -Property Files -Sum).Sum
            bytesScanned = ($pathReports | Measure-Object -Property Bytes -Sum).Sum
            # W12 review SEC-07: compressed parts decompressed and scanned (zlib and gzip streams, Setup's LZMA parts).
            compressedPartsScanned = ($pathReports | ForEach-Object { $_.Compressed.Streams } | Measure-Object -Sum).Sum
            decompressedBytesScanned = ($pathReports | ForEach-Object { $_.Compressed.Bytes } | Measure-Object -Sum).Sum
            hits = 0 } }
    sbom = [ordered]@{ file = 'licenses\Lucent.spdx.json'; packages = $sbom.Packages; files = $sbom.Files; validatedBy = $(if ($sbomValidated) { 'pyspdxtools' } else { 'structural check only' }) }
    fileAssociations = [ordered]@{ progId = 'Lucent.RawPhoto'; extensions = @($extensions | ForEach-Object { ".$_" })
        openWithProgids = 'only for extensions that already have a default program at install time' }
    components = @($components | ForEach-Object { [ordered]@{ port = $_.Port; version = $_.Version; licence = $_.LicenseConcluded; linkage = $_.Linkage
        correspondingSource = ($sourceProvisionPorts -contains $_.Port) } })
    staticLibraries = @($components | ForEach-Object { $_.StaticLibraries } | Where-Object { $_ } | ForEach-Object { [ordered]@{ library = $_.Path; licence = $_.LicenseConcluded } })
    files = $files
}
$manifestJson = ConvertTo-Json -InputObject $manifest -Depth 32
$manifestScan = Invoke-BuilderPathAudit -Scanner $pathScanner -Text $manifestJson -Name (Split-Path -Leaf $manifestPath) -Label 'Package manifest'
$pathReports.Add($manifestScan)
Write-BuilderPathReport -Reports $pathReports.ToArray() -Scanner $pathScanner -TextPath $pathReportText -JsonPath $pathReportJson
if (@($manifestScan.Errors).Count -gt 0) {
    foreach ($problem in $manifestScan.Errors) { Write-Host "    ERROR: $problem" -ForegroundColor Red }
    throw "The package manifest would name a folder of the build machine; see $pathReportText."
}
Write-TextFile -Path $manifestPath -Text ($manifestJson + "`n") -KeepLineEndings

$stageSize = ($files | ForEach-Object { $_.size } | Measure-Object -Sum).Sum
Write-Step ('Done in {0:N0} s' -f ((Get-Date) - $started).TotalSeconds)
Write-Host ("    Installation folder: {0} ({1} files, {2:N1} MiB)" -f $stage, $files.Count, ($stageSize / 1MB))
Write-Host ("    Builder paths: none in {0} scanned file(s) ({1})" -f ($pathReports | Measure-Object -Property Files -Sum).Sum, $pathReportText)
if ($installerEntry) {
    Write-Host ("    Installer: {0}" -f $installerPath)
    Write-Host ("    Size:      {0:N1} MiB ({1} bytes)" -f ($installerEntry.size / 1MB), $installerEntry.size)
    Write-Host ("    SHA-256:   {0}" -f $installerEntry.sha256)
    Write-Host ("    Signed:    {0}" -f $installerEntry.signed)
}
else {
    Write-Host '    Installer: not built (-StageOnly); the manifest records none.'
}
if ($signing) {
    Write-Host ("    Signatures: {0} by {1}{2}" -f (($signatures | ForEach-Object { $_.file }) -join ', '), $signing.Certificate.Thumbprint,
        $(if ($signing.Test) { ' - TEST SIGNATURE (untrusted root accepted), not for distribution' } else { '' }))
}
Write-Host ("    Update channel: {0}{1}" -f (Format-UpdateChannel $compiledChannel), $(if ($UpdateChannel) { " (declared: $UpdateChannel)" } else { ' (not declared: -StageOnly)' }))
Write-Host ("    Corresponding source: {0}" -f $sourcePackagePath)
Write-Host ("    Size:      {0:N1} MiB ({1} bytes), {2} files" -f ($sourcePackage.Size / 1MB), $sourcePackage.Size, @($sourcePackage.Entries).Count)
Write-Host ("    SHA-256:   {0}" -f $sourcePackage.Sha256)
Write-Host ("    Manifest:  {0}" -f $manifestPath)
Write-Host '    A release publishes the installer, its corresponding-source archive and its manifest together, after the gates of docs\release\checklist.md.'
