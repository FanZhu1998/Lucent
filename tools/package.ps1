<#
.SYNOPSIS
    Builds the Lucent installer, Lucent-Setup-<version>-x64.exe (docs/adr/0009-windows-packaging.md).

.DESCRIPTION
    1. Builds the release preset with tools\build.ps1 (optionally from scratch, optionally with tests).
       Outputs of an earlier run for the same version (installer, checksum, manifest, SBOM copy) are
       then deleted, so they can never be paired with a different layout.
    2. Lays out the installation folder with `cmake --install`: Lucent.exe, lucent-worker.exe (the
       isolated export worker it starts, W7 / ADR 0012), Qt deployed by windeployqt,
       every other runtime DLL, an installed-layout qt.conf and the bundled data assets
       (assets\<id>: the camera support database, W10 / ADR 0018) (cmake/LucentPackaging.cmake).
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
       folder as active. Any error stops the packaging.
    4. Signs Lucent.exe and lucent-worker.exe when signing is configured (see NOTES; off by default),
       and verifies each signature: the configured certificate's thumbprint, an offline
       WinVerifyTrust result, the timestamp.
    5. Writes licenses\: Lucent's licence, the copyright file of every shipped vcpkg port, licence
       texts vcpkg lacks, the Qt LGPL-3.0 and GPL-3.0 texts, LGPL-NOTICE.txt, THIRD-PARTY-NOTICES.txt
       (including the third-party code inside Qt from Qt's own SBOM and the libraries linked
       statically into Lucent's executables), then Lucent-files.txt (the installed-file list that the next
       version's Setup uses to upgrade) and the SBOM Lucent.spdx.json (SPDX 2.3, checked
       structurally, and with pyspdxtools when it is available).
    6. Scans every byte of every file of the folder for the build machine's folders (W12): the user
       profile, the repository, the build tree, the vcpkg root, installed tree, buildtrees, packages
       and downloads folders, and any X:\Users\<name>\ path, in 8-bit and UTF-16 text, either
       separator, any letter case. One hit (a PDB path in a PE debug directory, a __FILE__ string,
       a path in a text file) stops the packaging, naming the file and the byte offset
       (builder-paths.txt). Setup and the package manifest are scanned the same way.
    7. Compiles installer\lucent.iss with Inno Setup 6 (unless -StageOnly; Setup and the uninstaller
       are signed when signing is configured, and Setup's signature is verified) and writes the
       package manifest (version, git commit, signing, every file with its SHA-256, the installer's
       size and SHA-256, or no installer with -StageOnly).

    Outputs go to <BuildDir>\package unless -OutputDir is given:
      stage\                                  the installation folder that Setup installs
      Lucent-Setup-<version>-x64.exe           the installer
      Lucent-Setup-<version>-x64.manifest.json package manifest
      Lucent-Setup-<version>-x64.spdx.json     copy of the SBOM
      Lucent-Setup-<version>-x64.exe.sha256    checksum line
      audit.txt, audit.json, builder-paths.txt, builder-paths.json, install.log, smoke\ (the smoke
      run), iscc.log (and windeployqt.log in <BuildDir>\packaging)

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

.EXAMPLE
    powershell -NoProfile -File tools\package.ps1
.EXAMPLE
    powershell -NoProfile -File tools\package.ps1 -Fresh -RunTests -Jobs 2
.EXAMPLE
    powershell -NoProfile -File tools\package.ps1 -SkipBuild -StageOnly

.NOTES
    Authenticode signing is off by default, and builds made so far are unsigned. To sign Lucent.exe,
    lucent-worker.exe, Setup and the uninstaller, set before running:
      LUCENT_SIGN_CERT_SHA1      thumbprint of a code-signing certificate with its private key in the
                                 current user's personal store (Cert:\CurrentUser\My), checked
                                 before the build: present, valid now, usable for code signing
      LUCENT_SIGN_TIMESTAMP_URL  RFC 3161 timestamp server of the certificate's issuer (required)
      LUCENT_SIGNTOOL            optional path of signtool.exe (default: signtool.exe on PATH, which
                                 the Visual Studio environment provides)
      LUCENT_SIGN_TEST=1         a test signature (a self-signed certificate, for rehearsing the
                                 signing path): the one verification failure accepted is the
                                 untrusted root, LUCENT_SIGN_TIMESTAMP_URL may be empty (no
                                 timestamp: the signature then expires with the certificate), and
                                 the manifest records signing.test = true. Never for distribution.
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
    [string]$SpdxValidator
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0
$repoRoot = Split-Path -Parent $PSScriptRoot
$installerDir = Join-Path $repoRoot 'installer'
Import-Module (Join-Path $installerDir 'scripts\LucentPackaging.psm1') -Force

function Write-Step([string]$Text) { Write-Host "==> $Text" -ForegroundColor Cyan }

function Resolve-RepoPath([string]$Path) {
    # Full path without a trailing separator (a trailing backslash breaks native argument quoting).
    $full = if ([System.IO.Path]::IsPathRooted($Path)) { [System.IO.Path]::GetFullPath($Path) } else { [System.IO.Path]::GetFullPath((Join-Path $repoRoot $Path)) }
    if ($full.Length -gt 3) { $full = $full.TrimEnd('\', '/') }
    return $full
}

if ($SkipBuild -and ($Fresh -or $RunTests)) { throw '-SkipBuild cannot be combined with -Fresh or -RunTests.' }
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
if (-not $info.runtimeDeployed) { throw "$buildPath was configured with LUCENT_INSTALL_RUNTIME=OFF; reconfigure with it ON." }
$policy = Get-Content -LiteralPath (Join-Path $installerDir 'license-policy.json') -Raw | ConvertFrom-Json
$vcpkgConfiguration = Get-Content -LiteralPath (Join-Path $repoRoot 'vcpkg-configuration.json') -Raw | ConvertFrom-Json
$vcpkgBaseline = $vcpkgConfiguration.'default-registry'.baseline

# Inno Setup is located before the slow steps, so a wrong -Iscc or LUCENT_ISCC fails at once.
$isccInfo = $null
if (-not $StageOnly) {
    $isccInfo = Find-InnoSetupCompiler -Path $Iscc
    if (-not $isccInfo.Standard) {
        Write-Warning "ISCC.exe is not in a standard Inno Setup 6 folder ($($isccInfo.Path), found via $($isccInfo.From)); pass -Iscc or set LUCENT_ISCC to choose the compiler explicitly. Its path and SHA-256 are recorded in the manifest."
    }
}

# Never leave an earlier run's installer, checksum, manifest or SBOM copy next to this run's layout.
$baseName = "Lucent-Setup-$($info.version)-x64"
$installerPath = Join-Path $OutputDir "$baseName.exe"
$manifestPath = Join-Path $OutputDir "$baseName.manifest.json"
New-Item -ItemType Directory -Path $OutputDir -Force | Out-Null
foreach ($stale in @($installerPath, "$installerPath.sha256", $manifestPath, (Join-Path $OutputDir "$baseName.spdx.json"),
                     (Join-Path $OutputDir 'lucent-associations.iss'), (Join-Path $OutputDir 'builder-paths.txt'), (Join-Path $OutputDir 'builder-paths.json'))) {
    if (Test-Path -LiteralPath $stale) { Remove-Item -LiteralPath $stale -Force }
}

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

$fileIndex = Get-VcpkgFileIndex -InstalledDir ($info.vcpkgInstalledDir.Replace('/', '\')) -Triplet $info.vcpkgTriplet
$staticLibraries = @(Get-StaticVcpkgLibraries -Info $info -FileIndex $fileIndex)
Write-Host ("    Static vcpkg libraries linked into {0}: {1}" -f (Get-LucentExecutablesText $info), $(if ($staticLibraries.Count) { ($staticLibraries | ForEach-Object { "$($_.Path) ($($_.Port))" }) -join ', ' } else { 'none' }))

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
$components = New-LicenseBundle -StageDir $stage -Info $info -Report $report -Policy $policy -RepoRoot $repoRoot `
    -DownloadsDir $downloads -NoticesDir (Join-Path $installerDir 'notices') -StaticLibraries $staticLibraries `
    -VcpkgRoot $env:VCPKG_ROOT -Baseline $vcpkgBaseline
$sbomRelative = 'licenses\Lucent.spdx.json'
$sbomPath = Join-Path $stage $sbomRelative
$listed = Write-InstalledFileList -StageDir $stage -Version $info.version -AlsoList @($sbomRelative)
$sbom = New-PackageSbom -Path $sbomPath -Info $info -Components $components -Report $report -StageDir $stage `
    -GitCommit $gitCommit -VcRedist $vc -Policy $policy -VcpkgRoot $env:VCPKG_ROOT
Test-InstalledFileList -StageDir $stage
$attributions = @($components | Where-Object { $_.QtSbom } | ForEach-Object { $_.QtSbom.Attributions }).Count
Write-Host ("    {0} components ({1})" -f $components.Count, (($components | ForEach-Object { "$($_.Port) $($_.Version)" }) -join ', '))
Write-Host ("    {0} third-party components inside the shipped Qt files (from Qt's SBOMs)" -f $attributions)
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

Write-Step 'Scanning every file of the folder for the build machine''s folders (user profile, repository, build tree, vcpkg)'
$pathScanner = New-BuilderPathScanner -Rules (Get-BuilderPathRules -Info $info -RepoRoot $repoRoot)
$pathReports = New-Object System.Collections.Generic.List[object]
$pathReportText = Join-Path $OutputDir 'builder-paths.txt'
$pathReportJson = Join-Path $OutputDir 'builder-paths.json'
$pathReports.Add((Invoke-BuilderPathAudit -Scanner $pathScanner -Root $stage -Label 'Installation folder'))
Write-BuilderPathReport -Reports $pathReports.ToArray() -Scanner $pathScanner -TextPath $pathReportText -JsonPath $pathReportJson
$folderScan = $pathReports[0]
Write-Host ("    {0} files, {1:N1} MiB in {2} s: {3} hit(s)" -f $folderScan.Files, ($folderScan.Bytes / 1MB), $folderScan.Seconds, @($folderScan.Hits).Count)
if (@($folderScan.Errors).Count -gt 0) {
    foreach ($problem in ($folderScan.Errors | Select-Object -First 40)) { Write-Host "    ERROR: $problem" -ForegroundColor Red }
    throw "$(@($folderScan.Hits).Count) place(s) in $(@($folderScan.HitFiles).Count) shipped file(s) name a folder of the build machine; see $pathReportText."
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
    $isccArgs = @(
        "/DAppVersion=$($info.version)",
        "/DAppVersionQuad=$($info.version).0",
        "/DStagingDir=$stage",
        "/DVCRedistFile=$($vc.Path)",
        "/DVCRedistMajor=$($vc.Major)",
        "/DVCRedistMinor=$($vc.Minor)",
        "/DVCRedistBuild=$($vc.Build)",
        "/DVCRuntimeDlls=$($report.VcRuntimeImports -join ',')",
        "/DAssociationsInclude=$associationsPath",
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
    # A Setup that failed a check is deleted, so no unchecked installer is left next to the folder.
    if ($signing) {
        $check = Test-AuthenticodeFile -Path $installerPath -Thumbprint $signing.Certificate.Thumbprint -AllowUntrustedRoot:$signing.Test -RequireTimestamp:([bool]$signing.TimestampUrl)
        if (-not $check.Accepted) { Remove-Item -LiteralPath $installerPath -Force; throw "$baseName.exe $($check.Problem); see $(Join-Path $OutputDir 'iscc.log')." }
        Add-SignatureRecord "$baseName.exe" $check
        Write-Host ("    Setup signed by {0}: {1} (WinVerifyTrust {2}), {3}; the uninstaller is signed inside it (SignedUninstaller)" -f $check.Signer, $check.Trust, $check.TrustResult, $(if ($check.Timestamped) { 'timestamped' } else { 'no timestamp' }))
    }
    # Setup's own uncompressed parts (loader, version information, signature) must not name the
    # build machine's folders either; the files it installs were scanned in step 6.
    $setupScan = Invoke-BuilderPathAudit -Scanner $pathScanner -Path $installerPath -Label 'Setup'
    $pathReports.Add($setupScan)
    Write-BuilderPathReport -Reports $pathReports.ToArray() -Scanner $pathScanner -TextPath $pathReportText -JsonPath $pathReportJson
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
$msvc = if ($info.dumpbin -match '/MSVC/([0-9.]+)/') { $Matches[1] } else { 'unknown' }
$files = Get-FileInventory -Root $stage
$manifest = [ordered]@{
    schema = 3
    product = 'Lucent'
    version = $info.version
    architecture = 'x64'
    gitCommit = $gitCommit
    gitDirty = $gitDirty
    created = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    installer = $installerEntry
    signed = [bool]$signing
    signing = $(if ($signing) { [ordered]@{
        test = $signing.Test
        certificateSha1 = $signing.Certificate.Thumbprint; subject = $signing.Certificate.Subject; issuer = $signing.Certificate.Issuer
        notAfter = $signing.Certificate.NotAfter.ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
        digest = 'sha256'; timestampUrl = $(if ($signing.TimestampUrl) { $signing.TimestampUrl } else { $null }); timestamped = [bool]$signing.TimestampUrl
        uninstaller = 'signed by Inno Setup inside Setup (SignedUninstaller); installer\tests\Test-InstalledPackage.ps1 -SignerThumbprint verifies it after installing'
        signatures = $signatures.ToArray() } } else { $null })
    vcRedistributable = [ordered]@{ file = 'vc_redist.x64.exe'; version = $vc.Version; sha256 = $vc.Sha256 }
    toolchain = [ordered]@{
        msvc = $msvc; qt = $info.qtVersion; vcpkgTriplet = $info.vcpkgTriplet
        vcpkgBaseline = $vcpkgBaseline
        innoSetup = $(if ($isccInfo) { ConvertTo-PublishedPath $isccInfo.Path } else { $null })
        innoSetupVersion = $(if ($isccInfo -and ($isccInfo.PSObject.Properties.Name -contains 'Version')) { $isccInfo.Version } else { $null })
        innoSetupSha256 = $(if ($isccInfo) { $isccInfo.Sha256 } else { $null })
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
            hits = 0 } }
    sbom = [ordered]@{ file = 'licenses\Lucent.spdx.json'; packages = $sbom.Packages; files = $sbom.Files; validatedBy = $(if ($sbomValidated) { 'pyspdxtools' } else { 'structural check only' }) }
    fileAssociations = [ordered]@{ progId = 'Lucent.RawPhoto'; extensions = @($extensions | ForEach-Object { ".$_" })
        openWithProgids = 'only for extensions that already have a default program at install time' }
    components = @($components | ForEach-Object { [ordered]@{ port = $_.Port; version = $_.Version; licence = $_.LicenseConcluded; linkage = $_.Linkage } })
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
Write-Host ("    Manifest:  {0}" -f $manifestPath)
Write-Host '    Not for public distribution yet: how the LGPL libraries'' corresponding source is provided is decided in W12 (ADR 0009).'
