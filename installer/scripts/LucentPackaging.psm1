#Requires -Version 5.1
<#
.SYNOPSIS
    Packaging functions used by tools\package.ps1 (docs/adr/0009-windows-packaging.md).

.DESCRIPTION
    - Read-LucentPackageInfo      reads <build dir>\packaging\lucent-package.json (written by CMake).
    - Invoke-LayoutAudit          audits the installed layout: provenance of every file, PE imports
                                  (dumpbin), required Qt plugins, QML modules and their plugin DLLs,
                                  architecture, subsystem, qt.conf, forbidden files, and the bundled
                                  data assets against their manifests and the build's pins
                                  (Test-BundledAssets, W10, ADR 0018).
    - Invoke-SmokeRun             starts the staged Lucent.exe with a Windows-only PATH and checks it.
    - Get-StaticVcpkgLibraries    vcpkg libraries linked statically into Lucent.exe and its helper
                                  executables (lucent-worker.exe, W7) (build.ninja, lib).
    - Get-AssetLicences           the licence of every bundled data asset: Lucent's own (MIT), or the
                                  third-party terms installer\license-policy.json names for it, with
                                  the licence text the verified asset carries and its provenance
                                  record (the Lensfun lens database, W13, ADR 0023).
    - New-LicenseBundle           writes licenses\ (Lucent's licence, each shipped port's copyright
                                  file, licence texts missing from vcpkg, the third-party code inside
                                  Qt from Qt's own SBOM, the licence texts of third-party data assets,
                                  notices file, which begins with the terms of Lucent's logo, ADR 0025).
    - New-CorrespondingSourcePackage, Test-CorrespondingSourcePackage
                                  write and independently check Lucent-<version>-corresponding-
                                  source.zip: the upstream source archives, vcpkg recipes, triplet
                                  and build records of every shipped library whose licence needs a
                                  source provision (Get-SourceProvisionPlan: the LGPL libraries; a
                                  copyleft licence the policy does not provide for stops packaging,
                                  and GPL code or copyleft code compiled into the programs stops it
                                  whatever the policy provides; Get-LicenceKind), as a deterministic
                                  archive (New-DeterministicZip; every order ordinal,
                                  ConvertTo-OrdinalOrder) with every patch a recipe applies to its
                                  library's source (Get-RecipeModifications) (W12, ADR 0024).
    - Write-LgplNotice            writes licenses\LGPL-NOTICE.txt, which names that archive and each
                                  library's modifications.
    - Get-InnoSetupLicence, Test-InnoSetupLicence
                                  the licence an Inno Setup compiler's banner states, and whether it
                                  covers the distribution the policy records (ADR 0024).
    - New-PackageSbom             aggregates vcpkg's per-port SPDX documents and Qt's SBOMs into one
                                  SPDX 2.3 SBOM, with a package for each third-party data asset and
                                  the terms of Lucent's logo inside its programs, and checks it
                                  structurally.
    - Write-InstalledFileList     writes Lucent-files.txt, the list the next Setup's upgrade uses.
    - Get-BuilderPathRules, New-BuilderPathScanner, Invoke-BuilderPathAudit
                                  scan every shipped file (and the installer, and the package
                                  manifest) for the build machine's folders: the user profile, the
                                  repository, the build tree and the vcpkg folders (W12), in their
                                  bytes and in what they compress: every zlib and gzip stream (Qt's
                                  rcc resources), and Setup's LZMA-compressed setup data, setup
                                  engine and installed files (Find-BuilderPathsInContent,
                                  LucentCompressedContent.cs; W12 review SEC-07), within one
                                  decompression budget per scanned file (New-CompressedContentBudget).
    - Get-CodeSigningCertificate, Test-AuthenticodeFile
                                  check the signing certificate before signing and every signature
                                  after it (signer thumbprint, offline WinVerifyTrust result,
                                  timestamp) (W12).
    - Find-InnoSetupCompiler      locates ISCC.exe.
    - Get-FileInventory           lists files with size and SHA-256 for the package manifest.

    Windows PowerShell 5.1 compatible. Keep this file ASCII: PowerShell 5.1 reads BOM-less scripts in
    the ANSI code page.
#>

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

$script:Utf8NoBom = New-Object System.Text.UTF8Encoding($false)

# The list of every file the package installs, at the root of the installation folder. The next
# version's Setup reads it to delete exactly the files that version no longer ships (ADR 0009,
# decision 8). installer\lucent.iss and installer\tests\Test-InstalledPackage.ps1 use the same name.
$script:InstalledFileListName = 'Lucent-files.txt'
$script:InstalledFileListHeader = '; Lucent installed files, format 1'

# --- Small helpers -------------------------------------------------------------------------------

function Write-TextFile {
    # Writes UTF-8 without a BOM, with CRLF line endings (Notepad-friendly licence texts).
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][AllowEmptyString()][string]$Text,
          [switch]$KeepLineEndings)
    $directory = Split-Path -Parent $Path
    if ($directory -and -not (Test-Path -LiteralPath $directory)) {
        New-Item -ItemType Directory -Path $directory -Force | Out-Null
    }
    if (-not $KeepLineEndings) {
        $Text = ($Text -replace "`r`n", "`n") -replace "`n", "`r`n"
    }
    [System.IO.File]::WriteAllText($Path, $Text, $script:Utf8NoBom)
}

function Write-JsonFile {
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)]$Value)
    $json = ConvertTo-Json -InputObject $Value -Depth 32
    Write-TextFile -Path $Path -Text ($json + "`n") -KeepLineEndings
}

function Get-Sha256 {
    param([Parameter(Mandatory)][string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-Sha1 {
    param([Parameter(Mandatory)][string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA1).Hash.ToLowerInvariant()
}

function Get-RelativePathText {
    # Relative path of $Path below $Root with backslashes (PowerShell 5.1 has no GetRelativePath).
    param([Parameter(Mandatory)][string]$Root, [Parameter(Mandatory)][string]$Path)
    $rootFull = [System.IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
    $full = [System.IO.Path]::GetFullPath($Path)
    if (-not $full.StartsWith($rootFull, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "$Path is not below $Root"
    }
    return $full.Substring($rootFull.Length)
}

function ConvertTo-NativePath {
    param([Parameter(Mandatory)][string]$Path)
    return $Path.Replace('/', '\')
}

function Format-WrappedList {
    # "Prefix: a, b, c" wrapped at about 100 columns, continuation lines aligned under the first item.
    param([Parameter(Mandatory)][string]$Prefix, [AllowEmptyCollection()][string[]]$Items, [int]$Width = 100)
    $lines = New-Object System.Collections.Generic.List[string]
    $indent = ' ' * $Prefix.Length
    $current = $Prefix
    for ($i = 0; $i -lt $Items.Count; $i++) {
        $item = $Items[$i] + $(if ($i -lt $Items.Count - 1) { ',' } else { '' })
        if ($current.Length -gt $indent.Length -and $current.Length + 1 + $item.Length -gt $Width -and $current -ne $Prefix) {
            $lines.Add($current.TrimEnd())
            $current = $indent
        }
        if ($current -eq $Prefix -or $current -eq $indent) { $current += $item } else { $current += ' ' + $item }
    }
    $lines.Add($current.TrimEnd())
    return $lines
}

function Format-Paragraph {
    # Word-wraps text at about 100 columns.
    param([Parameter(Mandatory)][string]$Text, [int]$Width = 100)
    $lines = New-Object System.Collections.Generic.List[string]
    $current = ''
    foreach ($word in ($Text -split '\s+' | Where-Object { $_ })) {
        if ($current -and $current.Length + 1 + $word.Length -gt $Width) { $lines.Add($current); $current = $word }
        elseif ($current) { $current += ' ' + $word }
        else { $current = $word }
    }
    if ($current) { $lines.Add($current) }
    return ($lines -join "`n")
}

function ConvertTo-OrdinalOrder {
    # $Items in the ordinal order of their -Property (or of themselves), the same on every machine:
    # Sort-Object sorts by the thread's culture, which reorders names between regional formats
    # (Estonian puts z between s and t), and the corresponding-source archive and the notices must not
    # depend on it (W12 review LIC-7). Stable (equal keys keep their order); -Unique keeps the first
    # item of each key.
    param([AllowNull()][AllowEmptyCollection()][object[]]$Items, [string]$Property, [switch]$Unique)
    $list = @($Items | Where-Object { $null -ne $_ })
    if ($list.Count -eq 0) { return @() }
    $keys = New-Object 'string[]' $list.Count
    $byKey = @{}
    for ($i = 0; $i -lt $list.Count; $i++) {
        $key = if ($Property) { [string]$list[$i].$Property } else { [string]$list[$i] }
        # The index after a NUL keeps equal keys in their order and every key unique. (Only the keys are
        # sorted, in place: PowerShell would pass a copy of an items array to Array.Sort's generic form.)
        $keys[$i] = $key + [char]0 + $i.ToString('D9', [System.Globalization.CultureInfo]::InvariantCulture)
        $byKey[$keys[$i]] = $list[$i]
    }
    [Array]::Sort($keys, [System.StringComparer]::Ordinal)
    $result = New-Object System.Collections.Generic.List[object]
    $previous = $null
    for ($i = 0; $i -lt $keys.Count; $i++) {
        $key = $keys[$i].Substring(0, $keys[$i].LastIndexOf([char]0))
        if ($Unique -and $i -gt 0 -and [string]::Equals($key, $previous, [System.StringComparison]::Ordinal)) { continue }
        $result.Add($byKey[$keys[$i]])
        $previous = $key
    }
    return $result.ToArray()
}

function Get-PortBaseVersion {
    # "6.11.1#3" -> "6.11.1"
    param([string]$Version)
    if (-not $Version) { return '' }
    return ($Version -split '#')[0]
}

function Invoke-NativeCommand {
    <#
    .SYNOPSIS
        Runs a program and returns its exit code and combined output as text. Windows PowerShell 5.1
        turns redirected stderr lines into errors, which would stop a script running with
        $ErrorActionPreference = 'Stop'; this keeps them as plain text.
    #>
    param([Parameter(Mandatory)][string]$FilePath, [string[]]$Arguments = @(), [string]$LogPath)
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $output = @(& $FilePath @Arguments 2>&1 | ForEach-Object { "$_" })
        $code = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previous
    }
    if ($LogPath) { Write-TextFile -Path $LogPath -Text (($output -join "`n") + "`n") }
    return [pscustomobject]@{ ExitCode = $code; Output = $output }
}

# --- Build facts ---------------------------------------------------------------------------------

function Read-LucentPackageInfo {
    <#
    .SYNOPSIS
        Reads the packaging facts CMake wrote for a build tree (cmake/LucentPackaging.cmake).
    #>
    param([Parameter(Mandatory)][string]$BuildDir)
    $path = Join-Path $BuildDir 'packaging\lucent-package.json'
    if (-not (Test-Path -LiteralPath $path)) {
        throw "$path is missing: configure the build with the release preset first (tools\build.ps1 -Preset release)."
    }
    $info = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
    if ($info.schema -ne 6) { throw "$path has schema $($info.schema); this script understands schema 6. Reconfigure the build tree (tools\build.ps1 -Preset release)." }
    # The update channel compiled into Lucent.exe (schema 6, W12 review SEC-02; Get-LucentUpdateChannelRecord).
    if ($info.PSObject.Properties.Name -notcontains 'updateChannel' -or $null -eq $info.updateChannel) { throw "$path has no updateChannel; reconfigure the build tree." }
    # The build machine's folders that no shipped file may name (schema 5, W12; Get-BuilderPathRules).
    foreach ($field in 'userProfile', 'vcpkgRoot', 'vcpkgBuildtreesDir', 'vcpkgPackagesDir', 'vcpkgDownloadsDir') {
        if ($info.PSObject.Properties.Name -notcontains $field) { throw "$path has no $field; reconfigure the build tree." }
    }
    # The bundled data assets (schema 4, ADR 0018): each with the SHA-256 the build pinned for its
    # manifest, its folder in the source tree and the files it installs.
    foreach ($asset in @($info.assets)) {
        if (-not $asset) { continue }
        if ($asset.id -notmatch '^[a-z0-9]+(-[a-z0-9]+)*$') { throw "$path names an invalid asset id '$($asset.id)'." }
        if ($asset.manifestSha256 -notmatch '^[0-9a-f]{64}$') { throw "$path has no manifest pin for asset $($asset.id)." }
        if (-not (Test-Path -LiteralPath (ConvertTo-NativePath $asset.sourceDir) -PathType Container)) { throw "$path names a missing asset folder '$($asset.sourceDir)'." }
        if (@($asset.files).Count -lt 2 -or @($asset.files)[0] -ne 'manifest.json') { throw "$path lists no files for asset $($asset.id)." }
    }
    if ($info.version -notmatch '^\d+\.\d+\.\d+$') { throw "Unexpected version '$($info.version)' in $path." }
    foreach ($field in 'builtExecutable', 'vcpkgInstalledDir', 'dumpbin', 'qmlSourceDir') {
        $value = $info.$field
        if (-not $value -or -not (Test-Path -LiteralPath (ConvertTo-NativePath $value))) {
            throw "$path names a missing $field ('$value')."
        }
    }
    # Helper executables installed next to the application (lucent-worker.exe, W7): built, and named
    # once each.
    $helpers = @($info.helperExecutables)
    $builtHelpers = @($info.builtHelperExecutables)
    if ($helpers.Count -ne $builtHelpers.Count) { throw "$path lists $($helpers.Count) helper executables but $($builtHelpers.Count) built ones." }
    for ($i = 0; $i -lt $helpers.Count; ++$i) {
        if ([System.IO.Path]::GetFileName((ConvertTo-NativePath $builtHelpers[$i])) -ine $helpers[$i]) {
            throw "$path pairs helper $($helpers[$i]) with $($builtHelpers[$i])."
        }
        if (-not (Test-Path -LiteralPath (ConvertTo-NativePath $builtHelpers[$i]))) {
            throw "The helper executable $($builtHelpers[$i]) has not been built."
        }
    }
    return $info
}

# --- Portable executable inspection -------------------------------------------------------------

function Get-PeHeader {
    <#
    .SYNOPSIS
        Machine and subsystem of a PE file, read from its headers.
    #>
    param([Parameter(Mandatory)][string]$Path)
    $stream = [System.IO.File]::OpenRead($Path)
    try {
        $reader = New-Object System.IO.BinaryReader($stream)
        if ($reader.ReadUInt16() -ne 0x5A4D) { throw "$Path is not a PE file (no MZ header)." }
        $stream.Position = 0x3C
        $peOffset = $reader.ReadInt32()
        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x00004550) { throw "$Path is not a PE file (no PE signature)." }
        $machine = $reader.ReadUInt16()
        $stream.Position = $peOffset + 24          # optional header
        $magic = $reader.ReadUInt16()
        $stream.Position = $peOffset + 24 + 68     # Subsystem (same offset for PE32 and PE32+)
        $subsystem = $reader.ReadUInt16()
    }
    finally {
        $stream.Dispose()
    }
    $machineName = switch ($machine) { 0x8664 { 'x64' } 0x014C { 'x86' } 0xAA64 { 'arm64' } default { ('0x{0:X4}' -f $machine) } }
    $subsystemName = switch ($subsystem) { 2 { 'WindowsGui' } 3 { 'WindowsCui' } default { "$subsystem" } }
    return [pscustomobject]@{ Machine = $machineName; Pe32Plus = ($magic -eq 0x20B); Subsystem = $subsystemName }
}

function Get-PeDependencies {
    <#
    .SYNOPSIS
        Imported and delay-loaded DLL names of PE files, from dumpbin /dependents.
    .OUTPUTS
        Hashtable: lower-case full path -> @{ Imports = string[]; DelayImports = string[] }.
    #>
    param([Parameter(Mandatory)][string]$Dumpbin, [Parameter(Mandatory)][string[]]$Paths)
    $result = @{}
    $batchSize = 40
    for ($start = 0; $start -lt $Paths.Count; $start += $batchSize) {
        $batch = @($Paths[$start..([Math]::Min($start + $batchSize, $Paths.Count) - 1)])
        $output = & $Dumpbin /nologo /dependents @batch
        if ($LASTEXITCODE -ne 0) { throw "dumpbin failed ($LASTEXITCODE) on: $($batch -join ', ')" }
        $current = $null
        $mode = $null
        foreach ($line in $output) {
            if ($line -match '^Dump of file (.+?)\s*$') {
                $current = @{ Imports = New-Object System.Collections.Generic.List[string];
                              DelayImports = New-Object System.Collections.Generic.List[string] }
                $result[[System.IO.Path]::GetFullPath($Matches[1]).ToLowerInvariant()] = $current
                $mode = $null
            }
            elseif ($line -match '^\s+Image has the following dependencies:') { $mode = 'Imports' }
            elseif ($line -match '^\s+Image has the following delay load dependencies:') { $mode = 'DelayImports' }
            elseif ($line -match '^\s+Summary\s*$') { $mode = $null }
            elseif ($mode -and $current -and $line -match '^\s{4}(\S+)\s*$') { $current[$mode].Add($Matches[1]) }
        }
    }
    foreach ($path in $Paths) {
        if (-not $result.ContainsKey([System.IO.Path]::GetFullPath($path).ToLowerInvariant())) {
            throw "dumpbin reported nothing for $path"
        }
    }
    return $result
}

# --- vcpkg provenance ----------------------------------------------------------------------------

function Get-VcpkgFileIndex {
    <#
    .SYNOPSIS
        Maps every file vcpkg installed (relative to the triplet folder, lower case, forward slashes)
        to its port, from vcpkg_installed\vcpkg\info\<port>_<version>_<triplet>.list.
    #>
    param([Parameter(Mandatory)][string]$InstalledDir, [Parameter(Mandatory)][string]$Triplet)
    $infoDir = Join-Path $InstalledDir 'vcpkg\info'
    $index = @{}
    $suffix = "_$Triplet.list"
    foreach ($list in Get-ChildItem -LiteralPath $infoDir -Filter "*$suffix") {
        $stem = $list.Name.Substring(0, $list.Name.Length - $suffix.Length)
        $separator = $stem.IndexOf('_')
        if ($separator -lt 1) { throw "Unexpected vcpkg list file name $($list.Name)" }
        $port = $stem.Substring(0, $separator)
        $version = $stem.Substring($separator + 1)
        foreach ($line in [System.IO.File]::ReadAllLines($list.FullName)) {
            if (-not $line.StartsWith("$Triplet/") -or $line.EndsWith('/')) { continue }
            $relative = $line.Substring($Triplet.Length + 1)
            $index[$relative.ToLowerInvariant()] = [pscustomobject]@{ Port = $port; Version = $version; Path = $relative }
        }
    }
    return $index
}

function Get-StagedFileOrigins {
    <#
    .SYNOPSIS
        Classifies every staged file: Lucent's executable or one of its helper executables
        (lucent-worker.exe), a generated file, or a file of a vcpkg port (compared byte for byte with
        the vcpkg installation). Anything else is 'unknown'.
    #>
    param([Parameter(Mandatory)][string]$StageDir, [Parameter(Mandatory)]$Info, [Parameter(Mandatory)][hashtable]$FileIndex)
    $tripletDir = Join-Path (ConvertTo-NativePath $Info.vcpkgInstalledDir) $Info.vcpkgTriplet
    $origins = New-Object System.Collections.Generic.List[object]
    # The bundled data assets (ADR 0018): assets\<id>\<file> comes from the source tree's assets folder.
    $assetSources = @{}
    foreach ($asset in @($Info.assets)) {
        if (-not $asset) { continue }
        foreach ($file in @($asset.files)) {
            $relativeFile = ConvertTo-NativePath $file
            $assetSources[("assets\$($asset.id)\$relativeFile").ToLowerInvariant()] = Join-Path (ConvertTo-NativePath $asset.sourceDir) $relativeFile
        }
    }
    $helperSources = @{}
    $helperNames = @($Info.helperExecutables)
    $helperBuilt = @($Info.builtHelperExecutables)
    for ($i = 0; $i -lt $helperNames.Count; ++$i) { $helperSources[$helperNames[$i].ToLowerInvariant()] = ConvertTo-NativePath $helperBuilt[$i] }
    foreach ($file in Get-ChildItem -LiteralPath $StageDir -Recurse -File) {
        $relative = Get-RelativePathText -Root $StageDir -Path $file.FullName
        $segments = $relative.Split('\')
        $entry = [ordered]@{
            Path = $relative; Size = $file.Length; Sha256 = (Get-Sha256 $file.FullName)
            Origin = 'unknown'; Port = $null; PortVersion = $null; Source = $null; Identical = $null
        }
        if ($segments.Count -eq 1 -and $relative -ieq $Info.executable) {
            $entry.Origin = 'lucent'
            $entry.Source = ConvertTo-NativePath $Info.builtExecutable
            $entry.Identical = ($entry.Sha256 -eq (Get-Sha256 $entry.Source))
        }
        elseif ($segments.Count -eq 1 -and $helperSources.ContainsKey($relative.ToLowerInvariant())) {
            $entry.Origin = 'lucent'
            $entry.Source = $helperSources[$relative.ToLowerInvariant()]
            $entry.Identical = ($entry.Sha256 -eq (Get-Sha256 $entry.Source))
        }
        elseif ($segments.Count -eq 1 -and ($relative -ieq 'qt.conf' -or $relative -ieq $script:InstalledFileListName)) {
            $entry.Origin = 'generated'
        }
        elseif ($segments[0] -ieq 'licenses') {
            $entry.Origin = 'generated'
        }
        elseif ($segments[0] -ieq 'assets' -and $assetSources.ContainsKey($relative.ToLowerInvariant())) {
            $entry.Origin = 'asset'
            $entry.Source = $assetSources[$relative.ToLowerInvariant()]
            $entry.Identical = ($entry.Sha256 -eq (Get-Sha256 $entry.Source))
        }
        else {
            $vcpkgRelative = $null
            if ($segments.Count -eq 1) {
                $vcpkgRelative = "$($Info.qtBinDir)/$relative"
            }
            elseif ($segments[0] -ieq 'plugins') {
                $vcpkgRelative = "$($Info.qtPluginsDir)/" + (($segments[1..($segments.Count - 1)]) -join '/')
            }
            elseif ($segments[0] -ieq 'qml') {
                $vcpkgRelative = "$($Info.qtQmlDir)/" + (($segments[1..($segments.Count - 1)]) -join '/')
            }
            if ($vcpkgRelative) {
                $owner = $FileIndex[$vcpkgRelative.ToLowerInvariant()]
                if ($owner) {
                    $entry.Origin = 'vcpkg'
                    $entry.Port = $owner.Port
                    $entry.PortVersion = $owner.Version
                    $entry.Source = Join-Path $tripletDir (ConvertTo-NativePath $owner.Path)
                    $entry.Identical = ($entry.Sha256 -eq (Get-Sha256 $entry.Source))
                }
            }
        }
        $origins.Add([pscustomobject]$entry)
    }
    return $origins
}

# --- Audit ---------------------------------------------------------------------------------------

function Get-WindowsDllInfo {
    # A DLL that Windows itself provides: present in System32 and signed (through the Windows
    # catalogs) by "Microsoft Windows". The Visual C++ runtime DLLs there are signed by another
    # Microsoft publisher, and third-party DLLs copied into System32 are not signed by Windows.
    param([Parameter(Mandatory)][string]$Name, [Parameter(Mandatory)][hashtable]$Cache)
    $key = $Name.ToLowerInvariant()
    if ($Cache.ContainsKey($key)) { return $Cache[$key] }
    $path = Join-Path $env:SystemRoot "System32\$Name"
    $info = $null
    if (Test-Path -LiteralPath $path -PathType Leaf) {
        $version = (Get-Item -LiteralPath $path).VersionInfo
        $signature = Get-AuthenticodeSignature -LiteralPath $path
        $signer = if ($signature.SignerCertificate) { $signature.SignerCertificate.Subject } else { '' }
        $signedByWindows = ($signature.Status -eq 'Valid') -and ($signer -match '^CN=Microsoft Windows,')
        $info = [pscustomobject]@{ Name = $Name; Path = $path; Product = $version.ProductName; Company = $version.CompanyName
                                   FileVersion = $version.FileVersion; Signer = $signer; IsWindows = $signedByWindows }
    }
    $Cache[$key] = $info
    return $info
}

function Get-QmlSourceImports {
    # Module names imported by the application's QML sources ("import QtQuick.Controls").
    param([Parameter(Mandatory)][string]$QmlSourceDir)
    $modules = @{}
    foreach ($file in Get-ChildItem -LiteralPath $QmlSourceDir -Recurse -File -Include '*.qml', '*.js', '*.mjs') {
        foreach ($line in [System.IO.File]::ReadAllLines($file.FullName)) {
            if ($line -match '^\s*import\s+([A-Za-z_][A-Za-z0-9_]*(\.[A-Za-z_][A-Za-z0-9_]*)*)(\s|;|$)') {
                if (-not $modules.ContainsKey($Matches[1])) { $modules[$Matches[1]] = $file.Name }
            }
        }
    }
    return $modules
}

function Test-QmlLayout {
    <#
    .SYNOPSIS
        Checks the deployed QML modules. Qt loads QML plugins by name at run time, so no import table
        shows a missing one. Returns error texts.
    .DESCRIPTION
        - Every module the application's QML sources import, when Qt provides it (vcpkg's Qt qml
          folder has it; Lucent's own module is compiled into Lucent.exe), and every module in
          Info.requiredQmlModules (the style selected in code) has its qmldir in qml\.
        - From those modules, every non-optional "import" and "depends" of each qmldir is followed
          and must be deployed too.
        - Every qmldir in qml\ that names a plugin ("plugin" or "optional plugin") has that DLL.
    #>
    param([Parameter(Mandatory)][string]$StageDir, [Parameter(Mandatory)]$Info)
    $errors = New-Object System.Collections.Generic.List[string]
    $qmlRoot = Join-Path $StageDir 'qml'
    $qtQml = Join-Path (Join-Path (ConvertTo-NativePath $Info.vcpkgInstalledDir) $Info.vcpkgTriplet) (ConvertTo-NativePath $Info.qtQmlDir)

    function Get-ModuleQmldir([string]$Root, [string]$Module) { return Join-Path (Join-Path $Root ($Module.Replace('.', '\'))) 'qmldir' }

    # Plugin DLLs of every deployed module.
    if (Test-Path -LiteralPath $qmlRoot) {
        foreach ($qmldir in Get-ChildItem -LiteralPath $qmlRoot -Recurse -File -Filter 'qmldir') {
            foreach ($line in [System.IO.File]::ReadAllLines($qmldir.FullName)) {
                if ($line -match '^\s*(optional\s+)?plugin\s+(\S+)(\s+(\S+))?\s*$') {
                    $folder = if ($Matches[4]) { Join-Path $qmldir.DirectoryName (ConvertTo-NativePath $Matches[4]) } else { $qmldir.DirectoryName }
                    $dll = Join-Path $folder "$($Matches[2]).dll"
                    if (-not (Test-Path -LiteralPath $dll -PathType Leaf)) {
                        $errors.Add("$(Get-RelativePathText -Root $StageDir -Path $qmldir.FullName) names the plugin $($Matches[2]), but $(Get-RelativePathText -Root $StageDir -Path $dll) is missing")
                    }
                }
            }
        }
    }

    # Required modules and their imports.
    $pending = New-Object System.Collections.Generic.Queue[object]
    foreach ($entry in (Get-QmlSourceImports -QmlSourceDir (ConvertTo-NativePath $Info.qmlSourceDir)).GetEnumerator()) {
        if (Test-Path -LiteralPath (Get-ModuleQmldir $qtQml $entry.Key)) {
            $pending.Enqueue([pscustomobject]@{ Module = $entry.Key; By = "imported by $($entry.Value)" })
        }
    }
    foreach ($module in @($Info.requiredQmlModules)) {
        if ($module) { $pending.Enqueue([pscustomobject]@{ Module = $module; By = 'selected in code (requiredQmlModules)' }) }
    }
    $seen = @{}
    while ($pending.Count -gt 0) {
        $item = $pending.Dequeue()
        if ($seen.ContainsKey($item.Module)) { continue }
        $seen[$item.Module] = $true
        $qmldir = Get-ModuleQmldir $qmlRoot $item.Module
        if (-not (Test-Path -LiteralPath $qmldir -PathType Leaf)) {
            $errors.Add("QML module $($item.Module) ($($item.By)) is missing: $(Get-RelativePathText -Root $StageDir -Path $qmldir)")
            continue
        }
        foreach ($line in [System.IO.File]::ReadAllLines($qmldir)) {
            if ($line -match '^\s*(default\s+)?(import|depends)\s+([A-Za-z_][A-Za-z0-9_.]*)') {
                $pending.Enqueue([pscustomobject]@{ Module = $Matches[3]; By = "$($Matches[2]) of $($item.Module)" })
            }
        }
    }
    return [pscustomobject]@{ Errors = $errors; Modules = @($seen.Keys | Sort-Object) }
}

function Invoke-LayoutAudit {
    <#
    .SYNOPSIS
        Audits the installed layout. Returns a report; its Errors list must be empty to package.
    .DESCRIPTION
        - Exactly the executables Lucent ships: Lucent.exe and its helpers (lucent-worker.exe, W7),
          each a GUI-subsystem program identical to the build output; every PE file is x64.
        - No debug or build artefacts (pdb, lib, exp, ilk, obj); no port listed as never shipped.
        - Every file is one of those executables (identical to the build output), a generated file, or a file of a
          vcpkg port that is byte-identical to the vcpkg installation.
        - Every import and delay-load import of every PE file resolves to a DLL in the folder, an API
          set, a Windows DLL (System32, signed by Microsoft Windows), or one of the Visual C++ runtime
          DLLs installed by Microsoft's redistributable. Anything else is an error. A DLL in the
          folder that nothing imports is an error too (a plugin that needed it is gone).
        - DLLs that Qt loads by name at run time: the required Qt plugins (Info.requiredPlugins), and
          the QML modules Lucent uses with their plugin DLLs (Test-QmlLayout).
        - qt.conf exists and holds only relative paths; no .conf/.ini file names a build-tree path.
          Every byte of every shipped file is scanned for the build machine's folders later, once
          the licences and the SBOM exist (Invoke-BuilderPathAudit, W12).
    #>
    param(
        [Parameter(Mandatory)][string]$StageDir,
        [Parameter(Mandatory)]$Info,
        [Parameter(Mandatory)]$Policy,
        [switch]$AllowSignedExecutable)

    $errors = New-Object System.Collections.Generic.List[string]
    $warnings = New-Object System.Collections.Generic.List[string]
    $StageDir = [System.IO.Path]::GetFullPath($StageDir)

    $index = Get-VcpkgFileIndex -InstalledDir (ConvertTo-NativePath $Info.vcpkgInstalledDir) -Triplet $Info.vcpkgTriplet
    $origins = Get-StagedFileOrigins -StageDir $StageDir -Info $Info -FileIndex $index

    # Layout and provenance.
    $executables = @($origins | Where-Object { $_.Path -like '*.exe' } | ForEach-Object { $_.Path.ToLowerInvariant() } | Sort-Object)
    $expectedExecutables = @(@($Info.executable) + @($Info.helperExecutables) | Where-Object { $_ } |
        ForEach-Object { $_.ToLowerInvariant() } | Sort-Object)
    if (($executables -join '|') -ne ($expectedExecutables -join '|')) {
        $errors.Add("Expected exactly the executables $($expectedExecutables -join ', '); found: $($executables -join ', ')")
    }
    foreach ($origin in $origins) {
        if ($origin.Path -match '\.(pdb|lib|exp|ilk|obj|iobj|ipdb)$') {
            $errors.Add("Build artefact in the layout: $($origin.Path)")
        }
        switch ($origin.Origin) {
            'unknown' { $errors.Add("File of unknown origin (neither Lucent's build nor a vcpkg port): $($origin.Path)") }
            'vcpkg' {
                if (-not $origin.Identical) { $errors.Add("$($origin.Path) differs from the vcpkg file $($origin.Source)") }
                $reason = $null
                if ($Policy.neverShipped.PSObject.Properties.Name -contains $origin.Port) {
                    $reason = $Policy.neverShipped.($origin.Port)
                }
                if ($reason) { $errors.Add("$($origin.Path) belongs to port $($origin.Port), which must never ship: $reason") }
            }
            'lucent' {
                if (-not $origin.Identical -and -not $AllowSignedExecutable) {
                    $errors.Add("$($origin.Path) differs from the build output $($origin.Source)")
                }
            }
            'asset' {
                if (-not $origin.Identical) { $errors.Add("$($origin.Path) differs from the source tree's asset file $($origin.Source)") }
            }
        }
    }

    # Bundled data assets: verified as Lucent.exe verifies them before use.
    $assetCheck = Test-BundledAssets -StageDir $StageDir -Info $Info
    foreach ($problem in $assetCheck.Errors) { $errors.Add($problem) }

    # Portable executables.
    $peFiles = @($origins | Where-Object { $_.Path -match '\.(exe|dll)$' })
    $pePaths = @($peFiles | ForEach-Object { Join-Path $StageDir $_.Path })
    $dependencies = Get-PeDependencies -Dumpbin (ConvertTo-NativePath $Info.dumpbin) -Paths $pePaths
    $rootDlls = @{}
    foreach ($file in Get-ChildItem -LiteralPath $StageDir -File -Filter '*.dll') { $rootDlls[$file.Name.ToLowerInvariant()] = $file.Name }
    $vcRuntime = @{}
    foreach ($name in @($Info.vcRuntimeDlls)) { if ($name) { $vcRuntime[$name.ToLowerInvariant()] = $true } }
    if ($vcRuntime.Count -eq 0) { $errors.Add('The build recorded no Visual C++ runtime DLL names (vcRuntimeDlls); configure from the MSVC environment.') }

    $windowsCache = @{}
    $classes = @{}              # import name (lower) -> class
    $importedBy = @{}           # root DLL name (lower) -> count of importers
    $peReport = New-Object System.Collections.Generic.List[object]
    foreach ($pe in $peFiles) {
        $full = Join-Path $StageDir $pe.Path
        $header = Get-PeHeader -Path $full
        if ($header.Machine -ne 'x64' -or -not $header.Pe32Plus) { $errors.Add("$($pe.Path) is not an x64 image ($($header.Machine))") }
        if ($pe.Path -like '*.exe' -and $header.Subsystem -ne 'WindowsGui') {
            $errors.Add("$($pe.Path) is not a Windows GUI program (subsystem $($header.Subsystem))")
        }
        $deps = $dependencies[$full.ToLowerInvariant()]
        foreach ($kind in 'Imports', 'DelayImports') {
            foreach ($name in $deps[$kind]) {
                $lower = $name.ToLowerInvariant()
                $class = $null
                if ($rootDlls.ContainsKey($lower)) {
                    $class = 'package'
                    $importedBy[$lower] = 1 + [int]$importedBy[$lower]
                }
                elseif ($lower -match '^(api|ext)-ms-') { $class = 'apiset' }
                elseif ($vcRuntime.ContainsKey($lower)) { $class = 'vcruntime' }
                else {
                    $windows = Get-WindowsDllInfo -Name $name -Cache $windowsCache
                    if ($windows -and $windows.IsWindows) { $class = 'windows' }
                }
                if (-not $class) {
                    $what = if ($kind -eq 'DelayImports') { 'delay-loads' } else { 'imports' }
                    $errors.Add("$($pe.Path) $what $name, which is not in the package, not part of Windows and not a Visual C++ runtime DLL")
                    $class = 'unresolved'
                }
                $classes[$lower] = $class
            }
        }
        $peReport.Add([pscustomobject]@{
            Path = $pe.Path; Machine = $header.Machine; Subsystem = $header.Subsystem
            Imports = @($deps.Imports); DelayImports = @($deps.DelayImports) })
    }
    foreach ($lower in $rootDlls.Keys) {
        if (-not $importedBy.ContainsKey($lower)) {
            $errors.Add("$($rootDlls[$lower]) is in the folder but nothing imports it (a plugin or QML module that used it is missing, or it is not needed)")
        }
        $windows = Get-WindowsDllInfo -Name $rootDlls[$lower] -Cache $windowsCache
        if ($windows) { $warnings.Add("$($rootDlls[$lower]) has the same name as a DLL in System32 ($($windows.Product))") }
    }

    # DLLs loaded by name at run time: Qt plugins and QML modules.
    foreach ($plugin in @($Info.requiredPlugins)) {
        if ($plugin -and -not (Test-Path -LiteralPath (Join-Path $StageDir ('plugins\' + (ConvertTo-NativePath $plugin))) -PathType Leaf)) {
            $errors.Add("The required Qt plugin plugins\$(ConvertTo-NativePath $plugin) is missing")
        }
    }
    $qml = Test-QmlLayout -StageDir $StageDir -Info $Info
    foreach ($problem in $qml.Errors) { $errors.Add($problem) }

    # qt.conf and other configuration files.
    $qtConf = Join-Path $StageDir 'qt.conf'
    if (-not (Test-Path -LiteralPath $qtConf)) { $errors.Add('qt.conf is missing') }
    $buildPaths = @($Info.sourceDir, $Info.binaryDir, $Info.vcpkgInstalledDir) | Where-Object { $_ } |
        ForEach-Object { $_; (ConvertTo-NativePath $_) }
    foreach ($config in (Get-ChildItem -LiteralPath $StageDir -Recurse -File | Where-Object { $_.Extension -in '.conf', '.ini' })) {
        $text = Get-Content -LiteralPath $config.FullName -Raw
        foreach ($buildPath in $buildPaths) {
            if ($text.IndexOf($buildPath, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
                $errors.Add("$($config.Name) names a build-tree path ($buildPath)")
            }
        }
        foreach ($line in ($text -split "`r?`n")) {
            if ($line -match '^\s*[^;#].*=\s*(.*)$' -and ($Matches[1] -match '^[A-Za-z]:[\\/]|^[\\/]{2}|^\.\.')) {
                $errors.Add("$($config.Name) holds a path outside the installation folder: $($line.Trim())")
            }
        }
    }

    $windowsDlls = @($windowsCache.Values | Where-Object { $_ -and $_.IsWindows -and $classes[$_.Name.ToLowerInvariant()] -eq 'windows' } |
        Sort-Object Name)
    $vcImports = @($classes.Keys | Where-Object { $classes[$_] -eq 'vcruntime' } | Sort-Object)
    $ports = @($origins | Where-Object { $_.Origin -eq 'vcpkg' } | ForEach-Object Port | Sort-Object -Unique)
    return [pscustomobject]@{
        StageDir = $StageDir
        Errors = $errors
        Warnings = $warnings
        Files = $origins
        PeFiles = $peReport
        PackageDlls = @($rootDlls.Values | Sort-Object)
        WindowsDlls = $windowsDlls
        VcRuntimeImports = $vcImports
        ApiSetImports = @($classes.Keys | Where-Object { $classes[$_] -eq 'apiset' } | Sort-Object)
        Ports = $ports
        QmlModules = $qml.Modules
        Assets = $assetCheck.Assets
    }
}

function Test-BundledAssets {
    <#
    .SYNOPSIS
        Verifies every bundled data asset in the layout the way Lucent.exe does before it activates
        one (src/assets/include/lucent/assets/asset_store.hpp, ADR 0018): the manifest's SHA-256 is
        the pin the build recorded, the manifest is a "lucent.asset-manifest" version 1 for that
        asset, and every file it lists exists with exactly the listed size and SHA-256. The asset's
        folder holds nothing else, and assets\ holds no folder that is not a bundled asset.
    #>
    param([Parameter(Mandatory)][string]$StageDir, [Parameter(Mandatory)]$Info)
    $errors = New-Object System.Collections.Generic.List[string]
    $verified = New-Object System.Collections.Generic.List[object]
    $root = Join-Path $StageDir 'assets'
    $expected = @{}
    foreach ($asset in @($Info.assets)) {
        if (-not $asset) { continue }
        $expected[$asset.id.ToLowerInvariant()] = $true
        $folder = Join-Path $root $asset.id
        $manifestPath = Join-Path $folder 'manifest.json'
        if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { $errors.Add("assets\$($asset.id)\manifest.json is missing"); continue }
        $manifestSha = Get-Sha256 $manifestPath
        if ($manifestSha -ne $asset.manifestSha256) {
            $errors.Add("assets\$($asset.id)\manifest.json has SHA-256 $manifestSha, but the build pinned $($asset.manifestSha256)")
        }
        $manifest = $null
        try { $manifest = [System.IO.File]::ReadAllText($manifestPath) | ConvertFrom-Json }
        catch { $errors.Add("assets\$($asset.id)\manifest.json is not JSON: $($_.Exception.Message)"); continue }
        if ($manifest.schema -ne 'lucent.asset-manifest' -or $manifest.schema_version -ne 1) {
            $errors.Add("assets\$($asset.id)\manifest.json is not a lucent.asset-manifest version 1"); continue
        }
        if ($manifest.id -ne $asset.id) { $errors.Add("assets\$($asset.id)\manifest.json describes asset '$($manifest.id)'") }
        $listed = @{ 'manifest.json' = $true }
        foreach ($file in @($manifest.files)) {
            $relativeFile = ConvertTo-NativePath ([string]$file.path)
            $listed[$relativeFile.ToLowerInvariant()] = $true
            $path = Join-Path $folder $relativeFile
            if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { $errors.Add("assets\$($asset.id)\$relativeFile is listed by its manifest but missing"); continue }
            $length = (Get-Item -LiteralPath $path).Length
            $sha = Get-Sha256 $path
            if ($length -ne [int64]$file.size -or $sha -ne $file.sha256) {
                $errors.Add("assets\$($asset.id)\$relativeFile does not match its manifest: $length bytes with SHA-256 $sha, listed $($file.size) bytes with SHA-256 $($file.sha256)")
            }
        }
        foreach ($present in @(Get-ChildItem -LiteralPath $folder -Recurse -File)) {
            $relativeFile = Get-RelativePathText -Root $folder -Path $present.FullName
            if (-not $listed.ContainsKey($relativeFile.ToLowerInvariant())) { $errors.Add("assets\$($asset.id)\$relativeFile is not listed by the asset's manifest") }
        }
        $verified.Add([pscustomobject]@{ Id = $asset.id; Version = $manifest.version; Files = @($manifest.files).Count; ManifestSha256 = $manifestSha })
    }
    if (Test-Path -LiteralPath $root) {
        foreach ($entry in @(Get-ChildItem -LiteralPath $root)) {
            if (-not $entry.PSIsContainer -or -not $expected.ContainsKey($entry.Name.ToLowerInvariant())) {
                $errors.Add("assets\$($entry.Name) is not a bundled asset of this build")
            }
        }
    }
    if ($expected.Count -gt 0 -and -not (Test-Path -LiteralPath $root)) { $errors.Add('The assets folder is missing') }
    # An array, not the List[object]: @() of a List[object] fails in Windows PowerShell 5.1.
    return [pscustomobject]@{ Errors = $errors; Assets = $verified.ToArray() }
}

function Write-AuditReport {
    param([Parameter(Mandatory)]$Report, [Parameter(Mandatory)][string]$TextPath, [Parameter(Mandatory)][string]$JsonPath)
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add('Lucent package audit')
    $lines.Add("Layout: $($Report.StageDir)")
    $lines.Add("Files: $($Report.Files.Count); PE files: $($Report.PeFiles.Count); errors: $($Report.Errors.Count); warnings: $($Report.Warnings.Count)")
    $lines.Add('')
    $lines.Add('Errors:')
    if ($Report.Errors.Count -eq 0) { $lines.Add('  (none)') } else { foreach ($e in $Report.Errors) { $lines.Add("  $e") } }
    $lines.Add('Warnings:')
    if ($Report.Warnings.Count -eq 0) { $lines.Add('  (none)') } else { foreach ($w in $Report.Warnings) { $lines.Add("  $w") } }
    $lines.Add('')
    $lines.Add('Shipped vcpkg ports: ' + ($Report.Ports -join ', '))
    $lines.Add('Visual C++ runtime imports (from the redistributable): ' + ($Report.VcRuntimeImports -join ', '))
    $lines.Add('Windows DLL imports:')
    foreach ($dll in $Report.WindowsDlls) { $lines.Add(('  {0,-28} {1} {2}' -f $dll.Name, $dll.FileVersion, $dll.Product)) }
    $lines.Add('API set imports: ' + ($Report.ApiSetImports -join ', '))
    $lines.Add('QML modules required (imports of the QML sources, the style selected in code, and their imports): ' + ($Report.QmlModules -join ', '))
    $assetsText = (@($Report.Assets) | ForEach-Object { "$($_.Id) $($_.Version) ($($_.Files) file(s))" }) -join ', '
    $lines.Add('Bundled assets (manifest pinned, every file verified): ' + $assetsText)
    $lines.Add('')
    $lines.Add('Files (origin, port, path):')
    foreach ($file in ($Report.Files | Sort-Object Path)) {
        $port = if ($file.Port) { $file.Port } else { '-' }
        $lines.Add(('  {0,-9} {1,-16} {2}' -f $file.Origin, $port, $file.Path))
    }
    $lines.Add('')
    $lines.Add('PE imports:')
    foreach ($pe in ($Report.PeFiles | Sort-Object Path)) {
        $lines.Add("  $($pe.Path) [$($pe.Machine), $($pe.Subsystem)]")
        $lines.Add('    imports: ' + ($pe.Imports -join ', '))
        if ($pe.DelayImports.Count -gt 0) { $lines.Add('    delay-loads: ' + ($pe.DelayImports -join ', ')) }
    }
    Write-TextFile -Path $TextPath -Text (($lines -join "`n") + "`n")
    Write-JsonFile -Path $JsonPath -Value ([ordered]@{
        errors = @($Report.Errors); warnings = @($Report.Warnings); ports = @($Report.Ports)
        vcRuntimeImports = @($Report.VcRuntimeImports); apiSetImports = @($Report.ApiSetImports); qmlModules = @($Report.QmlModules)
        assets = @($Report.Assets | ForEach-Object { [ordered]@{ id = $_.Id; version = $_.Version; files = $_.Files; manifestSha256 = $_.ManifestSha256 } })
        windowsDlls = @($Report.WindowsDlls | ForEach-Object { [ordered]@{ name = $_.Name; version = $_.FileVersion; product = $_.Product } })
        files = @($Report.Files | Sort-Object Path | ForEach-Object { [ordered]@{ path = $_.Path; origin = $_.Origin; port = $_.Port; sha256 = $_.Sha256 } })
        peFiles = @($Report.PeFiles | Sort-Object Path | ForEach-Object { [ordered]@{ path = $_.Path; machine = $_.Machine; subsystem = $_.Subsystem; imports = @($_.Imports); delayImports = @($_.DelayImports) } })
    })
}

# --- Smoke run -----------------------------------------------------------------------------------

function Get-FolderSnapshot {
    # Every file (path, size, time) and folder (path) below Root.
    param([Parameter(Mandatory)][string]$Root)
    return @(Get-ChildItem -LiteralPath $Root -Recurse -Force | Sort-Object FullName | ForEach-Object {
        $relative = Get-RelativePathText -Root $Root -Path $_.FullName
        if ($_.PSIsContainer) { "$relative\" } else { '{0}|{1}|{2}' -f $relative, $_.Length, $_.LastWriteTimeUtc.Ticks } })
}

function Invoke-SmokeRun {
    <#
    .SYNOPSIS
        Starts <stage>\Lucent.exe the way a clean machine would (PATH reduced to Windows' folders, no
        Qt variables, a temporary --data-root), waits for its window to settle, saves a screenshot
        and checks the exit code, Qt's and QML's warnings and that the folder was not changed. This
        catches what no import table shows: Qt plugins and QML modules loaded by name at run time.
    #>
    param([Parameter(Mandatory)][string]$StageDir, [Parameter(Mandatory)][string]$Executable,
          [Parameter(Mandatory)][string]$WorkDir, [int]$SettleTimeoutMs = 60000)
    if (Test-Path -LiteralPath $WorkDir) { Remove-Item -LiteralPath $WorkDir -Recurse -Force }
    New-Item -ItemType Directory -Path $WorkDir | Out-Null
    $exe = Join-Path $StageDir $Executable
    $screenshot = Join-Path $WorkDir 'smoke.png'
    $stderrPath = Join-Path $WorkDir 'stderr.log'
    $before = Get-FolderSnapshot -Root $StageDir
    $names = 'PATH', 'QT_PLUGIN_PATH', 'QML_IMPORT_PATH', 'QML2_IMPORT_PATH', 'QT_QPA_PLATFORM_PLUGIN_PATH', 'QT_QPA_PLATFORM',
             'QT_QUICK_CONTROLS_STYLE', 'QT_FORCE_STDERR_LOGGING', 'QML_DISABLE_DISK_CACHE'
    $saved = @{}
    foreach ($name in $names) { $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process'); [Environment]::SetEnvironmentVariable($name, $null, 'Process') }
    try {
        $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
        $env:QT_FORCE_STDERR_LOGGING = '1'
        $env:QML_DISABLE_DISK_CACHE = '1'
        $arguments = @('--data-root', "`"$(Join-Path $WorkDir 'data')`"", '--screenshot', "`"$screenshot`"", '--quit-after-screenshot',
                       '--settle-timeout', "$SettleTimeoutMs")
        $process = Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $WorkDir -PassThru -NoNewWindow `
            -RedirectStandardError $stderrPath -RedirectStandardOutput (Join-Path $WorkDir 'stdout.log')
    }
    finally {
        foreach ($name in $names) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') }
    }
    $null = $process.Handle
    $timedOut = -not $process.WaitForExit($SettleTimeoutMs + 60000)
    if ($timedOut) { Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue; $process.WaitForExit() }
    $stderr = if (Test-Path -LiteralPath $stderrPath) { @(Get-Content -LiteralPath $stderrPath) } else { @() }
    $problems = @($stderr | Where-Object { $_ -match 'module ".*" is not installed|Cannot load library|Could not (find|load) the Qt platform plugin|failed to load|qrc:.*(Error|error)|QQmlApplicationEngine failed|Unsupported image format|is not a type|ReferenceError|TypeError|camera support database unavailable' })
    # W12 review SEC-02: the update channel compiled into the staged Lucent.exe, which its developer
    # automation reports at start-up ("lucent: update channel ..."; app::describe_update_channel()).
    $channel = ConvertFrom-UpdateChannelLogLine -Lines $stderr
    # W10 (ADR 0018): the installed camera support database must be found and verified in the folder.
    $supportLines = @($stderr | Where-Object { $_ -match 'camera support database \S+ \(qualified' })
    $changed = @(Compare-Object -ReferenceObject $before -DifferenceObject (Get-FolderSnapshot -Root $StageDir) | ForEach-Object { $_.InputObject })
    $errors = New-Object System.Collections.Generic.List[string]
    if ($timedOut) { $errors.Add("$Executable did not exit within $(($SettleTimeoutMs + 60000) / 1000) s") }
    elseif ($process.ExitCode -ne 0) { $errors.Add("$Executable exited with $($process.ExitCode) (0 expected; see $stderrPath)") }
    if (-not (Test-Path -LiteralPath $screenshot)) { $errors.Add("$Executable saved no screenshot") }
    foreach ($line in $problems) { $errors.Add("Qt/QML problem: $line") }
    if (-not $timedOut -and $supportLines.Count -eq 0) { $errors.Add("$Executable did not report an active camera support database (see $stderrPath)") }
    elseif ($supportLines.Count -gt 0 -and $supportLines[0].IndexOf((Join-Path $StageDir 'assets'), [System.StringComparison]::OrdinalIgnoreCase) -lt 0) {
        $errors.Add("$Executable used a camera support database outside the folder: $($supportLines[0])")
    }
    foreach ($line in $changed) { $errors.Add("The run changed the installation folder: $line") }
    if (-not $timedOut -and -not $channel.Found) { $errors.Add("$Executable did not report its update channel (see $stderrPath)") }
    return [pscustomobject]@{ Errors = $errors; ExitCode = $(if ($timedOut) { $null } else { $process.ExitCode }); Screenshot = $screenshot; Log = $stderrPath
                              CameraSupport = $(if ($supportLines.Count -gt 0) { $supportLines[0] } else { $null })
                              UpdateChannelReported = $channel.Found; UpdateChannel = $channel.Channel }
}

# --- The update channel (W12 review SEC-02; ADR 0022) ---------------------------------------------
#
# A channel record: [pscustomobject]@{ name; manifestUrl; keyIds; publisherThumbprints } (key ids:
# SHA-256 of each pinned public key's 65-byte point, as Lucent logs them; ids and thumbprints in
# lowercase, sorted), or $null for a build that checks no channel.

function Get-UpdateKeyId {
    <#
    .SYNOPSIS
        The key id of an ECDSA P-256 public key (130 hex digits, "04..."): SHA-256 of its 65 bytes.
    #>
    param([Parameter(Mandatory)][string]$PublicKey)
    $hex = $PublicKey.Trim().ToLowerInvariant()
    if ($hex -notmatch '^04[0-9a-f]{128}$') { throw "'$PublicKey' is not an uncompressed P-256 public key (130 hex digits starting with 04)." }
    $point = New-Object byte[] 65
    for ($i = 0; $i -lt 65; $i++) { $point[$i] = [Convert]::ToByte($hex.Substring(2 * $i, 2), 16) }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try { return -join ($sha.ComputeHash($point) | ForEach-Object { $_.ToString('x2') }) } finally { $sha.Dispose() }
}

function New-UpdateChannelRecord {
    param([string]$Name, [string]$ManifestUrl, [string[]]$KeyIds, [string[]]$PublisherThumbprints)
    return [pscustomobject]@{
        name = $Name; manifestUrl = $ManifestUrl
        keyIds = @(@($KeyIds | Where-Object { $_ } | ForEach-Object { $_.Trim().ToLowerInvariant() }) | Sort-Object -Unique)
        publisherThumbprints = @(@($PublisherThumbprints | Where-Object { $_ } | ForEach-Object { $_.Trim().ToLowerInvariant() }) | Sort-Object -Unique) }
}

function Get-LucentUpdateChannelRecord {
    <#
    .SYNOPSIS
        The update channel a build tree compiled into Lucent.exe (Read-LucentPackageInfo's
        updateChannel, from the LUCENT_UPDATE_* cache variables), as a channel record or $null.
    #>
    param([Parameter(Mandatory)]$Info)
    $channel = $Info.updateChannel
    if (-not $channel.manifestUrl) { return $null }
    return New-UpdateChannelRecord -Name $channel.name -ManifestUrl $channel.manifestUrl `
        -KeyIds @(@($channel.publicKeys) | Where-Object { $_ } | ForEach-Object { Get-UpdateKeyId $_ }) `
        -PublisherThumbprints @($channel.publisherThumbprints)
}

function ConvertFrom-UpdateChannelLogLine {
    <#
    .SYNOPSIS
        The update channel Lucent.exe reported at start-up (its developer automation's line on stderr,
        app::describe_update_channel() in src\application): "update channel '<name>' (<URL>): keys
        <id>,<id>; publishers <thumbprint>,... | none", or "update channel: none". Found = $false
        when no such line was written, also for "update channel refused: <reason>" (a compiled
        configuration Lucent refuses).
    #>
    param([string[]]$Lines)
    foreach ($line in @($Lines)) {
        if ($line -match "update channel '([a-z0-9-]+)' \((\S+)\): keys ([0-9a-f,]*); publishers ([0-9a-f,]+|none)") {
            $keys = @($Matches[3].Split(',') | Where-Object { $_ })
            $publishers = @()
            if ($Matches[4] -ne 'none') { $publishers = @($Matches[4].Split(',') | Where-Object { $_ }) }
            return [pscustomobject]@{ Found = $true; Channel = (New-UpdateChannelRecord -Name $Matches[1] -ManifestUrl $Matches[2] -KeyIds $keys -PublisherThumbprints $publishers) }
        }
        if ($line -match 'update channel: none') { return [pscustomobject]@{ Found = $true; Channel = $null } }
    }
    return [pscustomobject]@{ Found = $false; Channel = $null }
}

function Format-UpdateChannel {
    param($Channel)
    if ($null -eq $Channel) { return 'none (this build never checks for updates)' }
    $publishers = if (@($Channel.publisherThumbprints).Count -gt 0) { @($Channel.publisherThumbprints) -join ', ' } else { 'none named (Authenticode not checked)' }
    return "'$($Channel.name)' $($Channel.manifestUrl); release-manifest keys $(@($Channel.keyIds) -join ', '); installer publishers $publishers"
}

function Compare-UpdateChannel {
    <#
    .SYNOPSIS
        The differences between two channel records (none when they are the same channel: name, URL,
        key ids and publisher thumbprints). Callers wrap it in @(): no difference is no output.
    #>
    param($Expected, $Actual)
    $differences = New-Object System.Collections.Generic.List[string]
    if ($null -eq $Expected -or $null -eq $Actual) {
        if (($null -eq $Expected) -ne ($null -eq $Actual)) { $differences.Add("expected $(Format-UpdateChannel $Expected), found $(Format-UpdateChannel $Actual)") }
        return $differences
    }
    if ($Expected.name -cne $Actual.name) { $differences.Add("name '$($Actual.name)' instead of '$($Expected.name)'") }
    if ($Expected.manifestUrl -cne $Actual.manifestUrl) { $differences.Add("manifest URL $($Actual.manifestUrl) instead of $($Expected.manifestUrl)") }
    if ((@($Expected.keyIds) -join ',') -cne (@($Actual.keyIds) -join ',')) { $differences.Add("release-manifest keys $(@($Actual.keyIds) -join ', ') instead of $(@($Expected.keyIds) -join ', ')") }
    if ((@($Expected.publisherThumbprints) -join ',') -cne (@($Actual.publisherThumbprints) -join ',')) {
        $differences.Add("installer publishers $(@($Actual.publisherThumbprints) -join ', ') instead of $(@($Expected.publisherThumbprints) -join ', ')")
    }
    return $differences
}

function Read-ExpectedUpdateChannels {
    <#
    .SYNOPSIS
        installer\update-channels.json ("lucent.update-channels" version 1): the channels a release
        build may check, each with its manifest URL, release-manifest key ids and the publisher
        certificates its installers are signed with (at least one).
    #>
    param([Parameter(Mandatory)][string]$Path)
    $document = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
    if ($document.schema -ne 'lucent.update-channels' -or $document.version -ne 1) { throw "$Path is not a lucent.update-channels version 1 document." }
    $channels = @()
    foreach ($entry in @($document.channels)) {
        if (-not $entry) { continue }
        if ($entry.name -cnotmatch '^[a-z0-9]+(-[a-z0-9]+)*$') { throw "$Path names an invalid channel '$($entry.name)'." }
        if ($entry.manifestUrl -notmatch '^https://') { throw "$Path gives channel '$($entry.name)' a manifest URL that is not https." }
        foreach ($id in @($entry.keyIds)) { if ($id -cnotmatch '^[0-9a-f]{64}$') { throw "$Path gives channel '$($entry.name)' an invalid key id '$id'." } }
        foreach ($thumbprint in @($entry.publisherThumbprints)) {
            if ($thumbprint -cnotmatch '^([0-9a-f]{40}|[0-9a-f]{64})$') { throw "$Path gives channel '$($entry.name)' an invalid thumbprint '$thumbprint'." }
        }
        $channels += New-UpdateChannelRecord -Name $entry.name -ManifestUrl $entry.manifestUrl -KeyIds @($entry.keyIds) -PublisherThumbprints @($entry.publisherThumbprints)
    }
    return $channels
}

function Test-LucentUpdateChannelPolicy {
    <#
    .SYNOPSIS
        Whether a package may be built with the update channel its Lucent.exe checks (W12 review
        SEC-02). -Declared is tools\package.ps1's -UpdateChannel: 'none', a channel name, or empty.
        A Setup (not -StageOnly) must declare it. 'none' needs a build without a channel; a name needs
        the build to check exactly that entry of the expected channels, which must name at least one
        publisher certificate (otherwise the installer's Authenticode signature would not be checked
        and whoever holds the manifest key controls every update). Returns Errors and Warnings.
    #>
    param($Compiled, [string]$Declared, [object[]]$Expected = @(), [switch]$StageOnly)
    $errors = New-Object System.Collections.Generic.List[string]
    $warnings = New-Object System.Collections.Generic.List[string]
    if (-not $Declared) {
        if (-not $StageOnly) {
            $errors.Add('Pass -UpdateChannel <name> (an entry of installer\update-channels.json) or -UpdateChannel none: a Setup declares which update channel its Lucent.exe checks.')
        }
        elseif ($null -ne $Compiled -and -not @($Expected | Where-Object { @(Compare-UpdateChannel -Expected $_ -Actual $Compiled).Count -eq 0 })) {
            $warnings.Add("This build checks the update channel $(Format-UpdateChannel $Compiled), which installer\update-channels.json does not list: not for release.")
        }
    }
    elseif ($Declared -eq 'none') {
        if ($null -ne $Compiled) {
            $errors.Add("-UpdateChannel none, but this build checks $(Format-UpdateChannel $Compiled). Reconfigure with empty LUCENT_UPDATE_* (tools\build.ps1 -Fresh without them set).")
        }
    }
    else {
        $entry = @($Expected | Where-Object { $_.name -ceq $Declared })
        if ($null -eq $Compiled) {
            $errors.Add("-UpdateChannel $Declared, but this build checks no channel: configure it with the LUCENT_UPDATE_* values of that channel (docs\development\setup.md).")
        }
        elseif ($entry.Count -ne 1) {
            $errors.Add("installer\update-channels.json lists no channel '$Declared'.")
        }
        else {
            if (@($entry[0].publisherThumbprints).Count -eq 0) {
                $errors.Add("The channel '$Declared' names no publisher certificate in installer\update-channels.json: a release channel must, so that clients check the installer's Authenticode signature.")
            }
            foreach ($difference in (Compare-UpdateChannel -Expected $entry[0] -Actual $Compiled)) {
                $errors.Add("This build's update channel differs from installer\update-channels.json's '$Declared': $difference.")
            }
        }
    }
    return [pscustomobject]@{ Errors = $errors; Warnings = $warnings }
}

# --- Static libraries linked into the executable -------------------------------------------------

function Get-LucentExecutablesText {
    <#
    .SYNOPSIS
        "Lucent.exe and lucent-worker.exe": the executables Lucent ships (the application and its
        helpers), for licence texts and messages.
    #>
    param([Parameter(Mandatory)]$Info)
    $names = @(@($Info.executable) + @($Info.helperExecutables) | Where-Object { $_ })
    if ($names.Count -le 1) { return [string]$names[0] }
    return (($names[0..($names.Count - 2)] -join ', ') + ' and ' + $names[-1])
}

function Get-StaticVcpkgLibraries {
    <#
    .SYNOPSIS
        vcpkg libraries linked statically into the shipped executables (Lucent.exe and its helpers):
        the vcpkg .lib files on their link lines (build.ninja) whose members are object files rather
        than DLL imports (lib /list). Each names the executables that link it (Executables: their file
        names, in the order Lucent.exe, then its helpers), so the notices say which program holds its
        code (W12 review LIC-5).
    #>
    param([Parameter(Mandatory)]$Info, [Parameter(Mandatory)][hashtable]$FileIndex)
    $binaryDir = ConvertTo-NativePath $Info.binaryDir
    $ninja = Join-Path $binaryDir 'build.ninja'
    if (-not (Test-Path -LiteralPath $ninja)) { throw "$ninja is missing: the statically linked libraries cannot be determined (the presets use Ninja)." }
    $outputs = @{}
    $programNames = [ordered]@{}
    foreach ($built in @(@($Info.builtExecutable) + @($Info.builtHelperExecutables) | Where-Object { $_ })) {
        $key = (Get-RelativePathText -Root $binaryDir -Path (ConvertTo-NativePath $built)).ToLowerInvariant()
        $outputs[$key] = $null
        $programNames[$key] = [System.IO.Path]::GetFileName((ConvertTo-NativePath $built))
    }
    $current = $null
    foreach ($line in [System.IO.File]::ReadLines($ninja)) {
        if ($line -match '^build (.+?): CXX_EXECUTABLE_LINKER__') {
            $target = ($Matches[1] -replace '\$ ', ' ' -replace '\$:', ':' -replace '\$\$', '$').ToLowerInvariant()
            $current = if ($outputs.ContainsKey($target)) { $target } else { $null }
            continue
        }
        if ($current) {
            if ($line -notmatch '^\s') { $current = $null; continue }
            if ($line -match '^\s+LINK_LIBRARIES = (.*)$') { $outputs[$current] = $Matches[1]; $current = $null }
        }
    }
    foreach ($output in @($outputs.Keys)) {
        if ($null -eq $outputs[$output]) { throw "No link statement for $output was found in $ninja." }
    }
    $tripletDir = [System.IO.Path]::GetFullPath((Join-Path (ConvertTo-NativePath $Info.vcpkgInstalledDir) $Info.vcpkgTriplet)).TrimEnd('\') + '\'
    $lib = Join-Path (Split-Path -Parent (ConvertTo-NativePath $Info.dumpbin)) 'lib.exe'
    if (-not (Test-Path -LiteralPath $lib)) { throw "lib.exe was not found next to dumpbin ($lib)." }
    $found = @{}
    $imports = @{}   # import libraries, listed once
    $escapedSpace = [string][char]0
    foreach ($output in @($programNames.Keys)) {
        foreach ($token in ($outputs[$output].Replace('$ ', $escapedSpace) -split ' ')) {
            $path = $token.Replace($escapedSpace, ' ') -replace '\$:', ':' -replace '\$\$', '$'
            if ($path -notmatch '\.lib$') { continue }
            if (-not [System.IO.Path]::IsPathRooted($path)) { $path = Join-Path $binaryDir $path }
            $full = [System.IO.Path]::GetFullPath($path)
            $key = $full.ToLowerInvariant()
            if (-not $full.StartsWith($tripletDir, [System.StringComparison]::OrdinalIgnoreCase) -or $imports.ContainsKey($key)) { continue }
            if ($found.ContainsKey($key)) {
                if ($found[$key].Executables -notcontains $programNames[$output]) { $found[$key].Executables += $programNames[$output] }
                continue
            }
            $members = @(& $lib /nologo /list $full | Where-Object { $_.Trim() } | Sort-Object -Unique)
            if ($LASTEXITCODE -ne 0) { throw "lib /list failed on $full" }
            $objects = @($members | Where-Object { $_ -notmatch '\.dll$' })
            if ($objects.Count -eq 0) { $imports[$key] = $true; continue }   # an import library
            $relative = $full.Substring($tripletDir.Length).Replace('\', '/')
            $owner = $FileIndex[$relative.ToLowerInvariant()]
            if (-not $owner) { throw "$full is linked into $($programNames[$output]) but belongs to no vcpkg port." }
            $found[$key] = [pscustomobject]@{ Path = $relative; Name = [System.IO.Path]::GetFileName($full); Port = $owner.Port; Members = $objects
                Executables = @($programNames[$output]) }
        }
    }
    return @(ConvertTo-OrdinalOrder -Items @($found.Values) -Property Path)
}

function Format-NameList {
    # "A", "A and B", "A, B and C".
    param([AllowEmptyCollection()][string[]]$Names)
    $names = @($Names | Where-Object { $_ })
    if ($names.Count -le 1) { return [string]($names -join '') }
    return (($names[0..($names.Count - 2)] -join ', ') + ' and ' + $names[-1])
}

function Get-StaticLibraryProgramsText {
    # The programs a statically linked library is inside: its Executables (Get-StaticVcpkgLibraries), or
    # every shipped executable when a record does not say.
    param([Parameter(Mandatory)]$Library, [Parameter(Mandatory)]$Info)
    if (($Library.PSObject.Properties.Name -contains 'Executables') -and @($Library.Executables | Where-Object { $_ }).Count -gt 0) {
        return (Format-NameList @($Library.Executables))
    }
    return (Get-LucentExecutablesText $Info)
}

# --- Qt's own SBOM (tag-value SPDX) --------------------------------------------------------------

function Read-TagValueSpdx {
    <#
    .SYNOPSIS
        Reads the parts of a tag-value SPDX document that packaging uses: packages, files,
        relationships and extracted licences. Multi-line <text> values are joined.
    #>
    param([Parameter(Mandatory)][string]$Path)
    $packages = New-Object System.Collections.Generic.List[object]
    $files = New-Object System.Collections.Generic.List[object]
    $relationships = New-Object System.Collections.Generic.List[object]
    $extracted = @{}
    $current = $null
    $lines = [System.IO.File]::ReadAllLines($Path)
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -notmatch '^([A-Za-z]+):\s?(.*)$') { continue }
        $tag = $Matches[1]
        $value = $Matches[2]
        if ($value.StartsWith('<text>')) {
            $text = $value.Substring(6)
            while ($text.IndexOf('</text>') -lt 0 -and $i + 1 -lt $lines.Count) { $i++; $text += "`n" + $lines[$i] }
            $value = $text.Substring(0, $text.IndexOf('</text>'))
        }
        switch ($tag) {
            'PackageName' { $current = @{ Kind = 'package'; PackageName = $value }; $packages.Add($current) }
            'FileName' { $current = @{ Kind = 'file'; FileName = $value }; $files.Add($current) }
            'LicenseID' { $current = @{ Kind = 'licence'; LicenseID = $value }; $extracted[$value] = $current }
            'Relationship' {
                $parts = $value -split '\s+'
                if ($parts.Count -ge 3) { $relationships.Add([pscustomobject]@{ From = $parts[0]; Type = $parts[1]; To = $parts[2] }) }
            }
            default { if ($current -and -not $current.ContainsKey($tag)) { $current[$tag] = $value } }
        }
    }
    return [pscustomobject]@{ Packages = $packages; Files = $files; Relationships = $relationships; Extracted = $extracted }
}

function Get-LicenseIds {
    # The licence and exception identifiers of an SPDX licence expression.
    param([string]$Expression)
    if (-not $Expression) { return @() }
    return @(($Expression -replace '[()]', ' ') -split '\s+' | Where-Object { $_ -and $_ -notin 'AND', 'OR', 'WITH' } | Sort-Object -Unique)
}

function Get-QtSbomFacts {
    <#
    .SYNOPSIS
        From Qt's own SBOM of a shipped Qt port (vcpkg_installed\<triplet>\sbom\<port>-<version>.spdx):
        the Qt targets whose files ship and the third-party code inside them (Qt's
        "<target>_Attribution_<id>" packages), with holders, licences and source locations, plus
        the named packages (for statically linked targets, which list no files). Returns $null when
        the port has no Qt SBOM.
    #>
    param([Parameter(Mandatory)][string]$TripletDir, [Parameter(Mandatory)][string]$Port, [Parameter(Mandatory)][string]$Version,
          [AllowEmptyCollection()][string[]]$ShippedSources, [AllowEmptyCollection()][object[]]$ExtraTargets = @())
    $path = Join-Path $TripletDir "sbom\$Port-$Version.spdx"
    if (-not (Test-Path -LiteralPath $path)) { return $null }
    $doc = Read-TagValueSpdx -Path $path
    $byId = @{}
    $byName = @{}
    foreach ($package in $doc.Packages) {
        if ($package.ContainsKey('SPDXID')) { $byId[$package.SPDXID] = $package }
        $byName[$package.PackageName] = $package
    }
    $fileIds = @{}
    foreach ($file in $doc.Files) { if ($file.ContainsKey('SPDXID')) { $fileIds[$file.SPDXID] = $file.FileName -replace '^\./', '' } }
    $shipped = @{}
    foreach ($source in $ShippedSources) { $shipped[$source.ToLowerInvariant()] = $true }

    $targets = @{}     # SPDXID -> @{ Package; Files }
    foreach ($relationship in $doc.Relationships) {
        if ($relationship.Type -ne 'CONTAINS' -or -not $fileIds.ContainsKey($relationship.To) -or -not $byId.ContainsKey($relationship.From)) { continue }
        $file = $fileIds[$relationship.To]
        if (-not $shipped.ContainsKey($file.ToLowerInvariant())) { continue }
        if (-not $targets.ContainsKey($relationship.From)) {
            $targets[$relationship.From] = @{ Package = $byId[$relationship.From]; Files = New-Object System.Collections.Generic.List[string] }
        }
        $targets[$relationship.From].Files.Add([System.IO.Path]::GetFileName($file))
    }
    foreach ($extra in $ExtraTargets) {
        if (-not $byName.ContainsKey($extra.Name)) { throw "$path has no package named $($extra.Name)." }
        $package = $byName[$extra.Name]
        $targets[$package.SPDXID] = @{ Package = $package; Files = New-Object System.Collections.Generic.List[string] }
        $targets[$package.SPDXID].Files.Add($extra.File)
    }
    $attributions = @{}
    foreach ($relationship in $doc.Relationships) {
        if ($relationship.Type -ne 'DEPENDS_ON' -or -not $targets.ContainsKey($relationship.From) -or -not $byId.ContainsKey($relationship.To)) { continue }
        $package = $byId[$relationship.To]
        if ($package.PackageName -notmatch '_Attribution_') { continue }
        if (-not $attributions.ContainsKey($relationship.To)) {
            $comment = if ($package.ContainsKey('PackageComment')) { $package.PackageComment } else { '' }
            $name = if ($comment -match '(?m)^\s*Name:\s*(.+?)\s*$') { $Matches[1] } else { $package.PackageName }
            $licence = if ($package.ContainsKey('PackageLicenseConcluded') -and $package.PackageLicenseConcluded -ne 'NOASSERTION') { $package.PackageLicenseConcluded } else { $package.PackageLicenseDeclared }
            $attributions[$relationship.To] = [pscustomobject]@{
                Id = $package.PackageName; Name = $name
                Version = $(if ($package.ContainsKey('PackageVersion') -and $package.PackageVersion -ne 'unknown') { $package.PackageVersion } else { $null })
                License = $licence
                Copyright = $(if ($package.ContainsKey('PackageCopyrightText')) { $package.PackageCopyrightText.Trim() } else { 'NOASSERTION' })
                DownloadLocation = $(if ($package.ContainsKey('PackageDownloadLocation')) { $package.PackageDownloadLocation } else { 'NOASSERTION' })
                Files = New-Object System.Collections.Generic.List[string] }
        }
        foreach ($file in $targets[$relationship.From].Files) { if (-not $attributions[$relationship.To].Files.Contains($file)) { $attributions[$relationship.To].Files.Add($file) } }
    }
    return [pscustomobject]@{
        Path = $path
        Targets = @(ConvertTo-OrdinalOrder -Items @($targets.Values | ForEach-Object { $_.Package.PackageName }))
        Packages = $byName
        Attributions = @(ConvertTo-OrdinalOrder -Items @($attributions.Values) -Property Name)
        Extracted = $doc.Extracted }
}

# --- Licences ------------------------------------------------------------------------------------

function Get-PortSpdx {
    param([Parameter(Mandatory)][string]$TripletDir, [Parameter(Mandatory)][string]$Port)
    $path = Join-Path $TripletDir "share\$Port\vcpkg.spdx.json"
    if (-not (Test-Path -LiteralPath $path)) { throw "vcpkg SPDX document missing for port $Port ($path)" }
    return Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
}

function Get-PolicyEntry {
    param([Parameter(Mandatory)]$Policy, [Parameter(Mandatory)][string]$Port)
    # Strict-mode safe also for a policy without ports.
    $property = $Policy.ports.PSObject.Properties[$Port]
    if ($property) { return $property.Value }
    return $null
}

function Get-CompiledInPorts {
    <#
    .SYNOPSIS
        Header-only ports among the direct (non-feature) dependencies in vcpkg.json: their code is
        compiled into Lucent.exe, so their notices ship although no DLL does.
    #>
    param([Parameter(Mandatory)][string]$RepoRoot, [Parameter(Mandatory)][hashtable]$FileIndex)
    $manifest = Get-Content -LiteralPath (Join-Path $RepoRoot 'vcpkg.json') -Raw | ConvertFrom-Json
    $names = foreach ($dependency in $manifest.dependencies) {
        if ($dependency -is [string]) { $dependency } else { $dependency.name }
    }
    $binaries = @{}
    foreach ($entry in $FileIndex.Values) {
        if ($entry.Path -match '\.(dll|lib)$') { $binaries[$entry.Port] = $true }
    }
    return @(ConvertTo-OrdinalOrder -Items @($names | Where-Object { -not $binaries.ContainsKey($_) }))
}

function Get-SourceArchive {
    <#
    .SYNOPSIS
        Finds a port's upstream source archive in vcpkg's downloads folder and checks its SHA-512
        against the port's SPDX document.
    #>
    param([Parameter(Mandatory)]$Resource, [Parameter(Mandatory)][string]$DownloadsDir)
    $names = New-Object System.Collections.Generic.List[string]
    if ($Resource.PSObject.Properties.Name -contains 'packageFileName' -and $Resource.packageFileName) { $names.Add($Resource.packageFileName) }
    if ($Resource.downloadLocation -match '@([^@/]+)$') {
        $ref = $Matches[1]
        foreach ($extension in '.tar.gz', '.tar.xz', '.tar.bz2', '.zip') { $names.Add(($Resource.name -replace '/', '-') + "-$ref$extension") }
    }
    $expected = ($Resource.checksums | Where-Object { $_.algorithm -eq 'SHA512' } | Select-Object -First 1).checksumValue
    foreach ($name in $names) {
        $path = Join-Path $DownloadsDir $name
        if (Test-Path -LiteralPath $path) {
            $actual = (Get-FileHash -LiteralPath $path -Algorithm SHA512).Hash.ToLowerInvariant()
            if ($expected -and $actual -ne $expected.ToLowerInvariant()) { throw "$path does not match the SHA-512 recorded by vcpkg." }
            return $path
        }
    }
    return $null
}

function Get-CopyrightSection {
    # Extracts one "<name>.txt:" section of a Qt copyright file (vcpkg concatenates LICENSES\*.txt).
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][string]$Section)
    $lines = [System.IO.File]::ReadAllLines($Path)
    $out = New-Object System.Collections.Generic.List[string]
    $inside = $false
    foreach ($line in $lines) {
        if ($line -match '^[A-Za-z0-9.+-]+\.txt:$') {
            if ($inside) { break }
            $inside = ($line -eq "$($Section):")
            continue
        }
        if ($inside) { $out.Add($line) }
    }
    $text = ($out -join "`n").Trim()
    if ($text.Length -lt 1000) { throw "Section $Section not found (or truncated) in $Path" }
    return $text + "`n"
}

function Set-TemplateTitle {
    # Replaces @TITLE@ (which contains @VERSION@) and underlines it to its length.
    param([Parameter(Mandatory)][string]$Template, [Parameter(Mandatory)][string]$Version)
    $lines = $Template -split "`r?`n"
    $title = $lines[0].Replace('@VERSION@', $Version)
    $rest = ($lines | Select-Object -Skip 2) -join "`n"
    return ($title + "`n" + ('=' * $title.Length) + "`n" + $rest).Replace('@VERSION@', $Version)
}

function Get-OverlayPortFolder {
    <#
    .SYNOPSIS
        The repository's vcpkg overlay port of that name (vcpkg-configuration.json "overlay-ports"),
        or $null. vcpkg builds such a port from the repository's folder, not from its registry, so its
        recipe is identified by that folder (W13: gettext-libintl, which Lucent ships as intl-8.dll).
    .OUTPUTS
        Path (full) and Relative (from the repository root, with forward slashes).
    #>
    param([string]$RepoRoot, [Parameter(Mandatory)][string]$Port)
    if (-not $RepoRoot) { return $null }
    $configurationPath = Join-Path $RepoRoot 'vcpkg-configuration.json'
    if (-not (Test-Path -LiteralPath $configurationPath -PathType Leaf)) { return $null }
    $configuration = Get-Content -LiteralPath $configurationPath -Raw | ConvertFrom-Json
    if (-not ($configuration.PSObject.Properties.Name -contains 'overlay-ports')) { return $null }
    $root = [System.IO.Path]::GetFullPath($RepoRoot).TrimEnd('\') + '\'
    foreach ($overlay in @($configuration.'overlay-ports')) {
        $base = if ([System.IO.Path]::IsPathRooted($overlay)) { $overlay } else { Join-Path $RepoRoot $overlay }
        $folder = [System.IO.Path]::GetFullPath((Join-Path $base $Port))
        if (Test-Path -LiteralPath (Join-Path $folder 'portfile.cmake') -PathType Leaf) {
            $relative = if ($folder.StartsWith($root, [System.StringComparison]::OrdinalIgnoreCase)) { $folder.Substring($root.Length).Replace('\', '/') } else { $folder }
            return [pscustomobject]@{ Path = $folder; Relative = $relative }
        }
    }
    return $null
}

function Get-RecipeReference {
    <#
    .SYNOPSIS
        Identifies a port's vcpkg recipe. vcpkg's SPDX downloadLocation ends in the git tree hash of
        the port folder, not a commit; when the registry baseline commit holds exactly that tree, the
        port's folder at that commit is a browsable, checkable link. A port the repository overlays
        (Get-OverlayPortFolder) is identified by its folder in the repository instead (Overlay).
    #>
    param([Parameter(Mandatory)]$Component, [string]$VcpkgRoot, [string]$Baseline, [string]$RepoRoot)
    $overlay = Get-OverlayPortFolder -RepoRoot $RepoRoot -Port $Component.Port
    if ($overlay) {
        return [pscustomobject]@{ Tree = $null; AtBaseline = $false; Url = $null; Overlay = $overlay.Relative; OverlayPath = $overlay.Path }
    }
    $tree = if ($Component.Recipe -match '@([0-9a-f]{40})$') { $Matches[1] } else { $null }
    $atBaseline = $false
    if ($tree -and $VcpkgRoot -and $Baseline -and (Test-Path -LiteralPath (Join-Path $VcpkgRoot '.git'))) {
        $probe = Invoke-NativeCommand -FilePath 'git' -Arguments @('-C', $VcpkgRoot, 'rev-parse', "$($Baseline):ports/$($Component.Port)")
        $atBaseline = ($probe.ExitCode -eq 0 -and $probe.Output.Count -gt 0 -and $probe.Output[0].Trim() -eq $tree)
    }
    return [pscustomobject]@{
        Tree = $tree; AtBaseline = $atBaseline
        Url = $(if ($atBaseline) { "https://github.com/microsoft/vcpkg/tree/$Baseline/ports/$($Component.Port)" } else { $null })
        Overlay = $null; OverlayPath = $null }
}

function Get-CopyrightSectionNames {
    # Names of the "<licence id>.txt:" sections of a Qt copyright file (vcpkg concatenates LICENSES\*.txt).
    param([Parameter(Mandatory)][string]$Path)
    return @([System.IO.File]::ReadAllLines($Path) | ForEach-Object { if ($_ -match '^([A-Za-z0-9.+-]+)\.txt:$') { $Matches[1] } })
}

function Get-AssetLicences {
    <#
    .SYNOPSIS
        The licence of every bundled data asset of the layout (W13, ADR 0023).
    .DESCRIPTION
        An asset that installer\license-policy.json lists under "assets" is third-party data under the
        terms it names: its licence text (a file the asset's verified manifest lists, licenseFile) is
        required, and its files get that licence in the SBOM except those named lucentFiles (Lucent's
        own description of the data, MIT) and the manifest. A provenance.json the asset holds (its
        upstream source, archive SHA-256 and commit) is read into the record. Every other asset is
        Lucent's own (MIT). Returns one record per asset.
    #>
    param(
        [Parameter(Mandatory)][string]$StageDir,
        [Parameter(Mandatory)]$Info,
        [Parameter(Mandatory)]$Policy)
    $records = New-Object System.Collections.Generic.List[object]
    $policyAssets = $null
    if ($Policy.PSObject.Properties.Name -contains 'assets') { $policyAssets = $Policy.assets }
    foreach ($asset in @($Info.assets)) {
        if (-not $asset) { continue }
        $folder = Join-Path $StageDir "assets\$($asset.id)"
        $manifestPath = Join-Path $folder 'manifest.json'
        if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { throw "assets\$($asset.id)\manifest.json is missing." }
        $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
        $entry = $null
        if ($policyAssets -and ($policyAssets.PSObject.Properties.Name -contains $asset.id)) { $entry = $policyAssets.($asset.id) }
        $record = [pscustomobject]@{
            Id = $asset.id; Version = $manifest.version; Folder = "assets\$($asset.id)"
            ThirdParty = [bool]$entry; Name = $asset.id; Concluded = 'MIT'; LicenseFile = $null; LucentFiles = @('manifest.json')
            Homepage = $null; Copyright = $null; Notices = @(); Comment = $null; Provenance = $null }
        if ($entry) {
            foreach ($field in 'name', 'concluded', 'licenseFile', 'copyright') {
                if (-not ($entry.PSObject.Properties.Name -contains $field) -or -not $entry.$field) {
                    throw "installer\license-policy.json: assets.$($asset.id) has no $field."
                }
            }
            $listed = @($manifest.files | ForEach-Object { $_.path })
            if ($listed -notcontains $entry.licenseFile) {
                throw "installer\license-policy.json names $($entry.licenseFile) as the licence text of asset $($asset.id), but its manifest does not list that file."
            }
            $record.Name = $entry.name
            $record.Concluded = $entry.concluded
            $record.LicenseFile = $entry.licenseFile
            if ($entry.PSObject.Properties.Name -contains 'lucentFiles') { $record.LucentFiles = @('manifest.json') + @($entry.lucentFiles) }
            if ($entry.PSObject.Properties.Name -contains 'homepage') { $record.Homepage = $entry.homepage }
            $record.Copyright = $entry.copyright
            if ($entry.PSObject.Properties.Name -contains 'notices') { $record.Notices = @($entry.notices) }
            if ($entry.PSObject.Properties.Name -contains 'comment') { $record.Comment = $entry.comment }
            $provenancePath = Join-Path $folder 'provenance.json'
            if (Test-Path -LiteralPath $provenancePath -PathType Leaf) {
                $record.Provenance = Get-Content -LiteralPath $provenancePath -Raw | ConvertFrom-Json
            }
        }
        $records.Add($record)
    }
    return $records.ToArray()
}

function New-LicenseBundle {
    <#
    .SYNOPSIS
        Writes <stage>\licenses and returns the component list (used for the SBOM).
    .DESCRIPTION
        For each shipped port and each header-only direct dependency: its copyright file, licence
        texts named by installer\license-policy.json, credit lines, the libraries the executable links
        statically (each must be reviewed in the policy), and for Qt ports the third-party code inside
        the shipped Qt files as recorded by Qt's own SBOM. For each third-party data asset
        (Get-AssetLicences): licenses\<asset id>\ with its licence text, its credit lines and its
        provenance. Then THIRD-PARTY-NOTICES.txt, which begins with Lucent's own terms: the MIT
        License, except Lucent's logo, whose terms it states once, before the third-party list, as
        the branding asset records them (-LogoLicence: Get-BrandingLicence of
        tools\branding\LucentBranding.psm1; ADR 0025, amendment of 2026-10-15). LGPL-NOTICE.txt
        follows from Write-LgplNotice, after the corresponding-source package it names
        (New-CorrespondingSourcePackage).
    #>
    param(
        [Parameter(Mandatory)][string]$StageDir,
        [Parameter(Mandatory)]$Info,
        [Parameter(Mandatory)]$Report,
        [Parameter(Mandatory)]$Policy,
        [Parameter(Mandatory)][string]$RepoRoot,
        [Parameter(Mandatory)][string]$DownloadsDir,
        [Parameter(Mandatory)][string]$NoticesDir,
        [Parameter(Mandatory)]$LogoLicence,
        [AllowEmptyCollection()][object[]]$StaticLibraries = @(),
        [AllowEmptyCollection()][object[]]$AssetLicences = @(),
        [string]$VcpkgRoot,
        [string]$Baseline,
        [string]$GitCommit)

    $licenses = Join-Path $StageDir 'licenses'
    if (Test-Path -LiteralPath $licenses) { Remove-Item -LiteralPath $licenses -Recurse -Force }
    New-Item -ItemType Directory -Path $licenses | Out-Null
    $tripletDir = Join-Path (ConvertTo-NativePath $Info.vcpkgInstalledDir) $Info.vcpkgTriplet
    $index = Get-VcpkgFileIndex -InstalledDir (ConvertTo-NativePath $Info.vcpkgInstalledDir) -Triplet $Info.vcpkgTriplet

    Copy-Item -LiteralPath (Join-Path $RepoRoot 'LICENSE') -Destination (Join-Path $licenses 'Lucent-LICENSE.txt')

    $components = New-Object System.Collections.Generic.List[object]
    $dynamicPorts = @($Report.Ports)
    $compiledIn = @(Get-CompiledInPorts -RepoRoot $RepoRoot -FileIndex $index)
    $staticPorts = @($StaticLibraries | ForEach-Object Port)
    foreach ($port in @(ConvertTo-OrdinalOrder -Items (@($dynamicPorts) + @($compiledIn) + @($staticPorts)) -Unique)) {
        $spdx = Get-PortSpdx -TripletDir $tripletDir -Port $port
        $portPackage = $spdx.packages | Where-Object { $_.SPDXID -eq 'SPDXRef-port' } | Select-Object -First 1
        $vcpkgLicense = $portPackage.licenseConcluded
        if (-not $vcpkgLicense -or $vcpkgLicense -like 'LicenseRef-vcpkg-null' -or $vcpkgLicense -eq 'NOASSERTION') { $vcpkgLicense = 'NOASSERTION' }
        $policyEntry = Get-PolicyEntry -Policy $Policy -Port $port
        $version = Get-PortBaseVersion $portPackage.versionInfo
        if ($policyEntry -and $policyEntry.reviewedVersion -ne $version) {
            throw "installer\license-policy.json reviewed $port $($policyEntry.reviewedVersion), but the build ships $version. Review the port's licence and update the policy and docs\legal\third-party-notices.md."
        }
        $concluded = $vcpkgLicense
        if ($policyEntry -and ($policyEntry.PSObject.Properties.Name -contains 'concluded')) { $concluded = $policyEntry.concluded }
        if ($concluded -eq 'NOASSERTION') {
            throw "No licence is known for shipped port $($port): vcpkg declares none. Add a reviewed entry to installer\license-policy.json."
        }

        $portDir = Join-Path $licenses $port
        New-Item -ItemType Directory -Path $portDir | Out-Null
        $copyright = Join-Path $tripletDir "share\$port\copyright"
        if (-not (Test-Path -LiteralPath $copyright)) { throw "Port $port installed no copyright file ($copyright)." }
        $copyrightText = [System.IO.File]::ReadAllText($copyright)
        Write-TextFile -Path (Join-Path $portDir 'copyright.txt') -Text $copyrightText
        $texts = New-Object System.Collections.Generic.List[string]
        $texts.Add("$port\copyright.txt")

        $sources = @($spdx.packages | Where-Object { $_.SPDXID -like 'SPDXRef-resource-*' } | ForEach-Object {
            [pscustomobject]@{ Name = $_.name; Location = $_.downloadLocation
                               Sha512 = ($_.checksums | Where-Object { $_.algorithm -eq 'SHA512' } | Select-Object -First 1).checksumValue
                               Resource = $_ } })
        $notices = @()
        $lgpl = $false
        if ($policyEntry) {
            if ($policyEntry.PSObject.Properties.Name -contains 'notices') { $notices = @($policyEntry.notices) }
            if ($policyEntry.PSObject.Properties.Name -contains 'lgpl') { $lgpl = [bool]$policyEntry.lgpl }
            if ($policyEntry.PSObject.Properties.Name -contains 'extractSections') {
                foreach ($section in $policyEntry.extractSections) {
                    Write-TextFile -Path (Join-Path $portDir $section.as) -Text (Get-CopyrightSection -Path $copyright -Section $section.section)
                    $texts.Add("$port\$($section.as)")
                }
            }
            if ($policyEntry.PSObject.Properties.Name -contains 'supplementaryFiles') {
                foreach ($supplementary in $policyEntry.supplementaryFiles) {
                    $extracted = $false
                    foreach ($source in $sources) {
                        $archive = Get-SourceArchive -Resource $source.Resource -DownloadsDir $DownloadsDir
                        if (-not $archive) { continue }
                        $tar = Join-Path $env:SystemRoot 'System32\tar.exe'
                        $members = & $tar -tf $archive
                        if ($LASTEXITCODE -ne 0) { throw "tar could not list $archive" }
                        $member = $members | Where-Object { $_ -match ('^[^/]+/' + [regex]::Escape($supplementary.member) + '$') } | Select-Object -First 1
                        if (-not $member) { continue }
                        $temp = Join-Path ([System.IO.Path]::GetTempPath()) ('lucent-licence-' + [guid]::NewGuid().ToString('N'))
                        New-Item -ItemType Directory -Path $temp | Out-Null
                        try {
                            & $tar -xf $archive -C $temp $member
                            if ($LASTEXITCODE -ne 0) { throw "tar could not extract $member from $archive" }
                            $text = [System.IO.File]::ReadAllText((Join-Path $temp (ConvertTo-NativePath $member)))
                            Write-TextFile -Path (Join-Path $portDir $supplementary.member) -Text $text
                        }
                        finally {
                            Remove-Item -LiteralPath $temp -Recurse -Force -ErrorAction SilentlyContinue
                        }
                        $texts.Add("$port\$($supplementary.member)")
                        $extracted = $true
                        break
                    }
                    if (-not $extracted) {
                        throw "Could not take $($supplementary.member) of port $port from its source archive: the archive is not in $DownloadsDir (vcpkg keeps sources only for ports it built on this machine). Run 'vcpkg install --only-downloads' for the port, or build it once from source."
                    }
                }
            }
        }

        # Libraries of this port linked statically into the executable: each needs a reviewed entry.
        $reviewedStatic = $null
        if ($policyEntry -and ($policyEntry.PSObject.Properties.Name -contains 'staticLibraries')) { $reviewedStatic = $policyEntry.staticLibraries }
        $statics = New-Object System.Collections.Generic.List[object]
        foreach ($library in @($StaticLibraries | Where-Object { $_.Port -eq $port })) {
            $review = $null
            if ($reviewedStatic -and ($reviewedStatic.PSObject.Properties.Name -contains $library.Path)) { $review = $reviewedStatic.($library.Path) }
            if (-not $review) {
                throw "$(Get-StaticLibraryProgramsText $library $Info) link$(if (@($library.Executables).Count -le 1) { 's' }) $($library.Path) of port $port statically ($($library.Members -join ', ')); installer\license-policy.json has no reviewed entry for it (ports.$port.staticLibraries)."
            }
            if ($review.concluded -match 'GPL') {
                throw "$($library.Path) is used under $($review.concluded) but is linked statically into $(Get-StaticLibraryProgramsText $library $Info); LGPL and GPL code must stay in replaceable DLLs (ADR 0003)."
            }
            $statics.Add([pscustomobject]@{
                Path = $library.Path; File = $library.Name; Name = $review.name; LicenseDeclared = $review.declared; LicenseConcluded = $review.concluded
                Copyright = $review.copyright; Comment = $review.comment; Executables = @($library.Executables)
                QtSbomPackage = $(if ($review.PSObject.Properties.Name -contains 'qtSbomPackage') { $review.qtSbomPackage } else { $null }) })
        }

        # Qt's own SBOM: the third-party code compiled into the shipped Qt files.
        $files = @(ConvertTo-OrdinalOrder -Items @($Report.Files | Where-Object { $_.Port -eq $port } | ForEach-Object Path))
        $shippedSources = @($Report.Files | Where-Object { $_.Port -eq $port } | ForEach-Object {
            (Get-RelativePathText -Root $tripletDir -Path $_.Source).Replace('\', '/') })
        $extraTargets = @($statics | Where-Object { $_.QtSbomPackage } | ForEach-Object { [pscustomobject]@{ Name = $_.QtSbomPackage; File = $_.File } })
        $qtSbom = Get-QtSbomFacts -TripletDir $tripletDir -Port $port -Version $version -ShippedSources $shippedSources -ExtraTargets $extraTargets
        foreach ($static in $statics) {
            if (-not $static.QtSbomPackage) { continue }
            if (-not $qtSbom) { throw "$($static.Path): the policy names Qt SBOM package $($static.QtSbomPackage), but port $port has no Qt SBOM." }
            $declared = $qtSbom.Packages[$static.QtSbomPackage].PackageLicenseDeclared
            if ($declared -ne $static.LicenseDeclared) {
                throw "Qt's SBOM declares '$declared' for $($static.QtSbomPackage) ($($static.Path)); installer\license-policy.json reviewed '$($static.LicenseDeclared)'."
            }
        }

        $linkage = if ($files.Count -gt 0) { 'dynamic' } elseif ($statics.Count -gt 0) { 'static' } else { 'compiled-in' }
        $components.Add([pscustomobject]@{
            Port = $port; Version = $version; PortVersion = $portPackage.versionInfo
            Homepage = $(if ($portPackage.PSObject.Properties.Name -contains 'homepage') { $portPackage.homepage } else { $null })
            Recipe = $portPackage.downloadLocation
            LicenseDeclared = $vcpkgLicense; LicenseConcluded = $concluded
            Comment = $(if ($policyEntry -and ($policyEntry.PSObject.Properties.Name -contains 'comment')) { $policyEntry.comment } else { $null })
            Linkage = $linkage; Files = $files; LicenceTexts = @($texts); Notices = $notices; Lgpl = $lgpl
            StaticLibraries = $statics.ToArray(); QtSbom = $qtSbom; RecipeRef = $null
            Sources = @($sources | ForEach-Object { [pscustomobject]@{ Name = $_.Name; Location = $_.Location; Sha512 = $_.Sha512; Resource = $_.Resource } })
            Spdx = $spdx })
        $components[$components.Count - 1].RecipeRef = Get-RecipeReference -Component $components[$components.Count - 1] -VcpkgRoot $VcpkgRoot -Baseline $Baseline -RepoRoot $RepoRoot
        # A port Lucent overlays (W13: gettext-libintl, glib): its recipe exists only in Lucent's own
        # source, with Lucent's patches. It is part of the "scripts used to control compilation" of an
        # LGPL library, so the recipe itself ships in the licence bundle (licenses\<port>\recipe\), and
        # the notice and the SBOM point at that copy, which needs no published source repository.
        $recipeRef = $components[$components.Count - 1].RecipeRef
        if ($recipeRef -and $recipeRef.OverlayPath) {
            $recipeDir = Join-Path $portDir 'recipe'
            Copy-Item -LiteralPath $recipeRef.OverlayPath -Destination $recipeDir -Recurse
            $recipeRef | Add-Member -NotePropertyName ShippedRecipe -NotePropertyValue "licenses\$port\recipe"
        }
    }

    # Every licence named for code inside the Qt files (or linked statically) must have its text in
    # one of the shipped Qt copyright files.
    $qtSections = @{}
    foreach ($component in @($components | Where-Object { $_.QtSbom })) {
        foreach ($name in (Get-CopyrightSectionNames -Path (Join-Path $licenses "$($component.Port)\copyright.txt"))) {
            if (-not $qtSections.ContainsKey($name)) { $qtSections[$name] = "$($component.Port)\copyright.txt" }
        }
    }
    foreach ($component in @($components | Where-Object { $_.QtSbom })) {
        $named = @($component.QtSbom.Attributions | ForEach-Object { [pscustomobject]@{ What = $_.Name; License = $_.License } }) +
                 @($component.StaticLibraries | ForEach-Object { [pscustomobject]@{ What = $_.Name; License = $_.LicenseConcluded } })
        foreach ($item in $named) {
            foreach ($id in (Get-LicenseIds $item.License)) {
                if (-not $qtSections.ContainsKey($id)) {
                    throw "The licence $id of '$($item.What)' (inside port $($component.Port)) has no text in the shipped Qt copyright files."
                }
            }
        }
    }

    # LGPL-NOTICE.txt is written by Write-LgplNotice once the corresponding-source package exists
    # (W12, ADR 0024): the notice names that archive with its size and SHA-256.

    # Third-party notices.
    $noticeLines = New-Object System.Collections.Generic.List[string]
    $listLines = New-Object System.Collections.Generic.List[string]
    foreach ($component in $components) {
        foreach ($notice in $component.Notices) { $noticeLines.Add("- $notice") }
        $kind = if ($component.Linkage -eq 'dynamic') { 'DLLs' } else { 'Compiled in' }
        $listLines.Add("$($component.Port) $($component.Version)")
        $listLines.Add("  Licence: $($component.LicenseConcluded)")
        if ($component.LicenseDeclared -ne $component.LicenseConcluded -and $component.LicenseDeclared -ne 'NOASSERTION') { $listLines.Add("  Offered under: $($component.LicenseDeclared)") }
        if ($component.Comment) { $listLines.Add("  Note: $($component.Comment)") }
        if ($component.Homepage) { $listLines.Add("  Home page: $($component.Homepage)") }
        if ($component.Linkage -eq 'dynamic') {
            $dlls = @($component.Files | Where-Object { $_ -match '\.dll$' })
            foreach ($line in (Format-WrappedList -Prefix "  $($kind): " -Items $dlls)) { $listLines.Add($line) }
            $other = $component.Files.Count - $dlls.Count
            if ($other -gt 0) { $listLines.Add("  Other files: $other (QML module definitions and type information)") }
        }
        elseif ($component.Linkage -eq 'compiled-in') {
            $listLines.Add("  $($kind): part of $(Get-LucentExecutablesText $Info)")
        }
        foreach ($static in $component.StaticLibraries) {
            $listLines.Add("  Linked statically into $(Get-StaticLibraryProgramsText $static $Info): $($static.Name) ($($static.File)), $($static.LicenseConcluded)")
            foreach ($line in ($static.Copyright -split "`r?`n")) { if ($line.Trim()) { $listLines.Add("    $($line.Trim())") } }
            if ($static.Comment) { $listLines.Add("    $($static.Comment)") }
        }
        $listLines.Add('  Licence text: ' + ($component.LicenceTexts -join ', '))
        if ($component.QtSbom -and $component.QtSbom.Attributions.Count -gt 0) {
            $listLines.Add("  Third-party code inside these Qt files, as recorded by Qt's own SBOM ($([System.IO.Path]::GetFileName($component.QtSbom.Path))).")
            $listLines.Add('  Each licence text is the section of that name in the Qt copyright.txt files of this folder:')
            foreach ($attribution in $component.QtSbom.Attributions) {
                $what = $attribution.Name + $(if ($attribution.Version) { " $($attribution.Version)" } else { '' })
                $listLines.Add("    $what, in $(@($attribution.Files) -join ', '): $($attribution.License)")
                foreach ($line in ($attribution.Copyright -split "`r?`n")) { if ($line.Trim() -and $line.Trim() -ne 'NOASSERTION') { $listLines.Add("      $($line.Trim())") } }
                if ($attribution.DownloadLocation -and $attribution.DownloadLocation -notin 'NOASSERTION', 'NONE') { $listLines.Add("      Source: $($attribution.DownloadLocation)") }
            }
        }
        $listLines.Add('')
    }
    # Third-party data assets (W13, ADR 0023): the licence text the verified asset carries, credit
    # lines and where the data came from.
    $dataLines = New-Object System.Collections.Generic.List[string]
    foreach ($asset in @($AssetLicences | Where-Object { $_.ThirdParty })) {
        $assetDir = Join-Path $licenses $asset.Id
        New-Item -ItemType Directory -Path $assetDir -Force | Out-Null
        $text = [System.IO.File]::ReadAllText((Join-Path $StageDir "$($asset.Folder)\$($asset.LicenseFile)"))
        Write-TextFile -Path (Join-Path $assetDir $asset.LicenseFile) -Text $text
        foreach ($notice in $asset.Notices) { $noticeLines.Add("- $notice") }
        $dataLines.Add("$($asset.Name), asset $($asset.Id) $($asset.Version), in $($asset.Folder)")
        $dataLines.Add("  Licence: $($asset.Concluded)")
        $dataLines.Add("  $($asset.Copyright)")
        if ($asset.Homepage) { $dataLines.Add("  Home page: $($asset.Homepage)") }
        if ($asset.Provenance) {
            $source = $asset.Provenance.source
            $dataLines.Add("  Source: $($source.archive_url) (SHA-256 $($source.archive_sha256)), retrieved $($source.retrieved),")
            $dataLines.Add("          generated from $($source.repository) commit $($source.commit) ($($source.commit_date))")
            $dataLines.Add("  Modifications: $($asset.Provenance.modifications)")
        }
        if ($asset.Comment) { $dataLines.Add("  Note: $($asset.Comment)") }
        $dataLines.Add("  Licence text: $($asset.Id)\$($asset.LicenseFile) (also $($asset.Folder)\$($asset.LicenseFile))")
        $dataLines.Add('')
    }
    if ($dataLines.Count -eq 0) { $dataLines.Add('(none)') }

    if ($noticeLines.Count -eq 0) { $noticeLines.Add('(none)') }

    # Lucent's own notice, before the third-party list (ADR 0025, amendment of 2026-10-15): its logo is
    # not covered by the MIT License, and its terms, as the branding asset's provenance.json records them,
    # are stated here once (the SBOM names them by their identifier).
    $logoHolders = Format-NameList (@(@($Info.executable) + @($Info.helperExecutables) | Where-Object { $_ }) + @('Lucent Setup', 'its uninstaller (unins000.exe)'))
    $logoLines = New-Object System.Collections.Generic.List[string]
    $logoLines.Add((Format-Paragraph ("Lucent's logo is the icon of $logoHolders, and Lucent's window, Help > About and Setup's wizard show it. " +
        "Its terms, as its owner decided them on $($LogoLicence.Decided) (recorded in Lucent's source, assets/branding/provenance.json, branding " +
        "$($LogoLicence.Version); $($LogoLicence.Id) in Lucent.spdx.json):")))
    $logoLines.Add('')
    foreach ($line in ((Format-Paragraph -Text $LogoLicence.Text -Width 96) -split "`n")) { $logoLines.Add("    $line") }

    $template = [System.IO.File]::ReadAllText((Join-Path $NoticesDir 'THIRD-PARTY-NOTICES.txt.in'))
    if (-not $template.Contains('@LUCENT_LOGO@')) { throw "THIRD-PARTY-NOTICES.txt.in has no @LUCENT_LOGO@: the notices would not state the terms of Lucent's logo." }
    $noticesText = (Set-TemplateTitle -Template $template -Version $Info.version).Replace('@LUCENT_LOGO@', ($logoLines -join "`n")).Replace('@NOTICES@', ($noticeLines -join "`n")).Replace('@COMPONENTS@', ($listLines -join "`n").TrimEnd()).Replace('@DATA@', ($dataLines -join "`n").TrimEnd())
    Write-TextFile -Path (Join-Path $licenses 'THIRD-PARTY-NOTICES.txt') -Text $noticesText

    return $components
}

# --- SBOM ----------------------------------------------------------------------------------------

function ConvertTo-SpdxIdPart {
    param([Parameter(Mandatory)][string]$Text)
    return ($Text -replace '[^A-Za-z0-9.-]', '-')
}

function Copy-SpdxObject {
    # Shallow copy of a PSCustomObject into an ordered dictionary (so properties can be replaced).
    param([Parameter(Mandatory)]$Object)
    $copy = [ordered]@{}
    foreach ($property in $Object.PSObject.Properties) { $copy[$property.Name] = $property.Value }
    return $copy
}

function Get-SpdxVerificationCode {
    # SPDX 2.3 clause 7.9: SHA-1 of the ascending, concatenated SHA-1 values of the package's files.
    param([Parameter(Mandatory)][string[]]$Sha1Values)
    $sorted = [string[]]@($Sha1Values | ForEach-Object { $_.ToLowerInvariant() })
    [Array]::Sort($sorted, [System.StringComparer]::Ordinal)
    $sha1 = [System.Security.Cryptography.SHA1]::Create()
    try { $bytes = $sha1.ComputeHash([System.Text.Encoding]::ASCII.GetBytes(($sorted -join ''))) } finally { $sha1.Dispose() }
    return (($bytes | ForEach-Object { $_.ToString('x2') }) -join '')
}

function Test-SbomDocument {
    <#
    .SYNOPSIS
        Structural checks of the generated SPDX 2.3 document (the rules spdx-tools enforces that the
        aggregation could break). Returns error texts.
    #>
    param([Parameter(Mandatory)]$Document)
    $errors = New-Object System.Collections.Generic.List[string]
    $ids = @{ 'SPDXRef-DOCUMENT' = 'document' }
    $packageFilesAnalyzed = @{}
    foreach ($package in $Document.packages) {
        if ($ids.ContainsKey($package.SPDXID)) { $errors.Add("Duplicate SPDXID $($package.SPDXID)") }
        $ids[$package.SPDXID] = 'package'
        $packageFilesAnalyzed[$package.SPDXID] = (-not $package.Contains('filesAnalyzed')) -or [bool]$package.filesAnalyzed
        if ($package.Contains('filesAnalyzed') -and -not $package.filesAnalyzed -and $package.Contains('packageVerificationCode')) {
            $errors.Add("$($package.SPDXID) has a verification code but filesAnalyzed = false")
        }
    }
    foreach ($file in $Document.files) {
        if ($ids.ContainsKey($file.SPDXID)) { $errors.Add("Duplicate SPDXID $($file.SPDXID)") }
        $ids[$file.SPDXID] = 'file'
        if (-not (@($file.checksums) | Where-Object { $_.algorithm -eq 'SHA1' })) { $errors.Add("File $($file.SPDXID) ($($file.fileName)) has no SHA1 checksum") }
    }
    foreach ($relationship in $Document.relationships) {
        foreach ($id in @($relationship.spdxElementId, $relationship.relatedSpdxElement)) {
            if (-not $ids.ContainsKey($id) -and $id -notin 'NONE', 'NOASSERTION') { $errors.Add("Relationship names unknown element $id") }
        }
        if ($relationship.relationshipType -eq 'CONTAINS' -and $packageFilesAnalyzed.ContainsKey($relationship.spdxElementId) -and
            -not $packageFilesAnalyzed[$relationship.spdxElementId]) {
            $errors.Add("$($relationship.spdxElementId) has filesAnalyzed = false but CONTAINS $($relationship.relatedSpdxElement)")
        }
    }
    $declared = @{}
    # (Lists are enumerated with foreach: @() around a List[object] fails in Windows PowerShell 5.1.)
    foreach ($licence in $Document.hasExtractedLicensingInfos) { $declared[$licence.licenseId] = $true }
    $elements = New-Object System.Collections.Generic.List[object]
    foreach ($package in $Document.packages) { $elements.Add($package) }
    foreach ($file in $Document.files) { $elements.Add($file) }
    foreach ($element in $elements) {
        foreach ($field in 'licenseConcluded', 'licenseDeclared') {
            if (-not $element.Contains($field)) { continue }
            foreach ($id in (Get-LicenseIds ([string]$element[$field]))) {
                if ($id -like 'LicenseRef-*' -and -not $declared.ContainsKey($id)) { $errors.Add("$($element.SPDXID) uses $id, which the document does not define") }
            }
        }
    }
    return $errors
}

function New-PackageSbom {
    <#
    .SYNOPSIS
        One SPDX 2.3 document for the installed layout: Lucent and its own files (with the package
        verification code), every shipped port (vcpkg's port, binary and source packages, the
        recipe files, and the binary files that ship), the third-party code Qt records inside the
        shipped Qt files, libraries linked statically into the executable, each third-party data
        asset (its files carry the asset's licence; Get-AssetLicences), and the Visual C++
        redistributable bundled in Setup. Lucent's programs contain its logo, which is not covered by
        the MIT License (ADR 0025, amendment of 2026-10-15): the programs, and Lucent's package, are
        "MIT AND <the logo's licence reference>", whose text hasExtractedLicensingInfos gives as the
        branding asset records it (-LogoLicence: Get-BrandingLicence).
    #>
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)]$Info,
        [Parameter(Mandatory)]$Components,
        [Parameter(Mandatory)]$Report,
        [Parameter(Mandatory)][string]$StageDir,
        [Parameter(Mandatory)][string]$GitCommit,
        [Parameter(Mandatory)]$VcRedist,
        [Parameter(Mandatory)]$Policy,
        [Parameter(Mandatory)]$LogoLicence,
        [AllowEmptyCollection()][object[]]$AssetLicences = @(),
        [string]$VcpkgRoot)

    $packages = New-Object System.Collections.Generic.List[object]
    $files = New-Object System.Collections.Generic.List[object]
    $relationships = New-Object System.Collections.Generic.List[object]
    $ids = @{}
    function Add-Relationship([string]$From, [string]$Type, [string]$To) {
        $relationships.Add([ordered]@{ spdxElementId = $From; relationshipType = $Type; relatedSpdxElement = $To })
    }

    $created = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    $sbomRelative = Get-RelativePathText -Root $StageDir -Path $Path
    $sbomFileName = './' + $sbomRelative.Replace('\', '/')

    # Lucent's own files: everything in the folder that is not a vcpkg port's file, except this
    # document (excluded from the verification code, as SPDX allows).
    $vcpkgPaths = @{}
    foreach ($file in ($Report.Files | Where-Object { $_.Origin -eq 'vcpkg' })) { $vcpkgPaths[$file.Path.ToLowerInvariant()] = $true }
    $ownFiles = @(Get-ChildItem -LiteralPath $StageDir -Recurse -File | ForEach-Object {
            [pscustomobject]@{ Full = $_.FullName; Relative = (Get-RelativePathText -Root $StageDir -Path $_.FullName) } } |
        Where-Object { -not $vcpkgPaths.ContainsKey($_.Relative.ToLowerInvariant()) -and $_.Relative -ine $sbomRelative })
    # Ordinal: the file elements' order and identifiers do not depend on the regional format (LIC-7).
    $ownFiles = @(ConvertTo-OrdinalOrder -Items $ownFiles -Property Relative)
    $lucentId = 'SPDXRef-Lucent'
    # Lucent's logo is inside its programs (their icon; Lucent.exe's window icon and About logo) and is
    # not covered by the MIT License (ADR 0025): the programs are MIT and the logo's terms.
    $programLicence = "MIT AND $($LogoLicence.Id)"
    $ownEntries = New-Object System.Collections.Generic.List[object]
    $assetFileIds = @{}
    $counter = 0
    foreach ($file in $ownFiles) {
        $counter++
        $sha1 = Get-Sha1 $file.Full
        $program = $file.Relative -ieq $Info.executable -or @($Info.helperExecutables) -contains $file.Relative
        $licence = if ($program) { $programLicence } elseif ($file.Relative -like 'assets\*') { 'MIT' } else { 'NOASSERTION' }
        $copyright = 'NOASSERTION'
        # A third-party data asset's files carry its licence (W13, ADR 0023), except Lucent's own files in it.
        foreach ($asset in @($AssetLicences | Where-Object { $_.ThirdParty })) {
            $prefix = $asset.Folder + '\'
            if ($file.Relative.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
                $inside = $file.Relative.Substring($prefix.Length).Replace('\', '/')
                if (@($asset.LucentFiles) -notcontains $inside) {
                    $licence = $asset.Concluded
                    $copyright = $asset.Copyright
                    if (-not $assetFileIds.ContainsKey($asset.Id)) { $assetFileIds[$asset.Id] = New-Object System.Collections.Generic.List[string] }
                    $assetFileIds[$asset.Id].Add("SPDXRef-Lucent-file-$counter")
                }
            }
        }
        $entry = [ordered]@{
            fileName = './' + $file.Relative.Replace('\', '/'); SPDXID = "SPDXRef-Lucent-file-$counter"
            checksums = @([ordered]@{ algorithm = 'SHA1'; checksumValue = $sha1 },
                          [ordered]@{ algorithm = 'SHA256'; checksumValue = (Get-Sha256 $file.Full) })
            licenseConcluded = $licence
            copyrightText = $copyright }
        if ($program) { $entry.comment = "Contains Lucent's logo (branding $($LogoLicence.Version)), which is under $($LogoLicence.Id), not the MIT License." }
        $ownEntries.Add($entry)
    }
    $packages.Add([ordered]@{
        name = 'Lucent'; SPDXID = $lucentId; versionInfo = $Info.version
        supplier = 'Organization: Lucent project'
        downloadLocation = "git+$($Info.homepage).git@$GitCommit"
        homepage = $Info.homepage
        filesAnalyzed = $true
        packageVerificationCode = [ordered]@{
            packageVerificationCodeValue = (Get-SpdxVerificationCode -Sha1Values @($ownEntries | ForEach-Object { $_.checksums[0].checksumValue }))
            packageVerificationCodeExcludedFiles = @($sbomFileName) }
        licenseConcluded = $programLicence; licenseDeclared = $programLicence
        copyrightText = 'Copyright (c) 2026 Fan Zhu'
        comment = "Installed layout of Lucent $($Info.version) for x64 Windows, built from commit $GitCommit. Contains Lucent's own files (the executable, qt.conf, the installed-file list, the bundled data assets such as the camera support database, and the licence bundle) and the files of third-party data assets (the Lensfun lens database), which carry their own licence; the third-party binary files are listed under each component's binary package. Lucent's code is under the MIT License; its logo, inside $(Get-LucentExecutablesText $Info) (branding $($LogoLicence.Version)), is not: it is under $($LogoLicence.Id)." })
    $ids[$lucentId] = $true
    Add-Relationship 'SPDXRef-DOCUMENT' 'DESCRIBES' $lucentId
    foreach ($entry in $ownEntries) {
        $files.Add($entry)
        $ids[$entry.SPDXID] = $true
        Add-Relationship $lucentId 'CONTAINS' $entry.SPDXID
    }

    # Ports, from vcpkg's documents.
    $stagedBySource = @{}
    foreach ($file in ($Report.Files | Where-Object { $_.Origin -eq 'vcpkg' })) {
        $tripletRelative = Get-RelativePathText -Root (Join-Path (ConvertTo-NativePath $Info.vcpkgInstalledDir) $Info.vcpkgTriplet) -Path $file.Source
        $stagedBySource['./' + $tripletRelative.Replace('\', '/').ToLowerInvariant()] = $file
    }
    $droppedRecipeFiles = 0
    $usedExtracted = @{}
    foreach ($component in $Components) {
        $prefix = 'SPDXRef-' + (ConvertTo-SpdxIdPart $component.Port) + '-'
        $map = @{}
        foreach ($package in $component.Spdx.packages) {
            $copy = Copy-SpdxObject $package
            $newId = $prefix + $package.SPDXID.Substring('SPDXRef-'.Length)
            $map[$package.SPDXID] = $newId
            $copy.SPDXID = $newId
            if ($copy.Contains('licenseConcluded') -and $copy.licenseConcluded -like 'LicenseRef-vcpkg-null') { $copy.licenseConcluded = 'NOASSERTION' }
            if ($package.SPDXID -eq 'SPDXRef-port') {
                $copy.licenseDeclared = $component.LicenseDeclared
                $copy.licenseConcluded = $component.LicenseConcluded
                $tree = if ($component.RecipeRef) { $component.RecipeRef.Tree } else { $null }
                if ($component.RecipeRef -and $component.RecipeRef.Overlay) {
                    # An overlay port (vcpkg-configuration.json "overlay-ports"): the recipe is Lucent's
                    # own folder, shipped in the licence bundle; no download location is asserted.
                    $copy.downloadLocation = 'NOASSERTION'
                    $copy.comment = "The vcpkg port (recipe): vcpkg's port with Lucent's build changes, the overlay port $($component.RecipeRef.Overlay) of Lucent's source$(if ($GitCommit) { " at commit $GitCommit" } else { '' }), installed with Lucent in $($component.RecipeRef.ShippedRecipe); its files carry the SHA-1 of that copy."
                }
                else {
                    $copy.comment = 'The vcpkg port (recipe). Its downloadLocation names the git tree of the port folder in the vcpkg registry' +
                        $(if ($component.RecipeRef -and $component.RecipeRef.Url) { ", which is the port at the registry baseline: $($component.RecipeRef.Url)" } elseif ($tree) { " ($tree)" } else { '' }) + '.'
                }
            }
            elseif ($package.SPDXID -eq 'SPDXRef-binary') {
                $copy.licenseConcluded = $component.LicenseConcluded
                $copy.comment = switch ($component.Linkage) {
                    'dynamic' { 'Binary package built by vcpkg; Lucent ships the files listed as CONTAINS.' }
                    'static' { 'Binary package built by vcpkg; Lucent.exe links the static libraries listed as CONTAINS.' }
                    default { 'Header-only library compiled into Lucent.exe.' } }
            }
            if ($component.Comment -and $package.SPDXID -eq 'SPDXRef-port') { $copy.licenseComments = $component.Comment }
            $packages.Add($copy)
            $ids[$newId] = $true
        }
        $portFiles = @()
        if ($component.Spdx.PSObject.Properties.Name -contains 'files') { $portFiles = @($component.Spdx.files) }
        foreach ($file in $portFiles) {
            $sha1 = $null
            $staged = $null
            if ($file.SPDXID -like 'SPDXRef-port-file-*') {
                # Recipe files carry only SHA-256 in vcpkg's document; SPDX 2.3 requires SHA-1. Take it
                # from the port's folder (the repository's overlay port, else the vcpkg ports tree)
                # when that file is byte-identical, else leave the file out.
                $expected = (@($file.checksums) | Where-Object { $_.algorithm -eq 'SHA256' } | Select-Object -First 1).checksumValue
                $relativeFile = ConvertTo-NativePath ($file.fileName -replace '^\./', '')
                $candidate = $null
                if ($component.RecipeRef -and $component.RecipeRef.OverlayPath) { $candidate = Join-Path $component.RecipeRef.OverlayPath $relativeFile }
                elseif ($VcpkgRoot) { $candidate = Join-Path $VcpkgRoot ("ports\$($component.Port)\" + $relativeFile) }
                if ($candidate -and $expected -and (Test-Path -LiteralPath $candidate -PathType Leaf) -and (Get-Sha256 $candidate) -eq $expected.ToLowerInvariant()) {
                    $sha1 = Get-Sha1 $candidate
                }
                else { $droppedRecipeFiles++; continue }
            }
            elseif ($file.SPDXID -like 'SPDXRef-binary-file-*') {
                $staged = $stagedBySource[$file.fileName.ToLowerInvariant()]
                if (-not $staged) { continue }
                $sha1 = Get-Sha1 (Join-Path $StageDir $staged.Path)
            }
            else { continue }
            $copy = Copy-SpdxObject $file
            $newId = $prefix + $file.SPDXID.Substring('SPDXRef-'.Length)
            $map[$file.SPDXID] = $newId
            $copy.SPDXID = $newId
            $copy.checksums = @([ordered]@{ algorithm = 'SHA1'; checksumValue = $sha1 }) + @($file.checksums | Where-Object { $_.algorithm -ne 'SHA1' })
            if ($staged) { $copy.comment = 'Installed as ' + $staged.Path }
            $files.Add($copy)
            $ids[$newId] = $true
        }
        foreach ($relationship in $component.Spdx.relationships) {
            if ($map.ContainsKey($relationship.spdxElementId) -and $map.ContainsKey($relationship.relatedSpdxElement)) {
                Add-Relationship $map[$relationship.spdxElementId] $relationship.relationshipType $map[$relationship.relatedSpdxElement]
            }
        }
        if ($map.ContainsKey('SPDXRef-binary')) {
            $type = if ($component.Linkage -eq 'dynamic') { 'DYNAMIC_LINK' } else { 'STATIC_LINK' }
            Add-Relationship $lucentId $type $map['SPDXRef-binary']
        }

        # Libraries linked statically into Lucent.exe (for example Qt's Windows entry point).
        foreach ($static in $component.StaticLibraries) {
            $staticId = $prefix + 'static-' + (ConvertTo-SpdxIdPart $static.File)
            $packages.Add([ordered]@{
                name = $static.Name; SPDXID = $staticId; versionInfo = $component.Version
                packageFileName = $static.Path
                downloadLocation = 'NOASSERTION'
                filesAnalyzed = $false
                licenseConcluded = $static.LicenseConcluded; licenseDeclared = $static.LicenseDeclared
                copyrightText = $static.Copyright
                comment = "Static library $($static.Path) of the vcpkg port $($component.Port), linked into $(Get-StaticLibraryProgramsText $static $Info). $($static.Comment)" })
            $ids[$staticId] = $true
            foreach ($id in (Get-LicenseIds "$($static.LicenseConcluded) $($static.LicenseDeclared)")) { if ($id -like 'LicenseRef-*') { $usedExtracted[$id] = $component } }
            if ($map.ContainsKey('SPDXRef-binary')) { Add-Relationship $map['SPDXRef-binary'] 'CONTAINS' $staticId }
            Add-Relationship $lucentId 'STATIC_LINK' $staticId
        }

        # Third-party code inside the shipped Qt files, from Qt's own SBOM.
        if ($component.QtSbom) {
            foreach ($attribution in $component.QtSbom.Attributions) {
                $attributionId = $prefix + 'qt-' + (ConvertTo-SpdxIdPart $attribution.Id)
                $entry = [ordered]@{ name = $attribution.Name; SPDXID = $attributionId }
                if ($attribution.Version) { $entry.versionInfo = $attribution.Version }
                $entry.downloadLocation = $(if ($attribution.DownloadLocation) { $attribution.DownloadLocation } else { 'NOASSERTION' })
                $entry.filesAnalyzed = $false
                $entry.licenseConcluded = $attribution.License
                $entry.licenseDeclared = 'NOASSERTION'
                $entry.copyrightText = $(if ($attribution.Copyright) { $attribution.Copyright } else { 'NOASSERTION' })
                $entry.comment = "Third-party code inside $(@($attribution.Files) -join ', '), as recorded by Qt's SBOM ($([System.IO.Path]::GetFileName($component.QtSbom.Path)), package $($attribution.Id))."
                $packages.Add($entry)
                $ids[$attributionId] = $true
                foreach ($id in (Get-LicenseIds $attribution.License)) { if ($id -like 'LicenseRef-*') { $usedExtracted[$id] = $component } }
                if ($map.ContainsKey('SPDXRef-binary')) { Add-Relationship $map['SPDXRef-binary'] 'CONTAINS' $attributionId }
            }
        }
    }

    # Third-party data assets (W13, ADR 0023): a package per asset, with its upstream source; the
    # layout's files of the asset are Lucent's package's files (above) and carry the asset's licence.
    foreach ($asset in @($AssetLicences | Where-Object { $_.ThirdParty })) {
        $assetId = 'SPDXRef-asset-' + (ConvertTo-SpdxIdPart $asset.Id)
        $entry = [ordered]@{ name = $asset.Name; SPDXID = $assetId; versionInfo = $asset.Version }
        $entry.downloadLocation = 'NOASSERTION'
        $comment = "Bundled data asset $($asset.Id) $($asset.Version), installed in $($asset.Folder) and verified against the manifest hash compiled into $($Info.executable)."
        if ($asset.Provenance) {
            $source = $asset.Provenance.source
            $entry.packageFileName = [System.IO.Path]::GetFileName(([uri]$source.archive_url).AbsolutePath)
            $entry.downloadLocation = $source.archive_url
            $entry.checksums = @([ordered]@{ algorithm = 'SHA256'; checksumValue = $source.archive_sha256 })
            $comment += " Upstream: $($source.archive_url) (SHA-256 $($source.archive_sha256), retrieved $($source.retrieved)), generated from $($source.repository) commit $($source.commit) ($($source.commit_date)); modifications: $($asset.Provenance.modifications)."
        }
        if ($asset.Homepage) { $entry.homepage = $asset.Homepage }
        $entry.filesAnalyzed = $false
        $entry.licenseConcluded = $asset.Concluded
        $entry.licenseDeclared = $asset.Concluded
        $entry.copyrightText = $asset.Copyright
        $entry.comment = $comment
        $packages.Add($entry)
        $ids[$assetId] = $true
        Add-Relationship $lucentId 'CONTAINS' $assetId
        if ($assetFileIds.ContainsKey($asset.Id)) {
            foreach ($fileId in $assetFileIds[$asset.Id]) { Add-Relationship $fileId 'GENERATED_FROM' $assetId }
        }
    }

    # The Visual C++ redistributable inside Setup.
    $vcId = 'SPDXRef-VCRedist'
    $packages.Add([ordered]@{
        name = $Policy.vcRedistributable.name; SPDXID = $vcId; versionInfo = $VcRedist.Version
        supplier = 'Organization: Microsoft Corporation'
        packageFileName = 'vc_redist.x64.exe'
        downloadLocation = 'NOASSERTION'
        filesAnalyzed = $false
        checksums = @([ordered]@{ algorithm = 'SHA1'; checksumValue = $VcRedist.Sha1 }, [ordered]@{ algorithm = 'SHA256'; checksumValue = $VcRedist.Sha256 })
        licenseConcluded = $Policy.vcRedistributable.licenseRef; licenseDeclared = $Policy.vcRedistributable.licenseRef
        copyrightText = 'Copyright (c) Microsoft Corporation'
        comment = 'Bundled in Lucent Setup and run only when the installed Visual C++ runtime is older; not part of the installation folder.' })
    $ids[$vcId] = $true
    Add-Relationship $vcId 'RUNTIME_DEPENDENCY_OF' $lucentId

    # Licence texts of the LicenseRef identifiers used above (Qt's SBOM defines its own).
    $extractedInfos = New-Object System.Collections.Generic.List[object]
    $extractedInfos.Add([ordered]@{
        licenseId = $Policy.vcRedistributable.licenseRef
        name = $Policy.vcRedistributable.name
        extractedText = $Policy.vcRedistributable.licenseText })
    $extractedInfos.Add([ordered]@{
        licenseId = $LogoLicence.Id
        name = $LogoLicence.Name
        extractedText = $LogoLicence.Text
        comment = "The terms of Lucent's logo, decided by its owner on $($LogoLicence.Decided) and recorded in Lucent's source (assets/branding/provenance.json, branding $($LogoLicence.Version)). The logo is built into $(Format-NameList (@(@($Info.executable) + @($Info.helperExecutables) | Where-Object { $_ }) + @('Lucent Setup'))); THIRD-PARTY-NOTICES.txt states the same terms." })
    foreach ($id in @(ConvertTo-OrdinalOrder -Items @($usedExtracted.Keys))) {
        $definition = $null
        foreach ($component in $Components) {
            if ($component.QtSbom -and $component.QtSbom.Extracted.ContainsKey($id)) { $definition = $component.QtSbom.Extracted[$id]; break }
        }
        if (-not $definition -or -not $definition.ContainsKey('ExtractedText')) { throw "The SBOM uses $id, but no Qt SBOM defines its text." }
        $extractedInfos.Add([ordered]@{
            licenseId = $id
            name = $(if ($definition.ContainsKey('LicenseName')) { $definition.LicenseName } else { $id })
            extractedText = $definition.ExtractedText })
    }

    $document = [ordered]@{
        spdxVersion = 'SPDX-2.3'
        dataLicense = 'CC0-1.0'
        SPDXID = 'SPDXRef-DOCUMENT'
        name = "Lucent-$($Info.version)-x64"
        documentNamespace = "$($Info.homepage)/spdx/Lucent-$($Info.version)-x64-$GitCommit-" + [guid]::NewGuid().ToString()
        creationInfo = [ordered]@{
            created = $created
            creators = @('Tool: Lucent tools/package.ps1', 'Organization: Lucent project')
            comment = "Aggregated from the vcpkg SPDX documents (share/<port>/vcpkg.spdx.json) of the ports whose files ship, triplet $($Info.vcpkgTriplet), and from Qt's own SBOMs (sbom/<qt port>-<version>.spdx) for the third-party code inside the shipped Qt files. Only shipped binary files are listed; recipe files carry the SHA-1 of the identical file in the vcpkg ports tree, or in the repository's overlay port for a port Lucent overlays ($droppedRecipeFiles left out because that file differs or is missing). Licence conclusions follow installer/license-policy.json (ADR 0003, ADR 0009)." }
        packages = $packages
        files = $files
        relationships = $relationships
        hasExtractedLicensingInfos = $extractedInfos
    }
    $problems = @(Test-SbomDocument -Document $document)
    if ($problems.Count -gt 0) { throw ("The SBOM is not valid SPDX 2.3:`n  " + (($problems | Select-Object -First 20) -join "`n  ")) }
    Write-JsonFile -Path $Path -Value $document
    return [pscustomobject]@{ Packages = $packages.Count; Files = $files.Count; Relationships = $relationships.Count; DroppedRecipeFiles = $droppedRecipeFiles }
}

# --- Builder paths (W12) -------------------------------------------------------------------------

# One character per byte: regular expressions over this view of a file's bytes find text in any
# encoding, and a match's index is its byte offset.
$script:Latin1 = [System.Text.Encoding]::GetEncoding(28591)

function Get-BuilderPathRules {
    <#
    .SYNOPSIS
        The folders of the build machine that no shipped file may name, most specific first: the
        vcpkg installed tree, the build tree, the source tree and the repository, the vcpkg
        buildtrees, packages and downloads folders, the vcpkg root, and the user profile; from the
        build's packaging facts (configure time) and this process's environment, with their 8.3
        short forms where those differ.
    .DESCRIPTION
        A folder on a drive matches with its drive letter (C:\x, or /c/x); the user profile also
        without it (\Users\<name>). New-BuilderPathScanner adds a rule for any user's profile
        folder (X:\Users\<name>\), which also catches binaries a binary cache brought from another
        machine.
    .OUTPUTS
        Objects with Name, Path (full, no trailing separator) and DriveOptional.
    #>
    param([Parameter(Mandatory)]$Info, [Parameter(Mandatory)][string]$RepoRoot)
    $candidates = @(
        [pscustomobject]@{ Name = 'vcpkg installed tree'; Path = $Info.vcpkgInstalledDir; DriveOptional = $false },
        [pscustomobject]@{ Name = 'build tree'; Path = $Info.binaryDir; DriveOptional = $false },
        [pscustomobject]@{ Name = 'source tree'; Path = $Info.sourceDir; DriveOptional = $false },
        [pscustomobject]@{ Name = 'repository'; Path = $RepoRoot; DriveOptional = $false },
        [pscustomobject]@{ Name = 'vcpkg buildtrees folder'; Path = $Info.vcpkgBuildtreesDir; DriveOptional = $false },
        [pscustomobject]@{ Name = 'vcpkg buildtrees folder'; Path = $env:LUCENT_VCPKG_BUILDTREES; DriveOptional = $false },
        [pscustomobject]@{ Name = 'vcpkg packages folder'; Path = $Info.vcpkgPackagesDir; DriveOptional = $false },
        [pscustomobject]@{ Name = 'vcpkg downloads folder'; Path = $Info.vcpkgDownloadsDir; DriveOptional = $false },
        [pscustomobject]@{ Name = 'vcpkg downloads folder'; Path = $env:VCPKG_DOWNLOADS; DriveOptional = $false },
        [pscustomobject]@{ Name = 'vcpkg root'; Path = $Info.vcpkgRoot; DriveOptional = $false },
        [pscustomobject]@{ Name = 'vcpkg root'; Path = $env:VCPKG_ROOT; DriveOptional = $false },
        [pscustomobject]@{ Name = 'user profile'; Path = $Info.userProfile; DriveOptional = $true },
        [pscustomobject]@{ Name = 'user profile'; Path = $env:USERPROFILE; DriveOptional = $true })
    $fileSystem = $null
    try { $fileSystem = New-Object -ComObject Scripting.FileSystemObject } catch { $fileSystem = $null }
    $rules = New-Object System.Collections.Generic.List[object]
    $seen = @{}
    foreach ($candidate in $candidates) {
        $path = [string]$candidate.Path
        if (-not $path) { continue }
        $path = $path.Replace('/', '\')
        if (-not [System.IO.Path]::IsPathRooted($path)) { continue }
        try { $path = [System.IO.Path]::GetFullPath($path).TrimEnd('\') } catch { continue }
        if ($path.Length -le 3) { continue }   # a drive root would match every path on the drive
        $forms = @($path)
        if ($fileSystem -and (Test-Path -LiteralPath $path -PathType Container)) {
            try {
                $short = [string]$fileSystem.GetFolder($path).ShortPath
                if ($short -and $short -ine $path) { $forms += $short.TrimEnd('\') }
            }
            catch { }
        }
        foreach ($form in $forms) {
            $key = $form.ToLowerInvariant()
            if ($seen.ContainsKey($key)) { continue }
            $seen[$key] = $true
            # Without its drive letter only a folder at least two levels deep (\Users\<name>).
            $optional = $candidate.DriveOptional -and (@($form.Substring(2).Split('\') | Where-Object { $_ }).Count -ge 2)
            $rules.Add([pscustomobject]@{ Name = $candidate.Name; Path = $form; DriveOptional = $optional })
        }
    }
    # Most specific first: a match is reported under the longest folder it names.
    return @($rules | Sort-Object -Property @{ Expression = { $_.Path.Length }; Descending = $true }, Name)
}

function ConvertTo-BuilderPathPattern {
    # Regular expression (over the Latin1 view of a file) for $Path in the encoding $Encoding (UTF-8,
    # ANSI or UTF-16LE): separators match / or \, repeated (\\ in JSON and C strings); ASCII letters
    # match either case; other characters their encoded bytes. $null when the ANSI code page cannot
    # represent the path. A match starts at the separator after the drive, which the pattern checks
    # behind it (X:, also \\?\X:, or the MSYS form /x): every match then starts with \ or /, so the
    # regular expression engine skips straight to those bytes (Find-BuilderPathsInBytes reports the
    # offset of the drive letter).
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][bool]$DriveOptional, [Parameter(Mandatory)][string]$Encoding)
    $wide = ($Encoding -eq 'UTF-16LE')
    $codec = switch ($Encoding) {
        'UTF-8' { New-Object System.Text.UTF8Encoding($false) }
        'ANSI' { [System.Text.Encoding]::Default }
        default { [System.Text.Encoding]::Unicode } }
    if ($Encoding -eq 'ANSI' -and $codec.GetString($codec.GetBytes($Path)) -cne $Path) { return $null }
    $nul = if ($wide) { '\x00' } else { '' }
    $separator = "(?:[\\/]$nul)+"
    $builder = New-Object System.Text.StringBuilder
    $rest = $Path
    if ($Path -match '^([A-Za-z]):') {
        $rest = $Path.Substring(2)
        if (-not $DriveOptional) {
            $letter = "[$($Matches[1].ToLowerInvariant())$($Matches[1].ToUpperInvariant())]"
            [void]$builder.Append("(?<=$letter$($nul):$nul|[\\/]$nul$letter$nul)")
        }
    }
    foreach ($segment in ($rest -split '[\\/]+' | Where-Object { $_ })) {
        [void]$builder.Append($separator)
        foreach ($char in $segment.ToCharArray()) {
            if (($char -ge 'a' -and $char -le 'z') -or ($char -ge 'A' -and $char -le 'Z')) {
                [void]$builder.Append("[$([char]::ToLowerInvariant($char))$([char]::ToUpperInvariant($char))]$nul")
            }
            else {
                foreach ($byte in $codec.GetBytes([string]$char)) { [void]$builder.Append(('\x{0:X2}' -f $byte)) }
            }
        }
    }
    return $builder.ToString()
}

function New-BuilderPathScanner {
    <#
    .SYNOPSIS
        Compiles the rules of Get-BuilderPathRules, in UTF-8, the ANSI code page (when it encodes a
        path differently) and UTF-16LE, plus a rule for any user's profile folder
        (X:\Users\<name>\, except Public, Default and All Users), into one regular expression.
    #>
    param([Parameter(Mandatory)][object[]]$Rules)
    $alternatives = New-Object System.Collections.Generic.List[string]
    $groups = New-Object System.Collections.Generic.List[object]
    foreach ($rule in $Rules) {
        $patterns = @{}
        foreach ($encoding in 'UTF-8', 'ANSI', 'UTF-16LE') {
            $pattern = ConvertTo-BuilderPathPattern -Path $rule.Path -DriveOptional $rule.DriveOptional -Encoding $encoding
            if (-not $pattern -or $patterns.ContainsKey($pattern)) { continue }
            $patterns[$pattern] = $true
            $group = "g$($groups.Count)"
            $alternatives.Add("(?<$group>$pattern)")
            $label = if ($encoding -eq 'UTF-8' -and $rule.Path -match '^[\x20-\x7E]+$') { '8-bit' } else { $encoding }
            $groups.Add([pscustomobject]@{ Group = $group; Rule = $rule.Name; Folder = $rule.Path; Encoding = $label })
        }
    }
    foreach ($wide in $false, $true) {
        $nul = if ($wide) { '\x00' } else { '' }
        $word = {
            param([string]$Text)
            (($Text.ToCharArray() | ForEach-Object {
                if ($_ -match '[A-Za-z]') { "[$([char]::ToLowerInvariant($_))$([char]::ToUpperInvariant($_))]$nul" } else { "$_$nul" } }) -join '')
        }
        $separator = "(?:[\\/]$nul)+"
        $end = "(?:[\\/ `"]$nul|\x00$nul|$)"
        $excluded = '(?!(?:' + ((& $word 'Public'), (& $word 'Default'), (& $word 'All Users') -join '|') + ")$end)"
        # One character of the user's name. In UTF-16LE also any code unit above U+00FF (a non-zero high
        # byte: CJK, Cyrillic, Greek, surrogates), none of which is a separator or a control character
        # (W12 review SEC07-02; UTF-8 encodes those characters as bytes the 8-bit form accepts).
        $nameUnit = if ($wide) { "(?:[^\\/\x00-\x1F:*?`"<>|]\x00|[\x00-\xFF][\x01-\xFF])" } else { "[^\\/\x00-\x1F:*?`"<>|]" }
        $pattern = "(?<=[A-Za-z]$($nul):$nul|[\\/]$nul[A-Za-z]$nul)$separator$(& $word 'Users')$separator$excluded" +
                   "$nameUnit{1,64}[\\/]$nul"
        $group = "g$($groups.Count)"
        $alternatives.Add("(?<$group>$pattern)")
        $groups.Add([pscustomobject]@{ Group = $group; Rule = 'user profile folder (any user)'; Folder = 'X:\Users\<name>'
                                       Encoding = $(if ($wide) { 'UTF-16LE' } else { '8-bit' }) })
    }
    $options = [System.Text.RegularExpressions.RegexOptions]::CultureInvariant -bor [System.Text.RegularExpressions.RegexOptions]::Compiled
    return [pscustomobject]@{
        Regex = New-Object System.Text.RegularExpressions.Regex(($alternatives -join '|'), $options)
        Groups = $groups.ToArray()
        Rules = $Rules }
}

function Get-PeDebugRanges {
    # File ranges of a PE file's debug directory entries (type 2: CodeView, the PDB path), to say
    # where a match lies. Empty for anything that is not a well-formed PE file. ($Bytes is untyped: a
    # [byte[]] parameter makes PowerShell enumerate the whole array to bind it, about 90 ms per MB.)
    param($Bytes)
    $ranges = New-Object System.Collections.Generic.List[object]
    try {
        if ($Bytes.Length -lt 0x40 -or $Bytes[0] -ne 0x4D -or $Bytes[1] -ne 0x5A) { return @() }
        $pe = [BitConverter]::ToInt32($Bytes, 0x3C)
        if ($pe -lt 0 -or $pe + 0x18 -gt $Bytes.Length -or [BitConverter]::ToUInt32($Bytes, $pe) -ne 0x4550) { return @() }
        $sectionCount = [BitConverter]::ToUInt16($Bytes, $pe + 6)
        $optional = $pe + 0x18
        $directories = if ([BitConverter]::ToUInt16($Bytes, $optional) -eq 0x20B) { $optional + 112 } else { $optional + 96 }
        $debugRva = [BitConverter]::ToUInt32($Bytes, $directories + 6 * 8)
        $debugSize = [BitConverter]::ToUInt32($Bytes, $directories + 6 * 8 + 4)
        if ($debugRva -eq 0) { return @() }
        $sections = $optional + [BitConverter]::ToUInt16($Bytes, $pe + 20)
        $debugOffset = -1
        for ($i = 0; $i -lt $sectionCount; $i++) {
            $section = $sections + 40 * $i
            $virtualAddress = [BitConverter]::ToUInt32($Bytes, $section + 12)
            $span = [Math]::Max([BitConverter]::ToUInt32($Bytes, $section + 8), [BitConverter]::ToUInt32($Bytes, $section + 16))
            if ($debugRva -ge $virtualAddress -and $debugRva -lt $virtualAddress + $span) {
                $debugOffset = [BitConverter]::ToUInt32($Bytes, $section + 20) + ($debugRva - $virtualAddress)
            }
        }
        if ($debugOffset -lt 0) { return @() }
        for ($entry = 0; $entry -lt [int][Math]::Floor($debugSize / 28); $entry++) {
            $at = $debugOffset + 28 * $entry
            $type = [BitConverter]::ToUInt32($Bytes, $at + 12)
            $size = [BitConverter]::ToUInt32($Bytes, $at + 16)
            $pointer = [BitConverter]::ToUInt32($Bytes, $at + 24)
            $description = if ($type -eq 2) { 'PE debug directory (CodeView: the PDB path)' } else { "PE debug directory (type $type)" }
            $ranges.Add([pscustomobject]@{ Start = [int64]$pointer; End = [int64]$pointer + $size; Description = $description })
        }
    }
    catch { return @() }
    return $ranges.ToArray()
}

function Get-PrintableContext {
    # The printable text around a match (ASCII; UTF-16LE when $Wide), at most 100 characters on
    # each side.
    param([Parameter(Mandatory)][string]$Text, [int]$Index, [int]$Length, [bool]$Wide)
    $step = if ($Wide) { 2 } else { 1 }
    $start = $Index
    $limit = 100 * $step
    while ($start - $step -ge 0 -and $Index - $start -lt $limit) {
        $char = [int]$Text[$start - $step]
        if ($char -lt 0x20 -or $char -gt 0x7E -or ($Wide -and [int]$Text[$start - 1] -ne 0)) { break }
        $start -= $step
    }
    $end = $Index + $Length
    while ($end + $step - 1 -lt $Text.Length -and $end - ($Index + $Length) -lt $limit) {
        $char = [int]$Text[$end]
        if ($char -lt 0x20 -or $char -gt 0x7E -or ($Wide -and [int]$Text[$end + 1] -ne 0)) { break }
        $end += $step
    }
    $context = $Text.Substring($start, $end - $start)
    if ($Wide) { $context = $context.Replace([string][char]0, '') }
    return $context
}

function Find-BuilderPathsInBytes {
    # The builder-path matches in one file's (or text's) bytes (a byte[], untyped as in
    # Get-PeDebugRanges); $Name is how reports name it.
    param([Parameter(Mandatory)]$Scanner, $Bytes, [Parameter(Mandatory)][string]$Name)
    $hits = New-Object System.Collections.Generic.List[object]
    if ($Bytes.Length -eq 0) { return @() }
    $text = $script:Latin1.GetString($Bytes)
    $debugRanges = $null
    foreach ($match in $Scanner.Regex.Matches($text)) {
        $group = $null
        foreach ($candidate in $Scanner.Groups) {
            if ($match.Groups[$candidate.Group].Success) { $group = $candidate; break }
        }
        # Matches start at the separator after the drive: report the offset of the drive (X: or /x).
        $wide = ($group.Encoding -eq 'UTF-16LE')
        $unit = if ($wide) { 2 } else { 1 }
        $start = $match.Index
        if ($start -ge 2 * $unit) {
            $first = [string]$text[$start - 2 * $unit]
            $second = [string]$text[$start - $unit]
            if (($first -match '^[A-Za-z]$' -and $second -eq ':') -or ($first -match '^[\\/]$' -and $second -match '^[A-Za-z]$')) { $start -= 2 * $unit }
        }
        if ($null -eq $debugRanges) { $debugRanges = @(Get-PeDebugRanges -Bytes $Bytes) }
        $location = 'file data'
        foreach ($range in $debugRanges) { if ($start -ge $range.Start -and $start -lt $range.End) { $location = $range.Description } }
        $hits.Add([pscustomobject]@{
            File = $Name; Offset = [int64]$start; Encoding = $group.Encoding; Rule = $group.Rule; Folder = $group.Folder
            Location = $location; Context = (Get-PrintableContext -Text $text -Index $start -Length ($match.Index + $match.Length - $start) -Wide $wide) })
    }
    return $hits.ToArray()
}

# Compressed content (W12 review SEC-07): the most one stream may decompress to, and everything one
# scanned file decompresses in all: every part at every depth (and Setup's LZMA parts), and what a
# decoding produced before it proved not to be a stream, against one budget per file (W12 review
# SEC07-01), with a time limit. More cannot be scanned, so it fails the audit.
$script:CompressedStreamLimit = [int64]256MB
$script:CompressedTotalLimit = [int64]1GB
$script:CompressedSecondsLimit = 300
# How deep compressed content inside decompressed content is followed (a file's own streams: depth 1).
$script:CompressedDepthLimit = 2

function Initialize-CompressedContentType {
    # The decoders of LucentCompressedContent.cs (zlib, gzip, LZMA, LZMA2, Inno Setup's layout).
    if ('LucentPackaging.DecodeBudget' -as [type]) { return }
    Add-Type -TypeDefinition ([System.IO.File]::ReadAllText((Join-Path $PSScriptRoot 'LucentCompressedContent.cs'))) -Language CSharp
}

function New-CompressedContentBudget {
    <#
    .SYNOPSIS
        What one scanned file may decompress in all (a LucentPackaging.DecodeBudget): -Bytes (default
        1 GiB) at every depth, and -Seconds (default 300). Find-BuilderPathsInContent makes one for each
        file it is given without one; tests pass a smaller one.
    #>
    param([int64]$Bytes = $script:CompressedTotalLimit, [double]$Seconds = $script:CompressedSecondsLimit)
    Initialize-CompressedContentType
    return New-Object LucentPackaging.DecodeBudget($Bytes, $Seconds)
}

function New-CompressedContentTally {
    # What a scan decompressed: parts by kind, their bytes, and what it could not decompress.
    return [pscustomobject]@{ Kinds = @{}; Streams = 0; Bytes = [int64]0; Problems = (New-Object System.Collections.Generic.List[string]) }
}

function Find-BuilderPathsInContent {
    <#
    .SYNOPSIS
        The builder-path matches in a file's bytes and in what it compresses (W12 review SEC-07): every
        zlib and gzip stream that decodes completely with a matching checksum (Qt's rcc resources,
        qCompress data, PNG data, Qt's gzip-compressed MIME database), followed into what those hold
        to a depth of $script:CompressedDepthLimit, and with -InnoSetup an Inno Setup executable's
        LZMA-compressed setup data (the compiled script, messages, file names), setup engine and
        installed files (LucentCompressedContent.cs). A hit in decompressed content names the file,
        the part's kind and offset, and the offset in the decompressed data. What cannot be
        decompressed is a problem in -Tally, which fails the audit: a zstd frame (no Lucent build
        compresses with zstd; Qt is built without it), a stream beyond the size limit, content beyond
        the file's budget (-Budget: what the scanned file decompresses in all, at every depth, and the
        time it takes; W12 review SEC07-01; without -Budget a new one of 1 GiB and 300 s), an Inno
        Setup layout this reader does not know.
    #>
    param([Parameter(Mandatory)]$Scanner, $Bytes, [Parameter(Mandatory)][string]$Name, [Parameter(Mandatory)]$Tally,
          [switch]$InnoSetup, [int]$Depth = 0, $Budget)
    Initialize-CompressedContentType
    if ($null -eq $Budget) { $Budget = New-CompressedContentBudget }
    $hits = New-Object System.Collections.Generic.List[object]
    foreach ($hit in (Find-BuilderPathsInBytes -Scanner $Scanner -Bytes $Bytes -Name $Name)) { $hits.Add($hit) }
    if ($null -eq $Bytes -or $Bytes.Length -lt 3) { return $hits.ToArray() }
    foreach ($offset in [LucentPackaging.CompressedContent]::FindZstdFrames($Bytes)) {
        $Tally.Problems.Add(('{0} at offset 0x{1:X8}: zstd-compressed data, which the scan cannot decompress' -f $Name, $offset))
    }
    # A spent budget was reported where it ran out; nothing more is decompressed for this file.
    if ($Budget.Exhausted) { return $hits.ToArray() }
    $parts = New-Object System.Collections.Generic.List[object]
    try {
        foreach ($part in [LucentPackaging.CompressedContent]::FindDeflateStreams($Bytes, $script:CompressedStreamLimit, $Budget)) { $parts.Add($part) }
    }
    catch {
        $reason = if ($_.Exception.InnerException) { $_.Exception.InnerException.Message } else { $_.Exception.Message }
        $Tally.Problems.Add("$($Name): its zlib or gzip content cannot be decompressed for the scan ($reason)")
    }
    if ($InnoSetup -and -not $Budget.Exhausted) {
        try {
            foreach ($part in [LucentPackaging.InnoSetupContent]::Extract($Bytes, $Budget)) { $parts.Add($part) }
        }
        catch {
            $reason = if ($_.Exception.InnerException) { $_.Exception.InnerException.Message } else { $_.Exception.Message }
            $Tally.Problems.Add("$($Name): Inno Setup's compressed content cannot be decompressed for the scan ($reason)")
        }
    }
    foreach ($part in $parts) {
        $kind = $part.Kind -replace ', (block|chunk) \d+$', ''
        $Tally.Kinds[$kind] = 1 + $(if ($Tally.Kinds.ContainsKey($kind)) { $Tally.Kinds[$kind] } else { 0 })
        $Tally.Streams++
        $Tally.Bytes += $part.Data.Length
        $partName = '{0} [{1} at 0x{2:X8}]' -f $Name, $part.Kind, $part.Offset
        if ($Depth + 1 -lt $script:CompressedDepthLimit) {
            $found = @(Find-BuilderPathsInContent -Scanner $Scanner -Bytes $part.Data -Name $partName -Tally $Tally -Depth ($Depth + 1) -Budget $Budget)
        }
        else {
            $found = @(Find-BuilderPathsInBytes -Scanner $Scanner -Bytes $part.Data -Name $partName)
        }
        foreach ($hit in $found) {
            if ($hit.Location -eq 'file data') { $hit.Location = 'decompressed data' }
            $hits.Add($hit)
        }
    }
    return $hits.ToArray()
}

function Format-CompressedContentTally {
    # One line: what a scan decompressed.
    param([Parameter(Mandatory)]$Tally)
    if ($Tally.Streams -eq 0) { return 'no compressed content' }
    $kinds = @($Tally.Kinds.Keys | Sort-Object | ForEach-Object { '{0} {1}' -f $Tally.Kinds[$_], $_ }) -join ', '
    return ('{0} compressed part(s) decompressed and scanned ({1}), {2:N1} MiB' -f $Tally.Streams, $kinds, ($Tally.Bytes / 1MB))
}

function ConvertTo-CompressedContentRecord {
    # The compressed content of a scan report for builder-paths.json.
    param([Parameter(Mandatory)]$Report)
    $kinds = [ordered]@{}
    foreach ($name in ($Report.Compressed.Kinds.Keys | Sort-Object)) { $kinds[$name] = $Report.Compressed.Kinds[$name] }
    return [ordered]@{ parts = $Report.Compressed.Streams; decompressedBytes = $Report.Compressed.Bytes; kinds = $kinds
        problems = @($Report.CompressedProblems) }
}

function Invoke-BuilderPathAudit {
    <#
    .SYNOPSIS
        Scans every byte of every file below -Root (or the single file -Path, or the text -Text as
        UTF-8, or each of -Blobs: objects with Name and Bytes) for the build machine's folders
        (New-BuilderPathScanner), whatever the file type: the PDB paths in PE debug directories,
        __FILE__ strings, licence texts, the SBOM, QML type information, the corresponding-source
        package's own files; and what they compress (Find-BuilderPathsInContent: zlib and gzip
        streams, and with -InnoSetup the -Path Setup's LZMA-compressed parts; W12 review SEC-07).
        Returns a report; each hit becomes an error naming the file and the byte offset, and content
        the scan cannot decompress is an error too.
    #>
    param([Parameter(Mandatory)]$Scanner, [string]$Root, [string]$Path, [string]$Text, [string]$Name, [string]$Label = 'Scan',
          [AllowEmptyCollection()][object[]]$Blobs = @(), [switch]$InnoSetup)
    $hits = New-Object System.Collections.Generic.List[object]
    $tally = New-CompressedContentTally
    $files = 0
    $bytes = [int64]0
    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    if ($Root) {
        # -Force: hidden and system files are scanned too.
        foreach ($file in (Get-ChildItem -LiteralPath $Root -Recurse -File -Force | Sort-Object FullName)) {
            $content = [System.IO.File]::ReadAllBytes($file.FullName)
            foreach ($hit in (Find-BuilderPathsInContent -Scanner $Scanner -Bytes $content -Name (Get-RelativePathText -Root $Root -Path $file.FullName) -Tally $tally)) { $hits.Add($hit) }
            $files++
            $bytes += $content.Length
        }
    }
    if ($Path) {
        $content = [System.IO.File]::ReadAllBytes($Path)
        foreach ($hit in (Find-BuilderPathsInContent -Scanner $Scanner -Bytes $content -Name $(if ($Name) { $Name } else { Split-Path -Leaf $Path }) -Tally $tally -InnoSetup:$InnoSetup)) { $hits.Add($hit) }
        $files++
        $bytes += $content.Length
    }
    if ($PSBoundParameters.ContainsKey('Text')) {
        $content = (New-Object System.Text.UTF8Encoding($false)).GetBytes($Text)
        foreach ($hit in (Find-BuilderPathsInContent -Scanner $Scanner -Bytes $content -Name $(if ($Name) { $Name } else { 'text' }) -Tally $tally)) { $hits.Add($hit) }
        $files++
        $bytes += $content.Length
    }
    foreach ($blob in $Blobs) {
        foreach ($hit in (Find-BuilderPathsInContent -Scanner $Scanner -Bytes $blob.Bytes -Name $blob.Name -Tally $tally)) { $hits.Add($hit) }
        $files++
        $bytes += $blob.Bytes.Length
    }
    $errors = New-Object System.Collections.Generic.List[string]
    foreach ($hit in $hits) {
        $errors.Add(('{0} at offset 0x{1:X8} ({2}, {3}) names the {4} {5}: {6}' -f $hit.File, $hit.Offset, $hit.Encoding, $hit.Location, $hit.Rule, $hit.Folder, $hit.Context))
    }
    foreach ($problem in $tally.Problems) { $errors.Add($problem) }
    return [pscustomobject]@{
        Label = $Label; Files = $files; Bytes = $bytes; Seconds = [Math]::Round($timer.Elapsed.TotalSeconds, 1)
        Hits = $hits.ToArray(); Errors = $errors.ToArray(); HitFiles = @($hits | ForEach-Object File | Sort-Object -Unique)
        Compressed = $tally; CompressedProblems = @($tally.Problems) }
}

function Write-BuilderPathReport {
    # builder-paths.txt and .json: the folders searched, what was scanned and every hit. Local build
    # outputs (they name the folders); never shipped.
    param([Parameter(Mandatory)][object[]]$Reports, [Parameter(Mandatory)]$Scanner, [Parameter(Mandatory)][string]$TextPath, [Parameter(Mandatory)][string]$JsonPath)
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add('Lucent package audit: folders of the build machine in shipped files')
    $lines.Add('Folders searched (8-bit, ANSI and UTF-16LE text, either separator, any letter case):')
    foreach ($rule in $Scanner.Rules) { $lines.Add(('  {0,-26} {1}{2}' -f $rule.Name, $rule.Path, $(if ($rule.DriveOptional) { ' (also without the drive letter)' } else { '' }))) }
    $lines.Add('  user profile folder (any user): X:\Users\<name>\ except Public, Default and All Users')
    $lines.Add('')
    $lines.Add('Compressed content is decompressed and scanned too (W12 review SEC-07): zlib and gzip streams, and Setup''s LZMA-compressed parts.')
    $lines.Add('')
    foreach ($report in $Reports) {
        $lines.Add(('{0}: {1} file(s), {2:N0} bytes, {3} s; {4} hit(s) in {5} file(s); {6}' -f $report.Label, $report.Files, $report.Bytes, $report.Seconds,
            @($report.Hits).Count, @($report.HitFiles).Count, (Format-CompressedContentTally $report.Compressed)))
        foreach ($problem in $report.Errors) { $lines.Add("  $problem") }
    }
    Write-TextFile -Path $TextPath -Text (($lines -join "`n") + "`n")
    Write-JsonFile -Path $JsonPath -Value ([ordered]@{
        folders = @($Scanner.Rules | ForEach-Object { [ordered]@{ name = $_.Name; path = $_.Path; driveOptional = $_.DriveOptional } })
        scans = @($Reports | ForEach-Object { [ordered]@{
            label = $_.Label; files = $_.Files; bytes = $_.Bytes; seconds = $_.Seconds
            compressed = (ConvertTo-CompressedContentRecord $_)
            hits = @($_.Hits | ForEach-Object { [ordered]@{ file = $_.File; offset = $_.Offset; encoding = $_.Encoding; location = $_.Location; rule = $_.Rule; folder = $_.Folder; context = $_.Context } }) } }) })
}

# --- Authenticode (W12) --------------------------------------------------------------------------

function Get-CodeSigningCertificate {
    <#
    .SYNOPSIS
        The certificate LUCENT_SIGN_CERT_SHA1 names, checked before anything is signed: in the
        current user's personal store (Cert:\CurrentUser\My, where signtool /sha1 looks), with its
        private key, valid now, and usable for code signing (the Code Signing extended key usage, or
        no usage restriction). The thumbprint may be pasted from the certificate dialog: spaces and
        invisible characters are dropped.
    #>
    param([Parameter(Mandatory)][string]$Thumbprint)
    $normalized = ($Thumbprint -replace '[^0-9A-Fa-f]', '').ToUpperInvariant()
    if ($normalized.Length -ne 40) { throw "LUCENT_SIGN_CERT_SHA1 '$Thumbprint' is not a certificate thumbprint (40 hexadecimal digits)." }
    $storePath = "Cert:\CurrentUser\My\$normalized"
    if (-not (Test-Path -LiteralPath $storePath)) { throw "No certificate with the thumbprint $normalized is in the current user's personal store (Cert:\CurrentUser\My)." }
    $certificate = Get-Item -LiteralPath $storePath
    if (-not $certificate.HasPrivateKey) { throw "The certificate $normalized ($($certificate.Subject)) has no private key in this store." }
    $now = Get-Date
    if ($certificate.NotBefore -gt $now -or $certificate.NotAfter -lt $now) {
        throw "The certificate $normalized ($($certificate.Subject)) is valid from $($certificate.NotBefore) to $($certificate.NotAfter), not now."
    }
    $usages = @($certificate.Extensions | Where-Object { $_ -is [System.Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension] } |
        ForEach-Object { $_.EnhancedKeyUsages } | ForEach-Object { $_.Value })
    $usageExtension = @($certificate.Extensions | Where-Object { $_ -is [System.Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension] }).Count -gt 0
    if ($usageExtension -and $usages -notcontains '1.3.6.1.5.5.7.3.3') {
        throw "The certificate $normalized ($($certificate.Subject)) is not a code-signing certificate (no Code Signing extended key usage)."
    }
    return [pscustomobject]@{
        Thumbprint = $normalized; Subject = $certificate.Subject; Issuer = $certificate.Issuer
        NotBefore = $certificate.NotBefore; NotAfter = $certificate.NotAfter; SelfSigned = ($certificate.Subject -eq $certificate.Issuer) }
}

function Initialize-AuthenticodeType {
    if ('LucentPackaging.Authenticode' -as [type]) { return }
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace LucentPackaging {
    public static class Authenticode {
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct FileInfo {
            public uint StructSize;
            public string FilePath;
            public IntPtr File;
            public IntPtr KnownSubject;
        }
        [StructLayout(LayoutKind.Sequential)]
        private struct TrustData {
            public uint StructSize;
            public IntPtr PolicyCallbackData;
            public IntPtr SipClientData;
            public uint UiChoice;
            public uint RevocationChecks;
            public uint UnionChoice;
            public IntPtr Info;
            public uint StateAction;
            public IntPtr StateData;
            public IntPtr UrlReference;
            public uint ProviderFlags;
            public uint UiContext;
            public IntPtr SignatureSettings;
        }
        [DllImport("wintrust.dll", ExactSpelling = true)]
        private static extern int WinVerifyTrust(IntPtr window, [MarshalAs(UnmanagedType.LPStruct)] Guid action, ref TrustData data);
        // WinVerifyTrust with WINTRUST_ACTION_GENERIC_VERIFY_V2, no user interface, no revocation
        // check and no network retrieval: 0 when the signature verifies and chains to a trusted
        // root offline, otherwise the HRESULT (0x800B0109 CERT_E_UNTRUSTEDROOT, 0x80096010
        // TRUST_E_BAD_DIGEST, 0x800B0100 TRUST_E_NOSIGNATURE, ...).
        public static int Verify(string path) {
            Guid action = new Guid("00AAC56B-CD44-11d0-8CC2-00C04FC295EE");
            FileInfo file = new FileInfo();
            file.StructSize = (uint)Marshal.SizeOf(typeof(FileInfo));
            file.FilePath = path;
            IntPtr filePointer = Marshal.AllocHGlobal(Marshal.SizeOf(typeof(FileInfo)));
            try {
                Marshal.StructureToPtr(file, filePointer, false);
                TrustData data = new TrustData();
                data.StructSize = (uint)Marshal.SizeOf(typeof(TrustData));
                data.UiChoice = 2;                  // WTD_UI_NONE
                data.RevocationChecks = 0;          // WTD_REVOKE_NONE
                data.UnionChoice = 1;               // WTD_CHOICE_FILE
                data.Info = filePointer;
                data.StateAction = 1;               // WTD_STATEACTION_VERIFY
                data.ProviderFlags = 0x10 | 0x1000; // WTD_REVOCATION_CHECK_NONE | WTD_CACHE_ONLY_URL_RETRIEVAL
                int result = WinVerifyTrust(new IntPtr(-1), action, ref data);
                data.StateAction = 2;               // WTD_STATEACTION_CLOSE
                WinVerifyTrust(new IntPtr(-1), action, ref data);
                return result;
            }
            finally {
                Marshal.DestroyStructure(filePointer, typeof(FileInfo));
                Marshal.FreeHGlobal(filePointer);
            }
        }
    }
}
'@
}

function Test-AuthenticodeFile {
    <#
    .SYNOPSIS
        Checks the Authenticode signature of a file: present, by the certificate with -Thumbprint,
        verifying offline (WinVerifyTrust: digest and chain, no revocation check, no network). With
        -AllowUntrustedRoot (a test certificate, LUCENT_SIGN_TEST) the one accepted failure is
        CERT_E_UNTRUSTEDROOT, the chain ending in a root that this machine does not trust; the
        digest must still match. With -RequireTimestamp the signature must carry a timestamp.
    .OUTPUTS
        Path, Signer, TrustResult (hex), Trust ('trusted', 'untrusted root', or the failure),
        Timestamped, Accepted, Problem.
    #>
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][string]$Thumbprint,
          [switch]$AllowUntrustedRoot, [switch]$RequireTimestamp)
    Initialize-AuthenticodeType
    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    $signer = if ($signature.SignerCertificate) { $signature.SignerCertificate.Thumbprint.ToUpperInvariant() } else { $null }
    $code = [LucentPackaging.Authenticode]::Verify([System.IO.Path]::GetFullPath($Path))
    $hex = '0x{0:X8}' -f $code
    $trust = switch ($hex) { '0x00000000' { 'trusted' } '0x800B0109' { 'untrusted root' } '0x800B0100' { 'not signed' } '0x80096010' { 'digest mismatch' } default { "WinVerifyTrust $hex" } }
    $timestamped = [bool]$signature.TimeStamperCertificate
    $problem = $null
    if (-not $signer) { $problem = $(if ($hex -eq '0x800B0100') { 'is not signed' } else { "has no readable signature ($trust)" }) }
    elseif ($signer -ne $Thumbprint.ToUpperInvariant()) { $problem = "is signed by $signer, not by $Thumbprint" }
    elseif ($hex -eq '0x800B0109' -and -not $AllowUntrustedRoot) { $problem = 'has a signature that chains to a root this machine does not trust (a test certificate needs LUCENT_SIGN_TEST=1)' }
    elseif ($hex -ne '0x00000000' -and $hex -ne '0x800B0109') { $problem = "has a signature that does not verify ($trust)" }
    elseif ($RequireTimestamp -and -not $timestamped) { $problem = 'has a signature without a timestamp' }
    return [pscustomobject]@{
        Path = $Path; Signer = $signer; TrustResult = $hex; Trust = $trust; Timestamped = $timestamped
        Accepted = ($null -eq $problem); Problem = $problem }
}

# --- Inno Setup ----------------------------------------------------------------------------------

function Test-InnoSetupCompiler {
    param([string]$Path)
    if (-not $Path -or -not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $false }
    $banner = Invoke-NativeCommand -FilePath $Path -Arguments @('/?')
    return [bool]($banner.Output | Where-Object { $_ -match 'Inno Setup 6' })
}

function Find-InnoSetupCompiler {
    <#
    .SYNOPSIS
        Locates ISCC.exe (Inno Setup 6). An explicit -Path or LUCENT_ISCC must be valid: a typo never
        falls back to another copy. Otherwise PATH, the uninstall registry (InstallLocation of
        "Inno Setup 6" or "Inno Setup version 6.*"), then the standard folders are searched. The
        result says where it was found, whether that is a standard Inno Setup installation, and the
        SHA-256 of ISCC.exe (recorded in the package manifest).
    #>
    param([string]$Path)
    $standardFolders = @(${env:ProgramFiles(x86)}, $env:ProgramFiles, (Join-Path $env:LOCALAPPDATA 'Programs')) |
        Where-Object { $_ } | ForEach-Object { [System.IO.Path]::GetFullPath((Join-Path $_ 'Inno Setup 6')).TrimEnd('\') + '\' }
    function New-Result([string]$Candidate, [string]$From) {
        $full = [System.IO.Path]::GetFullPath($Candidate)
        $standard = [bool]($standardFolders | Where-Object { $full.StartsWith($_, [System.StringComparison]::OrdinalIgnoreCase) })
        return [pscustomobject]@{ Path = $full; From = $From; Standard = $standard; Sha256 = (Get-Sha256 $full) }
    }
    if ($Path) {
        if (-not (Test-InnoSetupCompiler $Path)) { throw "-Iscc '$Path' is not Inno Setup 6's ISCC.exe." }
        return New-Result $Path '-Iscc'
    }
    if ($env:LUCENT_ISCC) {
        if (-not (Test-InnoSetupCompiler $env:LUCENT_ISCC)) { throw "LUCENT_ISCC '$env:LUCENT_ISCC' is not Inno Setup 6's ISCC.exe." }
        return New-Result $env:LUCENT_ISCC 'LUCENT_ISCC'
    }
    $candidates = New-Object System.Collections.Generic.List[object]
    $onPath = Get-Command 'ISCC.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($onPath) { $candidates.Add([pscustomobject]@{ Path = $onPath.Source; From = 'PATH' }) }
    $uninstallRoots = @(
        'HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall',
        'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall',
        'HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall')
    foreach ($root in $uninstallRoots) {
        foreach ($key in (Get-ChildItem -LiteralPath $root -ErrorAction SilentlyContinue)) {
            $properties = Get-ItemProperty -LiteralPath $key.PSPath -ErrorAction SilentlyContinue
            if (-not $properties) { continue }
            $displayName = $properties.PSObject.Properties['DisplayName']
            $location = $properties.PSObject.Properties['InstallLocation']
            if ($displayName -and $location -and $location.Value -and
                ($key.PSChildName -eq 'Inno Setup 6_is1' -or $displayName.Value -like 'Inno Setup version 6*')) {
                $candidates.Add([pscustomobject]@{ Path = (Join-Path $location.Value 'ISCC.exe'); From = "registry ($($displayName.Value))" })
            }
        }
    }
    foreach ($folder in $standardFolders) { $candidates.Add([pscustomobject]@{ Path = (Join-Path $folder 'ISCC.exe'); From = 'standard folder' }) }
    foreach ($candidate in $candidates) {
        if (Test-InnoSetupCompiler $candidate.Path) { return New-Result $candidate.Path $candidate.From }
    }
    throw 'Inno Setup 6 (ISCC.exe) was not found. Install Inno Setup 6.3 or later, or set LUCENT_ISCC to ISCC.exe.'
}

function Get-InnoSetupLicence {
    <#
    .SYNOPSIS
        The licence an Inno Setup compiler states in its banner (what ISCC prints before it reads the
        script): Line (trimmed) and Kind: 'non-commercial' (a line that says non-commercial use, as
        6.7.3's "Non-commercial use only"), 'commercial' (a line that names commercial use or a
        licensee, and not non-commercial use), or $null when no banner line says either. The whole
        banner is searched, not only the line after "Compiler engine version" (W12 review LIC-6).
    #>
    param([AllowEmptyCollection()][AllowNull()][string[]]$Output = @())
    $banner = New-Object System.Collections.Generic.List[string]
    foreach ($line in @($Output)) {
        if ([string]$line -match '^\s*(Preprocessing|Parsing \[|Reading file|Compiling|Error)') { break }
        $banner.Add([string]$line)
    }
    foreach ($line in $banner) { if ($line -match '(?i)\bnon-?\s?commercial\b') { return [pscustomobject]@{ Line = $line.Trim(); Kind = 'non-commercial' } } }
    foreach ($line in $banner) { if ($line -match '(?i)\bcommercial\b|\blicen[cs]ed to\b') { return [pscustomobject]@{ Line = $line.Trim(); Kind = 'commercial' } } }
    return [pscustomobject]@{ Line = $null; Kind = $null }
}

function Test-InnoSetupLicence {
    <#
    .SYNOPSIS
        Whether the licence a compiler states (Get-InnoSetupLicence) covers the distribution
        installer\license-policy.json records (ADR 0024). A commercial distribution needs a licence the
        banner positively states as commercial: a non-commercial compiler, and one whose licence this
        reader does not recognise (another wording, a translation), are errors, so a change of model
        fails loudly. A non-commercial distribution with an unrecognised licence line is a warning (the
        package manifest then records none). Returns Errors and Warnings.
    #>
    param([Parameter(Mandatory)]$Licence, [Parameter(Mandatory)][bool]$Commercial)
    $errors = New-Object System.Collections.Generic.List[string]
    $warnings = New-Object System.Collections.Generic.List[string]
    if ($Commercial -and $Licence.Kind -ne 'commercial') {
        $errors.Add("installer\license-policy.json records a commercial distribution, but this Inno Setup compiler $(if ($Licence.Line) { "reports '$($Licence.Line)'" } else { 'states no licence that names commercial use' }): build Setup with a commercially licensed Inno Setup whose banner says so (ADR 0024).")
    }
    elseif (-not $Commercial -and -not $Licence.Kind) {
        $warnings.Add('This Inno Setup compiler states no licence that this script recognises: the package manifest records none (toolchain.innoSetupLicence). Check the compiler''s terms by hand before a release (ADR 0024).')
    }
    return [pscustomobject]@{ Errors = $errors.ToArray(); Warnings = $warnings.ToArray() }
}

function Write-InstalledFileList {
    <#
    .SYNOPSIS
        Writes <stage>\Lucent-files.txt: the relative path of every file Setup installs (itself
        included). The next version's Setup deletes exactly the listed files that it no longer ships,
        so an upgrade never touches files of other programs in a shared folder (ADR 0009, decision 8).
        Paths must be ASCII (Inno Setup reads the list in the system code page).
    #>
    param([Parameter(Mandatory)][string]$StageDir, [Parameter(Mandatory)][string]$Version,
          # Files written after the list (the SBOM, which itself lists this file).
          [string[]]$AlsoList = @())
    $listPath = Join-Path $StageDir $script:InstalledFileListName
    $paths = New-Object System.Collections.Generic.List[string]
    $relatives = @(Get-ChildItem -LiteralPath $StageDir -Recurse -File | ForEach-Object { Get-RelativePathText -Root $StageDir -Path $_.FullName }) +
                 @($AlsoList) + @($script:InstalledFileListName)
    foreach ($relative in $relatives) {
        if ($paths -icontains $relative) { continue }
        if ($relative -notmatch '^[\x20-\x7E]+$' -or $relative -match '\.\.|:|^\\|/') { throw "Cannot list '$relative' in $($script:InstalledFileListName): only plain ASCII relative paths are allowed." }
        $paths.Add($relative)
    }
    $sorted = [string[]]$paths.ToArray()
    [Array]::Sort($sorted, [System.StringComparer]::OrdinalIgnoreCase)
    $header = @($script:InstalledFileListHeader,
                "; Lucent $Version. Every file Lucent Setup installed into this folder, relative to it. The next",
                '; version of Lucent Setup deletes the files listed here that it no longer installs; nothing else.')
    # ASCII with CRLF, for Inno Setup's LoadStringsFromFile and Notepad.
    [System.IO.File]::WriteAllText($listPath, (((@($header) + @($sorted)) -join "`r`n") + "`r`n"), (New-Object System.Text.ASCIIEncoding))
    return $sorted.Count
}

function Test-InstalledFileList {
    # Errors when <stage>\Lucent-files.txt does not list exactly the files of the folder.
    param([Parameter(Mandatory)][string]$StageDir)
    $listed = @([System.IO.File]::ReadAllLines((Join-Path $StageDir $script:InstalledFileListName)) | Where-Object { $_ -and -not $_.StartsWith(';') })
    $present = @(Get-ChildItem -LiteralPath $StageDir -Recurse -File | ForEach-Object { Get-RelativePathText -Root $StageDir -Path $_.FullName })
    $differences = @(Compare-Object -ReferenceObject $listed -DifferenceObject $present | ForEach-Object {
        if ($_.SideIndicator -eq '<=') { "listed but missing: $($_.InputObject)" } else { "present but not listed: $($_.InputObject)" } })
    if ($differences.Count -gt 0) { throw "$($script:InstalledFileListName) does not match the folder: $(($differences | Select-Object -First 5) -join '; ')" }
}

# --- Corresponding source (W12, ADR 0024) --------------------------------------------------------

# Every entry of the corresponding-source archive carries this DOS time: 1980-02-01 00:00, a month
# after the DOS epoch so that no time zone conversion moves it before 1980. With sorted entries,
# stored data and no extra fields, the archive's bytes depend only on its content (ADR 0024).
$script:ZipDosDate = [uint16](((1980 - 1980) -shl 9) -bor (2 -shl 5) -bor 1)
$script:ZipDosTime = [uint16]0
$script:ZipTimestamp = New-Object DateTime(1980, 2, 1, 0, 0, 0)
$script:SourcePackageSchema = 'lucent.corresponding-source'

function Initialize-StoredZipType {
    if ('LucentPackaging.StoredZip' -as [type]) { return }
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
namespace LucentPackaging {
    public sealed class StoredZipEntry {
        public string Name;
        public string SourcePath;
        public byte[] Content;
    }
    public static class StoredZip {
        private static readonly uint[] CrcTable = CreateCrcTable();
        private static uint[] CreateCrcTable() {
            uint[] table = new uint[256];
            for (uint n = 0; n < 256; n++) {
                uint c = n;
                for (int k = 0; k < 8; k++) { c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1; }
                table[n] = c;
            }
            return table;
        }
        private static uint UpdateCrc(uint crc, byte[] buffer, int count) {
            for (int i = 0; i < count; i++) { crc = CrcTable[(crc ^ buffer[i]) & 0xFF] ^ (crc >> 8); }
            return crc;
        }
        public static uint Crc32(byte[] data) { return UpdateCrc(0xFFFFFFFFu, data, data.Length) ^ 0xFFFFFFFFu; }
        // Writes the entries in the order given (the caller sorts them): each stored (method 0) with
        // the given DOS time and date, its CRC-32 and sizes in the local header (no data descriptor),
        // no extra fields, comments or file attributes; then the central directory and its end
        // record. No ZIP64: a member or an archive of 4 GiB, or 65,535 members, is refused.
        public static void Write(string path, IList<StoredZipEntry> entries, ushort dosTime, ushort dosDate) {
            if (entries.Count >= 0xFFFF) { throw new InvalidOperationException("Too many entries for a ZIP archive without ZIP64."); }
            byte[] buffer = new byte[1 << 20];
            using (FileStream stream = new FileStream(path, FileMode.CreateNew, FileAccess.ReadWrite, FileShare.None))
            using (MemoryStream central = new MemoryStream()) {
                BinaryWriter writer = new BinaryWriter(stream);
                BinaryWriter centralWriter = new BinaryWriter(central);
                foreach (StoredZipEntry entry in entries) {
                    byte[] name = Encoding.ASCII.GetBytes(entry.Name);
                    long size = entry.Content != null ? entry.Content.LongLength : new FileInfo(entry.SourcePath).Length;
                    long offset = stream.Position;
                    if (size >= 0xFFFFFFFFL || offset >= 0xFFFFFFFFL) { throw new InvalidOperationException(entry.Name + ": the archive would need ZIP64."); }
                    writer.Write(0x04034B50u);
                    writer.Write((ushort)10);   // version needed to extract: 1.0 (stored)
                    writer.Write((ushort)0);    // general purpose flags: none
                    writer.Write((ushort)0);    // method: stored
                    writer.Write(dosTime);
                    writer.Write(dosDate);
                    long crcPosition = stream.Position;
                    writer.Write(0u);           // CRC-32, written once the data is
                    writer.Write((uint)size);
                    writer.Write((uint)size);
                    writer.Write((ushort)name.Length);
                    writer.Write((ushort)0);    // extra field length
                    writer.Write(name);
                    writer.Flush();
                    uint crc = 0xFFFFFFFFu;
                    long copied = 0;
                    if (entry.Content != null) {
                        crc = UpdateCrc(crc, entry.Content, entry.Content.Length);
                        stream.Write(entry.Content, 0, entry.Content.Length);
                        copied = entry.Content.LongLength;
                    } else {
                        using (FileStream input = new FileStream(entry.SourcePath, FileMode.Open, FileAccess.Read, FileShare.Read)) {
                            int read;
                            while ((read = input.Read(buffer, 0, buffer.Length)) > 0) {
                                crc = UpdateCrc(crc, buffer, read);
                                stream.Write(buffer, 0, read);
                                copied += read;
                            }
                        }
                    }
                    if (copied != size) { throw new IOException(entry.Name + ": its source changed while it was written."); }
                    crc ^= 0xFFFFFFFFu;
                    long end = stream.Position;
                    stream.Position = crcPosition;
                    writer.Write(crc);
                    writer.Flush();
                    stream.Position = end;
                    centralWriter.Write(0x02014B50u);
                    centralWriter.Write((ushort)20);   // made by: MS-DOS, ZIP 2.0
                    centralWriter.Write((ushort)10);
                    centralWriter.Write((ushort)0);
                    centralWriter.Write((ushort)0);
                    centralWriter.Write(dosTime);
                    centralWriter.Write(dosDate);
                    centralWriter.Write(crc);
                    centralWriter.Write((uint)size);
                    centralWriter.Write((uint)size);
                    centralWriter.Write((ushort)name.Length);
                    centralWriter.Write((ushort)0);    // extra field length
                    centralWriter.Write((ushort)0);    // comment length
                    centralWriter.Write((ushort)0);    // disk number
                    centralWriter.Write((ushort)0);    // internal attributes
                    centralWriter.Write(0u);           // external attributes
                    centralWriter.Write((uint)offset);
                    centralWriter.Write(name);
                }
                centralWriter.Flush();
                long centralOffset = stream.Position;
                if (centralOffset + central.Length >= 0xFFFFFFFFL) { throw new InvalidOperationException("The archive would need ZIP64."); }
                central.WriteTo(stream);
                writer.Write(0x06054B50u);
                writer.Write((ushort)0);
                writer.Write((ushort)0);
                writer.Write((ushort)entries.Count);
                writer.Write((ushort)entries.Count);
                writer.Write((uint)central.Length);
                writer.Write((uint)centralOffset);
                writer.Write((ushort)0);            // archive comment length
                writer.Flush();
            }
        }
    }
}
'@
}

function New-DeterministicZip {
    <#
    .SYNOPSIS
        Writes a ZIP archive whose bytes depend only on its entries (ADR 0024): sorted by name
        (ordinal), each stored (method 0: no compression, the large members are compressed archives
        already) with the fixed time 1980-02-01 00:00, with no extra fields, comments, file attributes
        or data descriptors, and no folder entries. The same entries give the same bytes on any
        machine. The archive is written beside -Path first and renamed when complete.
    .PARAMETER Entries
        Objects with Name (a relative path with forward slashes, printable ASCII, unique ignoring
        case) and either Content (byte[]) or SourcePath (a file, copied byte for byte).
    #>
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][object[]]$Entries)
    Initialize-StoredZipType
    $byName = @{}
    foreach ($entry in $Entries) {
        $name = [string]$entry.Name
        if ($name -cnotmatch '^[\x20-\x7E]+$' -or $name -match '\\|^/|/$|//|(^|/)\.\.?(/|$)|^ | $|:') {
            throw "'$name' is not a relative path of printable ASCII with forward slashes (archive entry)."
        }
        $key = $name.ToLowerInvariant()
        if ($byName.ContainsKey($key)) { throw "Two archive entries are named '$name' (ignoring case)." }
        $hasContent = ($entry.PSObject.Properties.Name -contains 'Content') -and ($null -ne $entry.Content)
        if (-not $hasContent -and -not (($entry.PSObject.Properties.Name -contains 'SourcePath') -and $entry.SourcePath -and
                                        (Test-Path -LiteralPath $entry.SourcePath -PathType Leaf))) {
            throw "The archive entry '$name' has neither content nor an existing source file."
        }
        $byName[$key] = $entry
    }
    $names = [string[]]@($byName.Values | ForEach-Object { [string]$_.Name })
    [Array]::Sort($names, [System.StringComparer]::Ordinal)
    $list = New-Object 'System.Collections.Generic.List[LucentPackaging.StoredZipEntry]'
    foreach ($name in $names) {
        $entry = $byName[$name.ToLowerInvariant()]
        $zipEntry = New-Object LucentPackaging.StoredZipEntry
        $zipEntry.Name = $name
        if (($entry.PSObject.Properties.Name -contains 'Content') -and ($null -ne $entry.Content)) { $zipEntry.Content = [byte[]]$entry.Content }
        else { $zipEntry.SourcePath = [System.IO.Path]::GetFullPath($entry.SourcePath) }
        $list.Add($zipEntry)
    }
    $partial = "$Path.partial"
    if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial -Force }
    try { [LucentPackaging.StoredZip]::Write($partial, $list, $script:ZipDosTime, $script:ZipDosDate) }
    catch {
        Remove-Item -LiteralPath $partial -Force -ErrorAction SilentlyContinue
        throw
    }
    if (Test-Path -LiteralPath $Path) { Remove-Item -LiteralPath $Path -Force }
    Move-Item -LiteralPath $partial -Destination $Path
}

function ConvertTo-HexText {
    param($Bytes)
    return [System.BitConverter]::ToString($Bytes).Replace('-', '').ToLowerInvariant()
}

function Read-ZipArchiveEntries {
    <#
    .SYNOPSIS
        Reads every entry of a ZIP archive with System.IO.Compression, an implementation independent
        of New-DeterministicZip: in central-directory order, each with Name, Length,
        CompressedLength, LastWriteTime (the DOS time as written), the SHA-256 and SHA-512 of its
        content and, for the names in -Keep, the content itself.
    #>
    param([Parameter(Mandatory)][string]$Path, [AllowEmptyCollection()][string[]]$Keep = @())
    Add-Type -AssemblyName System.IO.Compression
    $result = New-Object System.Collections.Generic.List[object]
    $buffer = New-Object byte[] (1MB)
    $file = [System.IO.File]::OpenRead($Path)
    try {
        $zip = New-Object System.IO.Compression.ZipArchive($file, [System.IO.Compression.ZipArchiveMode]::Read, $true)
        try {
            foreach ($entry in $zip.Entries) {
                $sha256 = [System.Security.Cryptography.SHA256]::Create()
                $sha512 = [System.Security.Cryptography.SHA512]::Create()
                $memory = $null
                if ($Keep -contains $entry.FullName) { $memory = New-Object System.IO.MemoryStream }
                $stream = $entry.Open()
                try {
                    while (($read = $stream.Read($buffer, 0, $buffer.Length)) -gt 0) {
                        [void]$sha256.TransformBlock($buffer, 0, $read, $null, 0)
                        [void]$sha512.TransformBlock($buffer, 0, $read, $null, 0)
                        if ($memory) { $memory.Write($buffer, 0, $read) }
                    }
                    [void]$sha256.TransformFinalBlock($buffer, 0, 0)
                    [void]$sha512.TransformFinalBlock($buffer, 0, 0)
                    # Assigned, not a subexpression's value: an empty entry's content stays an empty array.
                    $content = $null
                    if ($memory) { $content = $memory.ToArray() }
                    $result.Add([pscustomobject]@{
                        Name = $entry.FullName; Length = $entry.Length; CompressedLength = $entry.CompressedLength
                        LastWriteTime = $entry.LastWriteTime.DateTime
                        Sha256 = (ConvertTo-HexText $sha256.Hash); Sha512 = (ConvertTo-HexText $sha512.Hash)
                        Content = $content })
                }
                finally {
                    $stream.Dispose()
                    $sha256.Dispose()
                    $sha512.Dispose()
                }
            }
        }
        finally { $zip.Dispose() }
    }
    finally { $file.Dispose() }
    return $result.ToArray()
}

function Get-LicenceKind {
    <#
    .SYNOPSIS
        What an SPDX licence expression leaves Lucent to use: 'strong-copyleft' (GPL or AGPL without an
        exception: ADR 0003 needs a new ADR before such code ships), 'copyleft' (a licence whose terms
        reach the source of the covered files in object-form distribution: LGPL, MPL, EPL, CDDL, EUPL,
        CC BY-SA, a GPL with an exception, and others), 'permissive', or 'unknown' (a LicenseRef- or
        DocumentRef- licence, such as a vendor's commercial terms Lucent does not hold, or an identifier
        this list does not know; also NOASSERTION and an empty expression).
    .DESCRIPTION
        OR is a choice: Lucent takes the best alternative it can use, permissive before copyleft before
        strong copyleft; an unknown alternative is one it cannot take ("LicenseRef-Qt-Commercial OR
        LGPL-3.0-only" leaves LGPL-3.0-only: W12 review LIC-4), so OR is unknown only when every
        alternative is. AND needs every operand: strong copyleft before copyleft before unknown before
        permissive. A licence's exception (WITH) makes a GPL a copyleft licence of the library kind and
        leaves any other licence as it is. An election recorded in installer\license-policy.json
        ("concluded") is a single licence and needs no choice.
    #>
    param([string]$Expression)
    if (-not $Expression -or $Expression.Trim() -in 'NOASSERTION', 'NONE') { return 'unknown' }
    $state = @{ Expression = $Expression; Position = 0
                Tokens = @([regex]::Matches($Expression, '\(|\)|[^\s()]+') | ForEach-Object { $_.Value }) }
    $rank = Read-LicenceDisjunction $state
    if ($state.Position -ne $state.Tokens.Count) { throw "Unexpected '$($state.Tokens[$state.Position])' in the licence expression '$Expression'." }
    return $script:LicenceKinds[$rank]
}

function Test-CopyleftLicence {
    <#
    .SYNOPSIS
        True when an SPDX licence expression leaves no choice but a copyleft licence, strong or not
        (Get-LicenceKind): "FTL OR GPL-2.0-or-later" is not (FreeType is used under the FTL);
        "LicenseRef-Qt-Commercial OR LGPL-3.0-only" is (Lucent holds no commercial licence).
    #>
    param([string]$Expression)
    return ((Get-LicenceKind $Expression) -in 'copyleft', 'strong-copyleft')
}

# Licence identifiers whose terms reach the covered source in object-form distribution, those of them
# that are strong copyleft (GPL and AGPL, without an exception), and the permissive ones Lucent's
# dependencies use (an identifier in neither list is unknown, so it is never chosen over a copyleft
# alternative unreviewed).
$script:CopyleftLicencePattern = '^(?:A|L)?GPL(?:-|$)|^(?:MPL|EPL|CDDL|EUPL|OSL|CPL|IPL|SPL|MS-RL|RPL|RPSL|QPL|APSL|NPL|CECILL|CC-BY-SA|ODbL|Sleepycat)(?:-|$)'
$script:StrongCopyleftLicencePattern = '^A?GPL(?:-|$)'
$script:PermissiveLicences = @('0BSD', 'AML', 'AML-glslang', 'Apache-1.1', 'Apache-2.0', 'blessing', 'BSD-1-Clause', 'BSD-2-Clause', 'BSD-2-Clause-Patent',
    'BSD-3-Clause', 'BSD-3-Clause-Clear', 'BSD-4-Clause', 'BSD-Source-Code', 'BSL-1.0', 'bzip2-1.0.6', 'CC0-1.0', 'curl', 'FTL', 'HPND', 'HPND-sell-variant',
    'ICU', 'IJG', 'Imlib2', 'Info-ZIP', 'ISC', 'Libpng', 'libpng-2.0', 'libtiff', 'MIT', 'MIT-0', 'MIT-CMU', 'MIT-Modern-Variant', 'NCSA', 'NTP', 'OpenSSL',
    'PostgreSQL', 'PSF-2.0', 'Python-2.0', 'SSLeay-standalone', 'Unicode-3.0', 'Unicode-DFS-2015', 'Unicode-DFS-2016', 'Unlicense', 'W3C', 'W3C-20150513',
    'X11', 'Zlib', 'zlib-acknowledgement')
# Get-LicenceKind's ranks: OR takes the highest rank above 0, AND the lowest.
$script:LicenceKinds = @{ 0 = 'unknown'; 1 = 'strong-copyleft'; 2 = 'copyleft'; 3 = 'permissive' }

# Get-LicenceKind's recursive descent over an SPDX expression ($State: Tokens, Position), in ranks:
# 3 permissive, 2 copyleft, 1 strong copyleft, 0 unknown.
function Read-LicenceTerm($State) {
    if ($State.Position -ge $State.Tokens.Count) { throw "Incomplete licence expression '$($State.Expression)'." }
    $token = $State.Tokens[$State.Position]
    $State.Position++
    if ($token -eq '(') {
        $value = Read-LicenceDisjunction $State
        if ($State.Position -ge $State.Tokens.Count -or $State.Tokens[$State.Position] -ne ')') { throw "Unbalanced parentheses in the licence expression '$($State.Expression)'." }
        $State.Position++
        return $value
    }
    if ($token -in 'AND', 'OR', 'WITH', ')') { throw "Unexpected '$token' in the licence expression '$($State.Expression)'." }
    $exception = $false
    if ($State.Position -lt $State.Tokens.Count -and $State.Tokens[$State.Position] -eq 'WITH') { $State.Position += 2; $exception = $true }
    $id = $token.TrimEnd('+')
    if ($id -match '^(LicenseRef|DocumentRef)-') { return 0 }
    if ($id -match $script:StrongCopyleftLicencePattern -and -not $exception) { return 1 }
    if ($id -match $script:CopyleftLicencePattern) { return 2 }
    if ($script:PermissiveLicences -contains $id) { return 3 }
    return 0
}

function Read-LicenceConjunction($State) {
    # AND: every operand applies: the most demanding wins (strong copyleft, copyleft, unknown, permissive).
    $order = @{ 1 = 0; 2 = 1; 0 = 2; 3 = 3 }
    $value = Read-LicenceTerm $State
    while ($State.Position -lt $State.Tokens.Count -and $State.Tokens[$State.Position] -eq 'AND') {
        $State.Position++
        $operand = Read-LicenceTerm $State
        if ($order[$operand] -lt $order[$value]) { $value = $operand }
    }
    return $value
}

function Read-LicenceDisjunction($State) {
    # OR: Lucent takes the best alternative it can use; an unknown one it cannot take.
    $value = Read-LicenceConjunction $State
    while ($State.Position -lt $State.Tokens.Count -and $State.Tokens[$State.Position] -eq 'OR') {
        $State.Position++
        $operand = Read-LicenceConjunction $State
        if ($operand -gt $value) { $value = $operand }
    }
    return $value
}

function Get-SourceProvisionPlan {
    <#
    .SYNOPSIS
        The shipped components whose corresponding source goes into the corresponding-source package:
        those installer\license-policy.json marks "lgpl" or "correspondingSource" (ADR 0024). A
        component whose concluded licence, a library linked statically from it, or third-party code
        that Qt's SBOM records inside it leaves no choice but a copyleft licence (Test-CopyleftLicence)
        must be marked: a source provision is reviewed, never assumed, so an unmarked one is an error.
    .DESCRIPTION
        A source provision is not enough for everything copyleft (W12 review LIC-4; ADR 0003 decisions
        4 and 6):
        - Strong copyleft (GPL or AGPL without an exception, Get-LicenceKind) is an error unless the
          port's policy entry names the ADR that accepted it (ports.<port>.strongCopyleftAdr): GPL code
          does not ship in Lucent's MIT distribution on a source provision alone.
        - Copyleft code compiled into Lucent's programs (a header-only port, Linkage "compiled-in") is an
          error, as a copyleft library linked statically is (New-LicenseBundle): LGPL and GPL code stays
          in replaceable DLLs. A file-level copyleft (MPL, EPL, CDDL) passes only with a review recorded
          in ports.<port>.fileLevelCopyleftReview.
    .OUTPUTS
        Components (the components to include, by port, in ordinal order) and Errors.
    #>
    param([Parameter(Mandatory)][AllowEmptyCollection()][object[]]$Components, [Parameter(Mandatory)]$Policy)
    $included = New-Object System.Collections.Generic.List[object]
    $errors = New-Object System.Collections.Generic.List[string]
    foreach ($component in @(ConvertTo-OrdinalOrder -Items $Components -Property Port)) {
        $entry = Get-PolicyEntry -Policy $Policy -Port $component.Port
        $names = if ($entry) { @($entry.PSObject.Properties | ForEach-Object Name) } else { @() }
        $marked = [bool]$component.Lgpl
        if ($names -contains 'correspondingSource') { $marked = $marked -or [bool]$entry.correspondingSource }
        $adr = if ($names -contains 'strongCopyleftAdr') { [string]$entry.strongCopyleftAdr } else { '' }
        $reasons = New-Object System.Collections.Generic.List[string]
        $strong = New-Object System.Collections.Generic.List[string]
        $licences = New-Object System.Collections.Generic.List[object]
        $licences.Add([pscustomobject]@{ What = "its licence $($component.LicenseConcluded)"; Licence = $component.LicenseConcluded })
        foreach ($static in @($component.StaticLibraries | Where-Object { $_ })) {
            $licences.Add([pscustomobject]@{ What = "$($static.Name) linked from it ($($static.LicenseConcluded))"; Licence = $static.LicenseConcluded })
        }
        if (($component.PSObject.Properties.Name -contains 'QtSbom') -and $component.QtSbom) {
            foreach ($attribution in $component.QtSbom.Attributions) {
                $licences.Add([pscustomobject]@{ What = "$($attribution.Name) inside it ($($attribution.License))"; Licence = $attribution.License })
            }
        }
        foreach ($licence in $licences) {
            $kind = Get-LicenceKind $licence.Licence
            if ($kind -in 'copyleft', 'strong-copyleft') { $reasons.Add($licence.What) }
            if ($kind -eq 'strong-copyleft') { $strong.Add($licence.What) }
        }
        $refusedStrong = $strong.Count -gt 0 -and $adr -notmatch '^ADR \d{4}\b'
        if ($refusedStrong) {
            $errors.Add("Port $($component.Port) $($component.Version) leaves no choice but a GPL or AGPL licence without an exception ($($strong -join '; ')). GPL code does not ship in Lucent's MIT distribution on a source provision alone: ADR 0003 (decisions 4 and 6) needs a new ADR first, which ports.$($component.Port).strongCopyleftAdr then names; otherwise remove the dependency or the feature that brings it.")
        }
        $linkage = if ($component.PSObject.Properties.Name -contains 'Linkage') { [string]$component.Linkage } else { '' }
        if ($linkage -eq 'compiled-in' -and $reasons.Count -gt 0) {
            # A file-level copyleft only: every copyleft licence named is MPL, EPL or CDDL.
            $copyleftIds = @(Get-LicenseIds $component.LicenseConcluded | Where-Object { $_ -match $script:CopyleftLicencePattern })
            $fileLevel = $copyleftIds.Count -gt 0 -and @($copyleftIds | Where-Object { $_ -notmatch '^(MPL|EPL|CDDL)-' }).Count -eq 0
            $reviewed = ($names -contains 'fileLevelCopyleftReview') -and [string]$entry.fileLevelCopyleftReview
            if (-not ($fileLevel -and $reviewed -and $strong.Count -eq 0)) {
                $errors.Add("Port $($component.Port) $($component.Version) is compiled into Lucent's programs (it installs no DLL) under a copyleft licence ($($reasons -join '; ')). LGPL and GPL code must stay in replaceable DLLs (ADR 0003); a file-level copyleft (MPL, EPL, CDDL) needs a review recorded in ports.$($component.Port).fileLevelCopyleftReview.")
            }
        }
        if ($reasons.Count -gt 0 -and -not $marked -and -not $refusedStrong) {
            $errors.Add("Port $($component.Port) $($component.Version) ships under a copyleft licence ($($reasons -join '; ')), but installer\license-policy.json does not provide its corresponding source: review it and set ports.$($component.Port).lgpl (an LGPL library in a replaceable DLL) or ports.$($component.Port).correspondingSource (another library-level or file-level copyleft licence) (ADR 0024).")
        }
        if ($marked) { $included.Add($component) }
    }
    return [pscustomobject]@{ Components = $included.ToArray(); Errors = $errors.ToArray() }
}

function Get-SourceProvisionExpectation {
    <#
    .SYNOPSIS
        What the corresponding-source package must hold for one component, from vcpkg's record of its
        build (the port's SPDX document): every source archive, by the SHA-512 vcpkg verified, and
        every recipe file, by its path in the port folder and the SHA-256 vcpkg hashed.
    #>
    param([Parameter(Mandatory)]$Component)
    $archives = @($Component.Sources | ForEach-Object {
        [pscustomobject]@{ Name = $_.Name; Location = $_.Location; Sha512 = ([string]$_.Sha512).ToLowerInvariant() } })
    $recipe = New-Object System.Collections.Generic.List[object]
    $files = @()
    if ($Component.Spdx -and ($Component.Spdx.PSObject.Properties.Name -contains 'files')) { $files = @($Component.Spdx.files) }
    foreach ($file in $files) {
        if ($file.SPDXID -notlike 'SPDXRef-port-file-*') { continue }
        $sha = (@($file.checksums) | Where-Object { $_.algorithm -eq 'SHA256' } | Select-Object -First 1).checksumValue
        $recipe.Add([pscustomobject]@{ Path = ($file.fileName -replace '^\./', ''); Sha256 = ([string]$sha).ToLowerInvariant() })
    }
    return [pscustomobject]@{
        Port = $Component.Port; Version = $Component.Version; Licence = $Component.LicenseConcluded
        Archives = $archives; RecipeFiles = @(ConvertTo-OrdinalOrder -Items $recipe.ToArray() -Property Path) }
}

function ConvertFrom-SourcePackageRecord {
    <#
    .SYNOPSIS
        The expectations (Get-SourceProvisionExpectation's form) recorded in a package manifest
        (schema 5: correspondingSource.components), so a published archive can be checked against
        its package manifest without the build tree (installer\tests\Test-SourcePackage.ps1).
    #>
    param([Parameter(Mandatory)]$Record)
    return @(@($Record.components) | ForEach-Object {
        $port = $_.port
        [pscustomobject]@{
            Port = $port; Version = $_.version; Licence = $_.licence
            Archives = @(@($_.archives) | ForEach-Object { [pscustomobject]@{ Name = $_.file; Location = $_.url; Sha512 = ([string]$_.sha512).ToLowerInvariant() } })
            RecipeFiles = @(@($_.recipe.files) | ForEach-Object {
                [pscustomobject]@{ Path = ([string]$_.path).Substring("ports/$port/".Length); Sha256 = ([string]$_.sha256).ToLowerInvariant() } }) } })
}

function Test-CorrespondingSourcePackage {
    <#
    .SYNOPSIS
        Checks a corresponding-source archive, reading it with System.IO.Compression (not the writer's
        code), against what it must hold (ADR 0024).
    .DESCRIPTION
        - Form: every entry inside the folder Lucent-<version>-corresponding-source/, no folder
          entries, sorted by name (ordinal), unique ignoring case, stored, with the time 1980-02-01
          00:00; README.txt, MANIFEST.json and SHA256SUMS.txt present.
        - SHA256SUMS.txt lists every other entry exactly once, with the SHA-256 of its content.
        - MANIFEST.json is a lucent.corresponding-source version 1 of this version.
        - Every expected component (Get-SourceProvisionExpectation, ConvertFrom-SourcePackageRecord)
          is listed in MANIFEST.json, and the archive holds each of its source archives in sources/
          (an entry whose SHA-512 is the one vcpkg verified), each recipe file in ports/<port>/ with
          the SHA-256 vcpkg recorded, and build/<port>/vcpkg_abi_info.txt.
    .OUTPUTS
        Errors, Size, Sha256, Entries (Name, Length, Sha256) and Manifest (the parsed MANIFEST.json).
    #>
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][string]$Version,
          [Parameter(Mandatory)][AllowEmptyCollection()][object[]]$Expected)
    $errors = New-Object System.Collections.Generic.List[string]
    $root = "Lucent-$Version-corresponding-source/"
    $readme = $root + 'README.txt'
    $manifestName = $root + 'MANIFEST.json'
    $sumsName = $root + 'SHA256SUMS.txt'
    $entries = @()
    try { $entries = @(Read-ZipArchiveEntries -Path $Path -Keep @($readme, $manifestName, $sumsName)) }
    catch { $errors.Add("$Path is not a readable ZIP archive: $($_.Exception.Message)") }
    if ($entries.Count -eq 0 -and $errors.Count -eq 0) { $errors.Add("$Path holds no entry.") }

    $byName = @{}
    $previous = $null
    foreach ($entry in $entries) {
        if (-not $entry.Name.StartsWith($root, [System.StringComparison]::Ordinal)) { $errors.Add("$($entry.Name) is not inside the folder $root") }
        if ($entry.Name.EndsWith('/')) { $errors.Add("$($entry.Name) is a folder entry; the archive lists files only") }
        $key = $entry.Name.ToLowerInvariant()
        if ($byName.ContainsKey($key)) { $errors.Add("$($entry.Name) is in the archive twice (ignoring case)") }
        $byName[$key] = $entry
        if ($null -ne $previous -and [string]::CompareOrdinal($previous, $entry.Name) -ge 0) { $errors.Add("$($entry.Name) follows $($previous): the entries are not sorted") }
        $previous = $entry.Name
        if ($entry.CompressedLength -ne $entry.Length) { $errors.Add("$($entry.Name) is compressed; every entry is stored") }
        if ($entry.LastWriteTime -ne $script:ZipTimestamp) { $errors.Add("$($entry.Name) has the time $($entry.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss')), not 1980-02-01 00:00:00") }
    }
    foreach ($name in @($readme, $manifestName, $sumsName)) {
        if ($entries.Count -gt 0 -and -not $byName.ContainsKey($name.ToLowerInvariant())) { $errors.Add("$name is missing") }
    }
    $utf8 = New-Object System.Text.UTF8Encoding($false)

    # SHA256SUMS.txt: every other entry once, with its content's SHA-256.
    if ($byName.ContainsKey($sumsName.ToLowerInvariant())) {
        $listed = @{}
        foreach ($line in ($utf8.GetString($byName[$sumsName.ToLowerInvariant()].Content) -split "`r?`n")) {
            if (-not $line) { continue }
            if ($line -cnotmatch '^([0-9a-f]{64}) \*(\S.*)$') { $errors.Add("SHA256SUMS.txt: unexpected line '$line'"); continue }
            if ($listed.ContainsKey($Matches[2].ToLowerInvariant())) { $errors.Add("SHA256SUMS.txt lists $($Matches[2]) twice") }
            $listed[$Matches[2].ToLowerInvariant()] = $Matches[1]
        }
        foreach ($entry in $entries) {
            if ($entry.Name -eq $sumsName -or -not $entry.Name.StartsWith($root, [System.StringComparison]::Ordinal)) { continue }
            $relative = $entry.Name.Substring($root.Length).ToLowerInvariant()
            if (-not $listed.ContainsKey($relative)) { $errors.Add("SHA256SUMS.txt does not list $($entry.Name.Substring($root.Length))") }
            elseif ($listed[$relative] -ne $entry.Sha256) { $errors.Add("$($entry.Name.Substring($root.Length)) has SHA-256 $($entry.Sha256), but SHA256SUMS.txt lists $($listed[$relative])") }
        }
        foreach ($relative in @(ConvertTo-OrdinalOrder -Items @($listed.Keys))) {
            if (-not $byName.ContainsKey(($root + $relative).ToLowerInvariant())) { $errors.Add("SHA256SUMS.txt lists $relative, which the archive does not hold") }
        }
    }

    # MANIFEST.json: a JSON object of this schema and version (null, an array, a number, false or a string
    # is valid JSON but no manifest; W12 review LIC-3). Read strict-mode safe: {} has no members.
    $manifest = $null
    $listedPorts = @{}
    if ($byName.ContainsKey($manifestName.ToLowerInvariant())) {
        $parsed = $false
        try { $manifest = $utf8.GetString($byName[$manifestName.ToLowerInvariant()].Content) | ConvertFrom-Json; $parsed = $true }
        catch { $errors.Add("MANIFEST.json is not JSON: $($_.Exception.Message)") }
        if ($parsed -and $manifest -isnot [System.Management.Automation.PSCustomObject]) {
            $errors.Add("MANIFEST.json is not a JSON object ($(if ($null -eq $manifest) { 'null or empty' } else { "a $($manifest.GetType().Name)" }))")
            $manifest = $null
        }
    }
    if ($manifest) {
        $names = @($manifest.PSObject.Properties | ForEach-Object Name)
        if ($names -notcontains 'schema' -or $manifest.schema -cne $script:SourcePackageSchema -or $names -notcontains 'schemaVersion' -or $manifest.schemaVersion -ne 1) {
            $errors.Add("MANIFEST.json is not a $($script:SourcePackageSchema) version 1")
        }
        elseif ($names -notcontains 'productVersion' -or $manifest.productVersion -cne $Version) {
            $errors.Add("MANIFEST.json describes Lucent $(if ($names -contains 'productVersion') { $manifest.productVersion } else { '(no version)' }), not $Version")
        }
        if ($names -contains 'components') {
            foreach ($component in @($manifest.components)) {
                if ($component -is [System.Management.Automation.PSCustomObject] -and $component.PSObject.Properties['port']) { $listedPorts[[string]$component.port] = $component }
            }
        }
    }
    # The expected components: their source archives, recipe files and build records are checked
    # whatever MANIFEST.json holds; only "listed in MANIFEST.json" needs it.
    $bySha512 = @{}
    foreach ($entry in $entries) { if ($entry.Name.StartsWith($root + 'sources/', [System.StringComparison]::Ordinal)) { $bySha512[$entry.Sha512] = $entry } }
    foreach ($component in @(ConvertTo-OrdinalOrder -Items $Expected -Property Port)) {
        $what = "$($component.Port) $($component.Version) ($($component.Licence))"
        if (-not $listedPorts.ContainsKey([string]$component.Port)) { $errors.Add("$what needs its corresponding source, but MANIFEST.json does not list it") }
        if (@($component.Archives).Count -eq 0) { $errors.Add("$($what): no source archive is known (vcpkg recorded none)") }
        foreach ($archive in @($component.Archives)) {
            if (-not $bySha512.ContainsKey($archive.Sha512)) {
                $errors.Add("$($what): its source archive $($archive.Name) (SHA-512 $($archive.Sha512)) is not in sources/")
            }
        }
        if (@($component.RecipeFiles).Count -eq 0) { $errors.Add("$($what): no recipe file is known (vcpkg recorded none)") }
        foreach ($file in @($component.RecipeFiles)) {
            $name = "$($root)ports/$($component.Port)/$($file.Path)"
            if (-not $byName.ContainsKey($name.ToLowerInvariant())) { $errors.Add("$($what): its recipe file ports/$($component.Port)/$($file.Path) is missing") }
            elseif ($byName[$name.ToLowerInvariant()].Sha256 -ne $file.Sha256) { $errors.Add("$($what): ports/$($component.Port)/$($file.Path) is not the file vcpkg built it with") }
        }
        if (-not $byName.ContainsKey(("$($root)build/$($component.Port)/vcpkg_abi_info.txt").ToLowerInvariant())) {
            $errors.Add("$($what): build/$($component.Port)/vcpkg_abi_info.txt is missing")
        }
    }
    $size = $null
    $sha256 = $null
    if (Test-Path -LiteralPath $Path -PathType Leaf) { $size = (Get-Item -LiteralPath $Path).Length; $sha256 = Get-Sha256 $Path }
    return [pscustomobject]@{
        Errors = $errors.ToArray(); Size = $size; Sha256 = $sha256; Manifest = $manifest
        Entries = @($entries | ForEach-Object { [pscustomobject]@{ Name = $_.Name; Length = $_.Length; Sha256 = $_.Sha256 } }) }
}

function Get-GitBlobId {
    # The id git gives a file's content (SHA-1 of "blob <size>\0" and the bytes).
    param($Bytes)
    $header = [System.Text.Encoding]::ASCII.GetBytes("blob $($Bytes.Length)`0")
    $sha1 = [System.Security.Cryptography.SHA1]::Create()
    try {
        [void]$sha1.TransformBlock($header, 0, $header.Length, $null, 0)
        [void]$sha1.TransformFinalBlock($Bytes, 0, $Bytes.Length)
        return ConvertTo-HexText $sha1.Hash
    }
    finally { $sha1.Dispose() }
}

function Compare-OverlayPort {
    <#
    .SYNOPSIS
        Compares an overlay port of the repository with the vcpkg registry's port of that name at the
        baseline commit, file by file through git's blob ids: unchanged, line endings only, changed,
        added or removed. Lucent's changes to a recipe are what is not unchanged.
    #>
    param([Parameter(Mandatory)][string]$OverlayPath, [Parameter(Mandatory)][string]$Port,
          [Parameter(Mandatory)][string]$VcpkgRoot, [Parameter(Mandatory)][string]$Baseline)
    $registry = @{}
    $listing = Invoke-NativeCommand -FilePath 'git' -Arguments @('-C', $VcpkgRoot, 'ls-tree', '-r', "$($Baseline):ports/$Port")
    $inRegistry = ($listing.ExitCode -eq 0)
    if ($inRegistry) {
        foreach ($line in $listing.Output) { if ($line -match '^\d+ blob ([0-9a-f]{40})\t(.+)$') { $registry[$Matches[2]] = $Matches[1] } }
    }
    $tree = $null
    if ($inRegistry) {
        $probe = Invoke-NativeCommand -FilePath 'git' -Arguments @('-C', $VcpkgRoot, 'rev-parse', "$($Baseline):ports/$Port")
        if ($probe.ExitCode -eq 0) { $tree = $probe.Output[0].Trim() }
    }
    $files = New-Object System.Collections.Generic.List[object]
    $seen = @{}
    foreach ($file in @(ConvertTo-OrdinalOrder -Items @(Get-ChildItem -LiteralPath $OverlayPath -Recurse -File) -Property FullName)) {
        $relative = (Get-RelativePathText -Root $OverlayPath -Path $file.FullName).Replace('\', '/')
        $seen[$relative] = $true
        $status = 'added'
        if ($registry.ContainsKey($relative)) {
            $bytes = [System.IO.File]::ReadAllBytes($file.FullName)
            if ((Get-GitBlobId $bytes) -eq $registry[$relative]) { $status = 'unchanged' }
            else {
                $text = $script:Latin1.GetString($bytes)
                $other = if ($text.Contains("`r`n")) { $text.Replace("`r`n", "`n") } else { $text.Replace("`n", "`r`n") }
                $status = if ((Get-GitBlobId $script:Latin1.GetBytes($other)) -eq $registry[$relative]) { 'line endings' } else { 'changed' }
            }
        }
        $files.Add([pscustomobject]@{ File = $relative; Status = $status })
    }
    foreach ($relative in @(ConvertTo-OrdinalOrder -Items @($registry.Keys))) {
        if (-not $seen.ContainsKey($relative)) { $files.Add([pscustomobject]@{ File = $relative; Status = 'removed' }) }
    }
    return [pscustomobject]@{ InRegistry = $inRegistry; Tree = $tree; Files = @(ConvertTo-OrdinalOrder -Items $files.ToArray() -Property File) }
}

function Find-SourcePin {
    <#
    .SYNOPSIS
        Where a recipe pins a source archive's SHA-512: the files of Lucent's overlay port that
        contain it (-OverlayPath), or the files of the registry's port at the baseline commit that do
        (git grep in the vcpkg checkout). Returns their paths ("ports/<port>/<file>").
    #>
    param([Parameter(Mandatory)][string]$Sha512, [Parameter(Mandatory)][string]$Port, [string]$OverlayPath,
          [Parameter(Mandatory)][string]$VcpkgRoot, [Parameter(Mandatory)][string]$Baseline)
    if ($OverlayPath) {
        return @(ConvertTo-OrdinalOrder -Items @(Get-ChildItem -LiteralPath $OverlayPath -Recurse -File) -Property FullName | Where-Object {
                [System.IO.File]::ReadAllText($_.FullName).IndexOf($Sha512, [System.StringComparison]::OrdinalIgnoreCase) -ge 0 } |
            ForEach-Object { "ports/$Port/" + (Get-RelativePathText -Root $OverlayPath -Path $_.FullName).Replace('\', '/') })
    }
    $grep = Invoke-NativeCommand -FilePath 'git' -Arguments @('-C', $VcpkgRoot, 'grep', '-l', '-i', '-F', '-e', $Sha512, $Baseline, '--', "ports/$Port")
    if ($grep.ExitCode -ne 0) { return @() }
    $prefix = "$($Baseline):"
    return @(ConvertTo-OrdinalOrder -Items @($grep.Output | ForEach-Object { $_.Trim() } | Where-Object { $_.StartsWith($prefix) } | ForEach-Object { $_.Substring($prefix.Length) }))
}

function Read-VcpkgAbiInfo {
    # vcpkg_abi_info.txt: one "<key> <value>" per line (the inputs of a port's ABI hash).
    param([Parameter(Mandatory)][string]$Path)
    $values = [ordered]@{}
    foreach ($line in [System.IO.File]::ReadAllLines($Path)) {
        $space = $line.IndexOf(' ')
        if ($space -lt 1) { continue }
        $values[$line.Substring(0, $space)] = $line.Substring($space + 1)
    }
    return $values
}

function Get-RecipeCopy {
    # The files of a port's recipe folder exactly as vcpkg built with them: each file vcpkg recorded
    # (path and SHA-256, Get-SourceProvisionExpectation) must be in the folder with that SHA-256, and
    # the folder must hold nothing else. Problems are added to -Errors.
    param([Parameter(Mandatory)][string]$Port, [Parameter(Mandatory)][string]$Folder, [Parameter(Mandatory)][string]$Describe,
          [Parameter(Mandatory)][AllowEmptyCollection()][object[]]$RecipeFiles, [Parameter(Mandatory)][AllowEmptyCollection()][System.Collections.Generic.List[string]]$Errors)
    $copy = New-Object System.Collections.Generic.List[object]
    if ($RecipeFiles.Count -eq 0) { $Errors.Add("$($Port): vcpkg recorded no recipe file (share\$Port\vcpkg.spdx.json)."); return $copy.ToArray() }
    if (-not (Test-Path -LiteralPath $Folder -PathType Container)) { $Errors.Add("$($Port): the recipe folder $Describe is missing."); return $copy.ToArray() }
    $recorded = @{}
    foreach ($file in $RecipeFiles) {
        $recorded[$file.Path.ToLowerInvariant()] = $true
        $path = Join-Path $Folder (ConvertTo-NativePath $file.Path)
        if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { $Errors.Add("$($Port): $($file.Path), which vcpkg built it with, is missing from $Describe."); continue }
        $sha = Get-Sha256 $path
        if ($sha -ne $file.Sha256) {
            $Errors.Add("$($Port): $($file.Path) in $Describe is not the file vcpkg built it with (SHA-256 $sha; vcpkg recorded $($file.Sha256)). Rebuild the port, or restore its recipe.")
            continue
        }
        $copy.Add([pscustomobject]@{ Name = "ports/$Port/$($file.Path)"; SourcePath = $path; Sha256 = $sha })
    }
    foreach ($present in @(ConvertTo-OrdinalOrder -Items @(Get-ChildItem -LiteralPath $Folder -Recurse -File) -Property FullName)) {
        $relative = (Get-RelativePathText -Root $Folder -Path $present.FullName).Replace('\', '/')
        if (-not $recorded.ContainsKey($relative.ToLowerInvariant())) { $Errors.Add("$($Port): $relative is in $Describe, but vcpkg did not build with it. Rebuild the port.") }
    }
    return $copy.ToArray()
}

function Format-SourceDownload {
    # Where a source archive can be downloaded: vcpkg's SPDX document names a GitHub source as
    # git+https://github.com/<owner>/<repo>@<ref>, which vcpkg_from_github fetches as GitHub's
    # .tar.gz archive of that reference; any other location is a download URL already.
    param([string]$Location)
    if ($Location -match '^git\+(https://github\.com/[^/@]+/[^/@]+)@([^@\s]+)$') { return "$($Matches[1])/archive/$($Matches[2]).tar.gz" }
    return $Location
}

function Format-IndentedText {
    # Word-wraps $Text at about 100 columns after $Prefix, continuation lines aligned under its end.
    param([Parameter(Mandatory)][string]$Prefix, [Parameter(Mandatory)][string]$Text, [int]$Width = 100)
    $lines = @((Format-Paragraph -Text $Text -Width ($Width - $Prefix.Length)) -split "`n")
    $indent = ' ' * $Prefix.Length
    return @(for ($i = 0; $i -lt $lines.Count; $i++) { $(if ($i -eq 0) { $Prefix } else { $indent }) + $lines[$i] })
}

function Get-PatchedFiles {
    # The files a unified diff changes, as vcpkg applies it (git apply, -p1): the path of every "+++"
    # line (the "---" line's for a deleted file) without its first component (a/, b/ or the source
    # folder) and time stamp.
    param([Parameter(Mandatory)][AllowEmptyString()][string]$Text)
    $files = New-Object System.Collections.Generic.List[string]
    $previous = $null
    foreach ($line in ($Text -split "`r?`n")) {
        if ($line.StartsWith('--- ')) { $previous = $line.Substring(4); continue }
        if (-not $line.StartsWith('+++ ')) { continue }
        $path = $line.Substring(4)
        if ($path -match '^/dev/null') { $path = $previous }
        if (-not $path) { continue }
        $path = ($path -split "`t")[0].Trim().Trim('"')
        $slash = $path.IndexOf('/')
        if ($slash -ge 0) { $path = $path.Substring($slash + 1) }
        if ($path -and -not $files.Contains($path)) { $files.Add($path) }
    }
    return $files.ToArray()
}

function Get-RecipeModifications {
    <#
    .SYNOPSIS
        What a library's recipe changes in its upstream source (W12 review LIC-1: LGPL-2.1 section 2
        and GPL-3.0 section 5(a) ask for a notice of the changed files and the date): every patch file
        (*.patch, *.diff) of the recipe as built that its portfile.cmake names (a few apply only on
        other platforms or with features the build does not use; the portfile says when: listing them
        errs on the side of naming more), with the files it changes (Get-PatchedFiles), who made it
        and when. A patch Lucent's overlay port added or changed
        (Compare-OverlayPort) is the Lucent project's, dated as its own text states ("Changed by the
        Lucent project, <date>"); every other patch is vcpkg's, as in its registry at the baseline
        commit, dated by that commit.
    .PARAMETER RecipeFiles
        The recipe as built (Get-RecipeCopy): Name ("ports/<port>/<file>") and SourcePath.
    .PARAMETER Comparison
        Compare-OverlayPort's result, for a port Lucent overlays.
    .OUTPUTS
        Ordered dictionaries: patch (its path in the archive), by ("vcpkg" or "the Lucent project"),
        source, date (YYYY-MM-DD, or null when a Lucent patch states none), changes (the files).
    #>
    param([Parameter(Mandatory)][string]$Port, [AllowEmptyCollection()][object[]]$RecipeFiles = @(), $Comparison,
          [Parameter(Mandatory)][string]$Baseline, [string]$BaselineDate)
    $prefix = "ports/$Port/"
    $portfile = @($RecipeFiles | Where-Object { $_.Name -ceq "$($prefix)portfile.cmake" }) | Select-Object -First 1
    $portfileText = if ($portfile) { [System.IO.File]::ReadAllText($portfile.SourcePath) } else { '' }
    $status = @{}
    if ($Comparison) { foreach ($file in @($Comparison.Files)) { $status[[string]$file.File] = [string]$file.Status } }
    $records = New-Object System.Collections.Generic.List[object]
    foreach ($file in @(ConvertTo-OrdinalOrder -Items @($RecipeFiles) -Property Name)) {
        if (-not $file.Name.StartsWith($prefix, [System.StringComparison]::Ordinal)) { continue }
        $relative = $file.Name.Substring($prefix.Length)
        if ($relative -notmatch '\.(patch|diff)$') { continue }
        # Applied when the portfile names it (in PATCHES, or in a list of patches it applies).
        if ($portfileText -notmatch ('(?<![\w.-])' + [regex]::Escape([System.IO.Path]::GetFileName($relative)) + '(?![\w.-])')) { continue }
        $text = [System.IO.File]::ReadAllText($file.SourcePath)
        $lucent = [bool]$Comparison -and $status.ContainsKey($relative) -and $status[$relative] -in 'added', 'changed'
        $date = $BaselineDate
        if ($lucent) { $date = if ($text -match 'Changed by the Lucent project,\s*(\d{4}-\d{2}-\d{2})') { $Matches[1] } else { $null } }
        $records.Add([ordered]@{
            patch = $file.Name
            by = $(if ($lucent) { 'the Lucent project' } else { 'vcpkg' })
            source = $(if ($lucent) { "Lucent's overlay port" } else { "the vcpkg registry at commit $Baseline" })
            date = $(if ($date) { $date } else { $null })
            changes = @(Get-PatchedFiles -Text $text) })
    }
    return $records.ToArray()
}

function Format-RecipeModifications {
    # The "Modified" lines of a library (README.txt and LGPL-NOTICE.txt): each patch with who made it,
    # when, and the files it changes; or that the library is built from its source as released.
    param([AllowEmptyCollection()][object[]]$Modifications = @(), [Parameter(Mandatory)][string]$Prefix, [string]$Baseline)
    $short = if ($Baseline -and $Baseline.Length -ge 8) { $Baseline.Substring(0, 8) } else { $Baseline }
    # None given ($null: what an empty array becomes on its way through an if) is none.
    $items = @($Modifications | Where-Object { $null -ne $_ })
    if ($items.Count -eq 0) { return @(Format-IndentedText -Prefix $Prefix -Text 'no: built from the upstream source archive as released.') }
    $lines = New-Object System.Collections.Generic.List[string]
    $first = $true
    foreach ($modification in $items) {
        $who = if ($modification.by -eq 'the Lucent project') { "the Lucent project, $(if ($modification.date) { $modification.date } else { 'undated' })" }
               else { "vcpkg's, as in its registry at commit $short of $(if ($modification.date) { $modification.date } else { 'an unknown date' })" }
        $changes = @($modification.changes | Where-Object { $_ })
        $text = "$([System.IO.Path]::GetFileName([string]$modification.patch)) ($who) changes $(if ($changes.Count) { Format-NameList $changes } else { 'no file it names' })."
        foreach ($line in (Format-IndentedText -Prefix $(if ($first) { $Prefix } else { ' ' * $Prefix.Length }) -Text $text)) { $lines.Add($line) }
        $first = $false
    }
    return $lines.ToArray()
}

function Format-StaticCodeText {
    # What of the libraries' packages is linked into Lucent's programs (README.txt of the corresponding
    # source; W12 review LIC-5): their statically linked libraries, with the programs and the licence.
    param([AllowEmptyCollection()][object[]]$Components = @(), [Parameter(Mandatory)]$Info)
    $parts = New-Object System.Collections.Generic.List[string]
    foreach ($component in $Components) {
        if (-not ($component.PSObject.Properties.Name -contains 'StaticLibraries')) { continue }
        foreach ($static in @($component.StaticLibraries | Where-Object { $_ })) {
            $parts.Add("$($static.Name) ($($static.File), from $($component.Port)), linked statically into $(Get-StaticLibraryProgramsText $static $Info) and used under $($static.LicenseConcluded)")
        }
    }
    if ($parts.Count -eq 0) { return "No code of them is linked into $(Get-LucentExecutablesText $Info)." }
    return "The only code of these packages inside Lucent's programs is $(Format-NameList $parts.ToArray()), not under the LGPL (licenses\LGPL-NOTICE.txt and THIRD-PARTY-NOTICES.txt in Lucent's installation folder)."
}

function Format-RecipeChanges {
    # "lucent-x.patch (added), portfile.cmake (changed)" from Compare-OverlayPort's files.
    param([AllowEmptyCollection()][object[]]$Files)
    $changed = @($Files | Where-Object { $_.Status -ne 'unchanged' } | ForEach-Object {
        $what = if ($_.Status -eq 'line endings') { 'line endings only' } else { $_.Status }
        "$($_.File) ($what)" })
    if ($changed.Count -eq 0) { return 'none' }
    return ($changed -join ', ')
}

function New-CorrespondingSourcePackage {
    <#
    .SYNOPSIS
        Writes Lucent-<version>-corresponding-source.zip: the corresponding source of every shipped
        library whose licence needs one (the LGPL libraries; Get-SourceProvisionPlan), as built for
        this package (W12, ADR 0024). Returns what it holds.
    .DESCRIPTION
        For each such component (folder Lucent-<version>-corresponding-source/ in the archive):
        - sources/<archive>: every upstream source archive vcpkg built it from, from vcpkg's downloads
          folder. Its SHA-512 must be the one vcpkg verified for the build (the port's SPDX document)
          and must be pinned in the port's recipe: at the registry baseline commit for a registry
          port (git grep in the vcpkg checkout), in the overlay port for a port Lucent overlays.
        - ports/<port>/: the recipe exactly as built (Get-RecipeCopy): the registry port's folder,
          which must be the port at the baseline commit (its git tree), or Lucent's overlay port,
          compared with the registry's port file by file (Compare-OverlayPort) so README.txt names
          Lucent's changes.
        - build/<port>/vcpkg_abi_info.txt: vcpkg's record of the build's inputs (features, the hashes
          of the recipe, triplet and helper scripts, tool versions). The triplet and post-portfile
          files it records must be those in triplets/.
        And: triplets/ (vcpkg-configuration.json "overlay-triplets"), lucent/vcpkg.json and
        lucent/vcpkg-configuration.json, the recipes of Lucent's other overlay ports that the build
        installed (build tools such as gettext; their archives are included only when a library's
        already are), MANIFEST.json (all of it with hashes, and the build's other ports with the
        hashes of their sources, not included), README.txt
        (installer\notices\CORRESPONDING-SOURCE-README.txt.in) and SHA256SUMS.txt. The archive is
        deterministic (New-DeterministicZip). Any problem stops before anything is written.
    .OUTPUTS
        File, Path, Size, Sha256, Root, Components (port, version, licence, archives, recipe, ...),
        Entries (path inside the archive, size, SHA-256) and Texts (the entries that are not
        upstream archives, with their bytes, for the builder-path scan).
    #>
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)]$Info,
        [Parameter(Mandatory)][object[]]$Components,
        [Parameter(Mandatory)]$Report,
        [Parameter(Mandatory)]$Policy,
        [Parameter(Mandatory)][string]$RepoRoot,
        [Parameter(Mandatory)][string]$DownloadsDir,
        [Parameter(Mandatory)][string]$NoticesDir,
        [Parameter(Mandatory)][string]$VcpkgRoot,
        [Parameter(Mandatory)][string]$Baseline,
        [Parameter(Mandatory)][string]$GitCommit,
        [bool]$GitDirty,
        [string]$Msvc = 'unknown')

    $errors = New-Object System.Collections.Generic.List[string]
    $plan = Get-SourceProvisionPlan -Components $Components -Policy $Policy
    foreach ($problem in $plan.Errors) { $errors.Add($problem) }
    if (@($plan.Components).Count -eq 0) { $errors.Add('No shipped component is marked for a corresponding-source provision; the LGPL libraries at least must be (installer\license-policy.json "lgpl").') }
    $probe = Invoke-NativeCommand -FilePath 'git' -Arguments @('-C', $VcpkgRoot, 'cat-file', '-e', "$Baseline^{commit}")
    if ($probe.ExitCode -ne 0) {
        throw "The vcpkg checkout $VcpkgRoot does not hold the registry baseline commit $Baseline; the recipes cannot be checked against it (ADR 0024)."
    }
    # The date of the registry commit the recipes are taken from: the date of vcpkg's patches as built.
    $baselineDate = $null
    $dated = Invoke-NativeCommand -FilePath 'git' -Arguments @('-C', $VcpkgRoot, 'show', '-s', '--format=%cs', $Baseline)
    if ($dated.ExitCode -eq 0 -and $dated.Output.Count -gt 0 -and [string]$dated.Output[0] -match '^\d{4}-\d{2}-\d{2}$') { $baselineDate = [string]$dated.Output[0] }
    $tripletDir = Join-Path (ConvertTo-NativePath $Info.vcpkgInstalledDir) $Info.vcpkgTriplet
    $rootName = "Lucent-$($Info.version)-corresponding-source"
    $utf8 = New-Object System.Text.UTF8Encoding($false)
    $entries = New-Object System.Collections.Generic.List[object]   # Name (inside the root), SourcePath or Content, Kind
    $archivesByName = @{}

    # The triplet folder(s), as vcpkg-configuration.json names them.
    $configuration = Get-Content -LiteralPath (Join-Path $RepoRoot 'vcpkg-configuration.json') -Raw | ConvertFrom-Json
    $tripletFiles = New-Object System.Collections.Generic.List[object]
    if ($configuration.PSObject.Properties.Name -contains 'overlay-triplets') {
        foreach ($overlay in @($configuration.'overlay-triplets')) {
            $base = [System.IO.Path]::GetFullPath($(if ([System.IO.Path]::IsPathRooted($overlay)) { $overlay } else { Join-Path $RepoRoot $overlay }))
            foreach ($file in @(ConvertTo-OrdinalOrder -Items @(Get-ChildItem -LiteralPath $base -Recurse -File) -Property FullName)) {
                $relative = 'triplets/' + (Get-RelativePathText -Root $base -Path $file.FullName).Replace('\', '/')
                $tripletFiles.Add([pscustomobject]@{ Name = $relative; SourcePath = $file.FullName; Sha256 = (Get-Sha256 $file.FullName) })
            }
        }
    }
    $tripletFile = @($tripletFiles | Where-Object { $_.Name -eq "triplets/$($Info.vcpkgTriplet).cmake" }) | Select-Object -First 1
    if (-not $tripletFile) { $errors.Add("The triplet $($Info.vcpkgTriplet) is not in the repository's overlay triplets (vcpkg-configuration.json); a build with a registry triplet is not packaged.") }
    foreach ($file in $tripletFiles) { $entries.Add([pscustomobject]@{ Name = $file.Name; SourcePath = $file.SourcePath; Kind = 'triplet' }) }

    # Lucent's vcpkg manifest and configuration (features, baseline, overlay folders).
    foreach ($name in 'vcpkg.json', 'vcpkg-configuration.json') {
        $entries.Add([pscustomobject]@{ Name = "lucent/$name"; SourcePath = (Join-Path $RepoRoot $name); Kind = 'lucent' })
    }

    # Which DLLs import which (the audit's dumpbin results), for "how Lucent loads them".
    $importers = @{}
    foreach ($pe in $Report.PeFiles) {
        foreach ($name in (@($pe.Imports) + @($pe.DelayImports) | Where-Object { $_ })) {
            $key = ([string]$name).ToLowerInvariant()
            if (-not $importers.ContainsKey($key)) { $importers[$key] = New-Object System.Collections.Generic.List[string] }
            if (-not $importers[$key].Contains($pe.Path)) { $importers[$key].Add($pe.Path) }
        }
    }

    $records = New-Object System.Collections.Generic.List[object]
    foreach ($component in @(ConvertTo-OrdinalOrder -Items @($plan.Components) -Property Port)) {
        $port = $component.Port
        $expectation = Get-SourceProvisionExpectation -Component $component
        $recipeRef = $component.RecipeRef
        $isOverlay = [bool]($recipeRef -and $recipeRef.OverlayPath)

        # The recipe as built.
        $recipeFolder = if ($isOverlay) { $recipeRef.OverlayPath } else { Join-Path $VcpkgRoot "ports\$port" }
        $describe = if ($isOverlay) { "Lucent's overlay port $($recipeRef.Overlay)" } else { "the vcpkg ports tree ($recipeFolder)" }
        if (-not $isOverlay -and -not ($recipeRef -and $recipeRef.AtBaseline)) {
            $tree = if ($recipeRef) { $recipeRef.Tree } else { $null }
            $errors.Add("$($port): vcpkg built it with the recipe tree $tree, which is not the port at the registry baseline $($Baseline): the corresponding source would not be the registry's port.")
        }
        $recipeCopy = @(Get-RecipeCopy -Port $port -Folder $recipeFolder -Describe $describe -RecipeFiles $expectation.RecipeFiles -Errors $errors)
        foreach ($file in $recipeCopy) { $entries.Add([pscustomobject]@{ Name = $file.Name; SourcePath = $file.SourcePath; Kind = 'recipe' }) }
        $comparison = $null
        if ($isOverlay) { $comparison = Compare-OverlayPort -OverlayPath $recipeFolder -Port $port -VcpkgRoot $VcpkgRoot -Baseline $Baseline }
        # What the recipe changes in the library's upstream source, with who and when (LIC-1).
        $modifications = @(Get-RecipeModifications -Port $port -RecipeFiles $recipeCopy -Comparison $comparison -Baseline $Baseline -BaselineDate $baselineDate)

        # The upstream source archives.
        $archives = New-Object System.Collections.Generic.List[object]
        if (@($component.Sources).Count -eq 0) { $errors.Add("$($port): vcpkg recorded no source archive (share\$port\vcpkg.spdx.json).") }
        foreach ($source in @($component.Sources)) {
            if (([string]$source.Sha512) -notmatch '^[0-9A-Fa-f]{128}$') {
                $errors.Add("$($port): vcpkg recorded no SHA-512 for its source $($source.Name) ($($source.Location)); an archive that cannot be verified is not packaged.")
                continue
            }
            $archive = $null
            try { $archive = Get-SourceArchive -Resource $source.Resource -DownloadsDir $DownloadsDir }
            catch { $errors.Add("$($port): $($_.Exception.Message)"); continue }
            if (-not $archive) {
                $errors.Add("$($port): the source archive of $($source.Name) ($($source.Location)) is not in vcpkg's downloads folder $DownloadsDir. vcpkg keeps the archives of the ports it built on this machine; for a port restored from the binary cache, fetch it with 'vcpkg install --only-downloads' (the archive vcpkg pins, nothing else).")
                continue
            }
            $sha512 = ([string]$source.Sha512).ToLowerInvariant()
            $pins = @(Find-SourcePin -Sha512 $sha512 -Port $port -OverlayPath $(if ($isOverlay) { $recipeFolder } else { $null }) -VcpkgRoot $VcpkgRoot -Baseline $Baseline)
            $registryPins = if ($isOverlay) { @(Find-SourcePin -Sha512 $sha512 -Port $port -VcpkgRoot $VcpkgRoot -Baseline $Baseline) } else { $pins }
            if ($pins.Count -eq 0) {
                $errors.Add("$($port): the SHA-512 of $([System.IO.Path]::GetFileName($archive)) is not pinned in its recipe ($(if ($isOverlay) { $recipeRef.Overlay } else { "ports/$port at $Baseline" })).")
                continue
            }
            $fileName = [System.IO.Path]::GetFileName($archive)
            $key = $fileName.ToLowerInvariant()
            if ($archivesByName.ContainsKey($key)) {
                if ($archivesByName[$key].Sha512 -ne $sha512) { $errors.Add("Two different source archives are named $fileName ($port and $($archivesByName[$key].Port)).") }
            }
            else {
                $archivesByName[$key] = [pscustomobject]@{ Port = $port; FileName = $fileName; Sha512 = $sha512; Sha256 = (Get-Sha256 $archive); Size = (Get-Item -LiteralPath $archive).Length }
                $entries.Add([pscustomobject]@{ Name = "sources/$fileName"; SourcePath = $archive; Kind = 'archive' })
            }
            $archives.Add([ordered]@{
                file = "sources/$($archivesByName[$key].FileName)"; url = $source.Location; download = (Format-SourceDownload $source.Location)
                size = $archivesByName[$key].Size
                sha512 = $sha512; sha256 = $archivesByName[$key].Sha256
                pinnedIn = @($pins | ForEach-Object { if ($isOverlay) { "$_ (Lucent's overlay port)" } else { "$_ (vcpkg registry $Baseline)" } })
                registryPinnedIn = @($registryPins | ForEach-Object { "$_ (vcpkg registry $Baseline)" }) })
        }

        # vcpkg's record of the build's inputs: features, and the triplet the build used.
        $features = @()
        $abiPath = Join-Path $tripletDir "share\$port\vcpkg_abi_info.txt"
        if (-not (Test-Path -LiteralPath $abiPath -PathType Leaf)) { $errors.Add("$($port): vcpkg_abi_info.txt is missing ($abiPath).") }
        else {
            $abi = Read-VcpkgAbiInfo -Path $abiPath
            if ($abi.Contains('features')) { $features = @(([string]$abi['features']) -split ';' | Where-Object { $_ }) }
            else { $errors.Add("$($port): vcpkg_abi_info.txt records no features.") }
            # triplet_abi starts with the SHA-256 of the triplet file (vcpkg 2026-07-27); each
            # post_portfile_include_<n> is the SHA-256 of a post-portfile script.
            $tripletAbi = if ($abi.Contains('triplet_abi')) { ([string]$abi['triplet_abi'] -split '-')[0] } else { $null }
            if (-not $tripletFile -or $tripletAbi -ne $tripletFile.Sha256) {
                $errors.Add("$($port) was built with another triplet file than $(if ($tripletFile) { $tripletFile.Name } else { 'the repository''s' }) (vcpkg_abi_info.txt triplet_abi $tripletAbi). Rebuild the port.")
            }
            foreach ($abiKey in @($abi.Keys | Where-Object { $_ -like 'post_portfile_include_*' })) {
                if (-not ($tripletFiles | Where-Object { $_.Sha256 -eq $abi[$abiKey] })) {
                    $errors.Add("$($port) was built with a post-portfile script ($abiKey $($abi[$abiKey])) that is not among the triplet files. Rebuild the port.")
                }
            }
            $entries.Add([pscustomobject]@{ Name = "build/$port/vcpkg_abi_info.txt"; SourcePath = $abiPath; Kind = 'build' })
        }

        # The shipped files, and what loads each DLL.
        $loading = New-Object System.Collections.Generic.List[object]
        foreach ($file in @(ConvertTo-OrdinalOrder -Items @($component.Files | Where-Object { $_ -match '\.dll$' }))) {
            $leaf = [System.IO.Path]::GetFileName($file)
            $by = @()
            if ($file -notmatch '\\' -and $importers.ContainsKey($leaf.ToLowerInvariant())) { $by = @(ConvertTo-OrdinalOrder -Items @($importers[$leaf.ToLowerInvariant()])) }
            $how = if ($file -match '^plugins\\') { 'a Qt plugin, loaded by Qt by file name' }
                   elseif ($file -match '^qml\\') { 'a QML module plugin, loaded by Qt QML by file name' }
                   elseif ($by.Count -gt 0) { 'imported by ' + ($by -join ', ') }
                   else { 'not imported' }
            $loading.Add([ordered]@{ file = $file; importedBy = $by; loaded = $how })
        }

        $recipeRecord = [ordered]@{
            folder = "ports/$port"
            origin = $(if ($isOverlay) { 'Lucent overlay port' } else { 'vcpkg registry port' })
            portVersion = $component.PortVersion
            registryTree = $(if ($isOverlay) { $(if ($comparison) { $comparison.Tree } else { $null }) } else { $recipeRef.Tree })
            registryUrl = $(if (-not $isOverlay -and $recipeRef.Url) { $recipeRef.Url } elseif ($comparison -and $comparison.InRegistry) { "https://github.com/microsoft/vcpkg/tree/$Baseline/ports/$port" } else { $null })
            lucentSource = $(if ($isOverlay) { $recipeRef.Overlay } else { $null })
            lucentChanges = @(if ($comparison) { $comparison.Files | Where-Object { $_.Status -ne 'unchanged' } | ForEach-Object { [ordered]@{ file = $_.File; status = $_.Status } } })
            files = @($recipeCopy | ForEach-Object { [ordered]@{ path = $_.Name; sha256 = $_.Sha256 } }) }
        $records.Add([pscustomobject]@{
            Port = $port; Version = $component.Version; PortVersion = $component.PortVersion; Licence = $component.LicenseConcluded
            Homepage = $component.Homepage; Features = $features; Files = @($component.Files); Loading = $loading.ToArray()
            Archives = $archives.ToArray(); Recipe = $recipeRecord; Comparison = $comparison; IsOverlay = $isOverlay
            Modifications = $modifications; StaticLibraries = @($component.StaticLibraries | Where-Object { $_ }) })
    }

    # Lucent's other overlay ports that this build installed: build tools of a shipped library (the
    # gettext programs GLib's build runs). Their recipes are part of how the libraries were built.
    $buildTools = New-Object System.Collections.Generic.List[object]
    $includedPorts = @($records | ForEach-Object Port)
    if ($configuration.PSObject.Properties.Name -contains 'overlay-ports') {
        foreach ($overlay in @($configuration.'overlay-ports')) {
            $base = [System.IO.Path]::GetFullPath($(if ([System.IO.Path]::IsPathRooted($overlay)) { $overlay } else { Join-Path $RepoRoot $overlay }))
            foreach ($folder in @(ConvertTo-OrdinalOrder -Items @(Get-ChildItem -LiteralPath $base -Directory) -Property Name)) {
                $port = $folder.Name
                if ($includedPorts -contains $port) { continue }
                $spdxPath = Join-Path $tripletDir "share\$port\vcpkg.spdx.json"
                if (-not (Test-Path -LiteralPath $spdxPath -PathType Leaf)) { continue }   # not part of this build
                $spdx = Get-Content -LiteralPath $spdxPath -Raw | ConvertFrom-Json
                $portPackage = $spdx.packages | Where-Object { $_.SPDXID -eq 'SPDXRef-port' } | Select-Object -First 1
                $pseudo = [pscustomobject]@{ Port = $port; Version = (Get-PortBaseVersion $portPackage.versionInfo); LicenseConcluded = $portPackage.licenseConcluded
                    Sources = @($spdx.packages | Where-Object { $_.SPDXID -like 'SPDXRef-resource-*' } | ForEach-Object {
                        [pscustomobject]@{ Name = $_.name; Location = $_.downloadLocation; Sha512 = ($_.checksums | Where-Object { $_.algorithm -eq 'SHA512' } | Select-Object -First 1).checksumValue } })
                    Spdx = $spdx }
                $expectation = Get-SourceProvisionExpectation -Component $pseudo
                $relativeFolder = (Get-RelativePathText -Root $RepoRoot -Path $folder.FullName).Replace('\', '/')
                $copy = @(Get-RecipeCopy -Port $port -Folder $folder.FullName -Describe "Lucent's overlay port $relativeFolder" -RecipeFiles $expectation.RecipeFiles -Errors $errors)
                foreach ($file in $copy) { $entries.Add([pscustomobject]@{ Name = $file.Name; SourcePath = $file.SourcePath; Kind = 'recipe' }) }
                $comparison = Compare-OverlayPort -OverlayPath $folder.FullName -Port $port -VcpkgRoot $VcpkgRoot -Baseline $Baseline
                $buildTools.Add([pscustomobject]@{
                    Port = $port; Version = $pseudo.Version; PortVersion = $portPackage.versionInfo; Licence = $pseudo.LicenseConcluded
                    Comparison = $comparison; LucentSource = $relativeFolder
                    Recipe = [ordered]@{ folder = "ports/$port"; origin = 'Lucent overlay port'; portVersion = $portPackage.versionInfo
                        registryTree = $comparison.Tree; lucentSource = $relativeFolder
                        lucentChanges = @(@($comparison.Files) | Where-Object { $_.Status -ne 'unchanged' } | ForEach-Object { [ordered]@{ file = $_.File; status = $_.Status } })
                        files = @($copy | ForEach-Object { [ordered]@{ path = $_.Name; sha256 = $_.Sha256 } }) }
                    Archives = @($pseudo.Sources | ForEach-Object {
                        $sha = ([string]$_.Sha512).ToLowerInvariant()
                        $included = @(ConvertTo-OrdinalOrder -Items @($archivesByName.Keys) | Where-Object { $archivesByName[$_].Sha512 -eq $sha }) | Select-Object -First 1
                        [ordered]@{ url = $_.Location; sha512 = $sha; file = $(if ($included) { "sources/$($archivesByName[$included].FileName)" } else { $null }) } }) })
            }
        }
    }

    # The rest of the build's ports: not included, listed with the hashes of their sources.
    $shippedPorts = @($Components | ForEach-Object Port)
    $buildToolPorts = @($buildTools | ForEach-Object Port)
    $others = New-Object System.Collections.Generic.List[object]
    foreach ($folder in @(ConvertTo-OrdinalOrder -Items @(Get-ChildItem -LiteralPath (Join-Path $tripletDir 'share') -Directory) -Property Name)) {
        $spdxPath = Join-Path $folder.FullName 'vcpkg.spdx.json'
        if (-not (Test-Path -LiteralPath $spdxPath -PathType Leaf)) { continue }
        $spdx = Get-Content -LiteralPath $spdxPath -Raw | ConvertFrom-Json
        $portPackage = $spdx.packages | Where-Object { $_.SPDXID -eq 'SPDXRef-port' } | Select-Object -First 1
        $port = $portPackage.name
        if ($includedPorts -contains $port -or $buildToolPorts -contains $port) { continue }
        $component = $Components | Where-Object { $_.Port -eq $port } | Select-Object -First 1
        $licence = if ($component) { $component.LicenseConcluded } elseif ($portPackage.licenseConcluded -and $portPackage.licenseConcluded -notlike 'LicenseRef-vcpkg-null') { $portPackage.licenseConcluded } else { 'NOASSERTION' }
        $others.Add([ordered]@{
            port = $port; portVersion = $portPackage.versionInfo; licence = $licence
            use = $(if ($shippedPorts -contains $port) { 'shipped (permissive licence)' } else { 'build only (not shipped)' })
            sources = @($spdx.packages | Where-Object { $_.SPDXID -like 'SPDXRef-resource-*' } | ForEach-Object {
                [ordered]@{ url = $_.downloadLocation; download = (Format-SourceDownload $_.downloadLocation)
                            sha512 = ([string](($_.checksums | Where-Object { $_.algorithm -eq 'SHA512' } | Select-Object -First 1).checksumValue)).ToLowerInvariant() } }) })
    }

    if ($errors.Count -gt 0) {
        throw ("The corresponding-source package cannot be made (ADR 0024):`n  " + ($errors -join "`n  "))
    }

    # MANIFEST.json.
    $manifest = [ordered]@{
        schema = $script:SourcePackageSchema; schemaVersion = 1
        product = 'Lucent'; productVersion = $Info.version; architecture = 'x64'
        installer = "Lucent-Setup-$($Info.version)-x64.exe"
        description = "The corresponding source of the libraries Lucent $($Info.version) ships under the GNU LGPL, as built for that installer: upstream source archives (sources/), vcpkg recipes (ports/), the triplet (triplets/), vcpkg's build records (build/) and Lucent's vcpkg manifest (lucent/). See README.txt."
        lucent = [ordered]@{ licence = 'MIT'; repository = $Info.homepage; commit = $GitCommit; uncommittedChanges = [bool]$GitDirty
            files = @('lucent/vcpkg.json', 'lucent/vcpkg-configuration.json') }
        vcpkg = [ordered]@{ registry = 'https://github.com/microsoft/vcpkg'; baseline = $Baseline
            triplet = $Info.vcpkgTriplet; hostTriplet = $Info.vcpkgTriplet
            tripletFiles = @($tripletFiles | ForEach-Object { [ordered]@{ path = $_.Name; sha256 = $_.Sha256 } }) }
        toolchain = [ordered]@{ msvc = $Msvc
            note = "The MSVC toolset of the build environment. vcpkg's ABI hash of every port covers the compiler it detected (triplet_abi in build/<port>/vcpkg_abi_info.txt), so a library restored from vcpkg's binary cache was built by the same compiler." }
        components = @($records | ForEach-Object { [ordered]@{
            port = $_.Port; version = $_.Version; portVersion = $_.PortVersion; licence = $_.Licence; homepage = $_.Homepage
            features = @($_.Features); shippedFiles = @($_.Files); loading = @($_.Loading)
            archives = @($_.Archives); recipe = $_.Recipe; modifications = @($_.Modifications); buildRecord = "build/$($_.Port)/vcpkg_abi_info.txt" } })
        buildTools = @($buildTools | ForEach-Object { [ordered]@{
            port = $_.Port; version = $_.Version; portVersion = $_.PortVersion; licence = $_.Licence
            note = 'Built and run while the libraries above were built (not shipped). Its recipe is included; its source archive only when a library above uses the same one.'
            archives = @($_.Archives); recipe = $_.Recipe } })
        notIncluded = $others.ToArray()
    }
    $manifestText = (ConvertTo-Json -InputObject $manifest -Depth 32) + "`n"
    $entries.Add([pscustomobject]@{ Name = 'MANIFEST.json'; Content = $utf8.GetBytes($manifestText); Kind = 'text' })

    # README.txt from its template.
    $componentLines = New-Object System.Collections.Generic.List[string]
    $loading = New-Object System.Collections.Generic.List[object]
    $installItems = New-Object System.Collections.Generic.List[string]
    foreach ($record in $records) {
        $componentLines.Add("$($record.Port) $($record.Version) ($($record.Licence))")
        if ($record.Homepage) { $componentLines.Add("  Home page:  $($record.Homepage)") }
        $dlls = @($record.Files | Where-Object { $_ -match '\.dll$' -and $_ -notmatch '\\' })
        $pluginCount = @($record.Files | Where-Object { $_ -match '\.dll$' -and $_ -match '\\' }).Count
        foreach ($line in (Format-WrappedList -Prefix '  Ships as:   ' -Items $dlls)) { $componentLines.Add($line) }
        if ($pluginCount -gt 0) { $componentLines.Add("              and $pluginCount plugin DLLs in plugins\ and qml\") }
        foreach ($archive in $record.Archives) {
            $componentLines.Add("  Source:     $($archive.file.Replace('/', '\'))")
            $componentLines.Add("              from $($archive.download)")
            $componentLines.Add("              SHA-512 $($archive.sha512)")
        }
        $recipeText = if ($record.IsOverlay) {
            "ports\$($record.Port)\: the Lucent project's overlay of vcpkg's port $($record.Port) $($record.PortVersion) ($($record.Recipe.lucentSource) in Lucent's source). Its changes to vcpkg's port at the registry commit: $(Format-RecipeChanges $record.Comparison.Files)."
        } else {
            "ports\$($record.Port)\: vcpkg's port $($record.Port) $($record.PortVersion) at the registry commit, unchanged$(if ($record.Recipe.registryUrl) { " ($($record.Recipe.registryUrl))" })."
        }
        foreach ($line in (Format-IndentedText -Prefix '  Recipe:     ' -Text $recipeText)) { $componentLines.Add($line) }
        foreach ($line in (Format-RecipeModifications -Modifications @($record.Modifications) -Prefix '  Modified:   ' -Baseline $Baseline)) { $componentLines.Add($line) }
        foreach ($line in (Format-WrappedList -Prefix '  Features:   ' -Items @($record.Features))) { $componentLines.Add($line) }
        $componentLines.Add('')
        foreach ($item in $record.Loading) {
            if ($item.file -match '\\') { continue }
            # Executables by name; other DLLs by name when they are few; plugins counted.
            $programs = @($item.importedBy | Where-Object { $_ -match '\.exe$' })
            $libraries = @($item.importedBy | Where-Object { $_ -match '\.dll$' -and $_ -notmatch '\\' })
            $plugins = @($item.importedBy | Where-Object { $_ -match '\\' })
            $parts = @($programs)
            if ($libraries.Count -le 3) { $parts += $libraries } else { $parts += "$($libraries.Count) other DLLs" }
            if ($plugins.Count -gt 0) { $parts += "$($plugins.Count) $(if ($plugins.Count -eq 1) { 'plugin' } else { 'plugins' })" }
            $text = if ($parts.Count -eq 0) { $item.loaded } elseif ($parts.Count -eq 1) { "imported by $($parts[0])" } else { 'imported by ' + (($parts[0..($parts.Count - 2)]) -join ', ') + ' and ' + $parts[-1] }
            $loading.Add([pscustomobject]@{ File = $item.file; Text = $text })
        }
        $installItems.Add('"' + $record.Port + '[' + (@($record.Features) -join ',') + ']"')
    }
    $column = (@($loading | ForEach-Object { $_.File.Length }) | Measure-Object -Maximum).Maximum + 2
    $loadingLines = @($loading | ForEach-Object { '    ' + $_.File.PadRight($column) + $_.Text })
    $sourceFolder = "C:\src\$rootName"
    $installLines = New-Object System.Collections.Generic.List[string]
    $installLines.Add("       C:\src\vcpkg\vcpkg.exe install ^")
    $installLines.Add("           --triplet $($Info.vcpkgTriplet) --host-triplet $($Info.vcpkgTriplet) ^")
    $installLines.Add("           --overlay-ports=$sourceFolder\ports ^")
    $installLines.Add("           --overlay-triplets=$sourceFolder\triplets ^")
    $current = '          '
    for ($i = 0; $i -lt $installItems.Count; $i++) {
        $item = $installItems[$i]
        if ($current.Length + 1 + $item.Length -gt 96 -and $current.Trim()) { $installLines.Add($current + ' ^'); $current = '          ' }
        $current += ' ' + $item
    }
    $installLines.Add($current)
    $toolNotes = @($buildTools | ForEach-Object {
        Format-Paragraph ("ports\$($_.Port)\ is the Lucent project's overlay of vcpkg's port $($_.Port) $($_.PortVersion) ($($_.LucentSource) in Lucent's source), " +
            "a build tool: building GLib runs its programs ($($_.Licence)), which are not part of Lucent. Its changes to vcpkg's port at the registry commit: " +
            "$(Format-RecipeChanges $_.Comparison.Files). Its source archive is " +
            $(if (@($_.Archives | Where-Object { $_.file }).Count -gt 0) { (@($_.Archives | Where-Object { $_.file } | ForEach-Object { $_.file.Replace('/', '\') }) -join ', ') + ', the same as a library''s above.' } else { 'not included (MANIFEST.json, "buildTools").' })) })
    $template = [System.IO.File]::ReadAllText((Join-Path $NoticesDir 'CORRESPONDING-SOURCE-README.txt.in'))
    $readme = Set-TemplateTitle -Template $template -Version $Info.version
    $replacements = [ordered]@{
        '@COMPONENTS@' = ($componentLines -join "`n").TrimEnd()
        '@LOADING@' = ($loadingLines -join "`n")
        '@INSTALL_COMMAND@' = ($installLines -join "`n")
        '@ONE_LIBRARY@' = $(@(@($installItems | Where-Object { $_.StartsWith('"lensfun[') }) + @($installItems))[0])
        '@BUILD_TOOLS@' = $(if ($toolNotes.Count -gt 0) { "`n" + ($toolNotes -join "`n`n") + "`n" } else { '' })
        '@BUILT_WITH@' = (Format-Paragraph "The libraries were built with vcpkg at commit $Baseline of its registry (https://github.com/microsoft/vcpkg), with Microsoft Visual C++ $Msvc (Visual Studio 2022, x64) and the Windows SDK. To build them again:")
        '@LUCENT_SOURCE@' = (Format-Paragraph ("Lucent is free software under the MIT License (licenses\Lucent-LICENSE.txt in its installation folder). Its source code, including its build files (CMake, vcpkg.json, cmake\ports, cmake\triplets) and the packaging scripts that made this archive (tools\package.ps1, installer\scripts\LucentPackaging.psm1), is published at $($Info.homepage). This version was built from commit $GitCommit" +
            $(if ($GitDirty) { ', with uncommitted changes: the recipes, triplet and vcpkg manifest in this archive are the ones it was built with.' } else { '.' })))
        '@STATIC_CODE@' = (Format-Paragraph (Format-StaticCodeText -Components $records.ToArray() -Info $Info))
        '@BASELINE@' = $Baseline
        '@TRIPLET@' = $Info.vcpkgTriplet
        '@ROOT@' = $rootName }
    foreach ($pair in $replacements.GetEnumerator()) { $readme = $readme.Replace([string]$pair.Key, [string]$pair.Value) }
    if ($readme -cmatch '@[A-Z_]+@') { throw "CORRESPONDING-SOURCE-README.txt.in has a placeholder that was not filled: $($Matches[0])" }
    $readme = ($readme -replace "`r`n", "`n") -replace "`n", "`r`n"
    $entries.Add([pscustomobject]@{ Name = 'README.txt'; Content = $utf8.GetBytes($readme); Kind = 'text' })

    # SHA256SUMS.txt: every other entry, sorted as in the archive.
    $hasher = [System.Security.Cryptography.SHA256]::Create()
    try {
        $sums = @{}
        foreach ($entry in $entries) {
            $inline = ($entry.PSObject.Properties.Name -contains 'Content') -and $null -ne $entry.Content
            $sums[$entry.Name] = [pscustomobject]@{
                Name = $entry.Name
                Sha256 = $(if ($inline) { ConvertTo-HexText $hasher.ComputeHash([byte[]]$entry.Content) } else { Get-Sha256 $entry.SourcePath })
                Size = $(if ($inline) { $entry.Content.Length } else { (Get-Item -LiteralPath $entry.SourcePath).Length }) }
        }
        $sumNames = [string[]]@($sums.Keys)
        [Array]::Sort($sumNames, [System.StringComparer]::Ordinal)
        $sumsText = (@($sumNames | ForEach-Object { "$($sums[$_].Sha256) *$_" }) -join "`n") + "`n"
        $sumsBytes = $utf8.GetBytes($sumsText)
        $sums['SHA256SUMS.txt'] = [pscustomobject]@{ Name = 'SHA256SUMS.txt'; Sha256 = (ConvertTo-HexText $hasher.ComputeHash($sumsBytes)); Size = $sumsBytes.Length }
    }
    finally { $hasher.Dispose() }
    $entries.Add([pscustomobject]@{ Name = 'SHA256SUMS.txt'; Content = $sumsBytes; Kind = 'text' })

    # The archive.
    $zipEntries = @($entries | ForEach-Object {
        $item = [pscustomobject]@{ Name = "$rootName/$($_.Name)"; Content = $null; SourcePath = $null }
        if (($_.PSObject.Properties.Name -contains 'Content') -and $null -ne $_.Content) { $item.Content = $_.Content } else { $item.SourcePath = $_.SourcePath }
        $item })
    New-DeterministicZip -Path $Path -Entries $zipEntries
    $item = Get-Item -LiteralPath $Path
    $allNames = [string[]]@($sums.Keys)
    [Array]::Sort($allNames, [System.StringComparer]::Ordinal)
    $allSums = @($allNames | ForEach-Object { $sums[$_] })
    $texts = @($entries | Where-Object { $_.Kind -ne 'archive' } | ForEach-Object {
        $bytes = if (($_.PSObject.Properties.Name -contains 'Content') -and $null -ne $_.Content) { [byte[]]$_.Content } else { [System.IO.File]::ReadAllBytes($_.SourcePath) }
        [pscustomobject]@{ Name = "$($item.Name)/$rootName/$($_.Name)"; Bytes = $bytes } })
    return [pscustomobject]@{
        File = $item.Name; Path = $item.FullName; Size = $item.Length; Sha256 = (Get-Sha256 $item.FullName); Root = $rootName
        Components = $records.ToArray(); BuildTools = $buildTools.ToArray()
        Entries = @($allSums | ForEach-Object { [ordered]@{ path = "$rootName/$($_.Name)"; size = $_.Size; sha256 = $_.Sha256 } })
        Texts = $texts }
}

function Write-LgplNotice {
    <#
    .SYNOPSIS
        Writes <stage>\licenses\LGPL-NOTICE.txt (installer\notices\LGPL-NOTICE.txt.in): the LGPL
        libraries, the right to replace their DLLs, the statically linked non-LGPL code, and their
        corresponding source: the corresponding-source package (its file name, size and SHA-256;
        New-CorrespondingSourcePackage) and, per library, its upstream archives with SHA-512 and its
        recipe (ADR 0003, ADR 0009 decision 10, ADR 0024).
    #>
    param(
        [Parameter(Mandatory)][string]$StageDir,
        [Parameter(Mandatory)]$Info,
        [Parameter(Mandatory)][object[]]$Components,
        [Parameter(Mandatory)][string]$NoticesDir,
        [Parameter(Mandatory)]$SourcePackage,
        [string]$Baseline,
        [string]$GitCommit)
    $licenses = Join-Path $StageDir 'licenses'
    $lgplComponents = @($Components | Where-Object { $_.Lgpl })
    $textLines = New-Object System.Collections.Generic.List[string]
    $componentLines = New-Object System.Collections.Generic.List[string]
    foreach ($component in $lgplComponents) {
        $packaged = @($SourcePackage.Components | Where-Object { $_.Port -eq $component.Port }) | Select-Object -First 1
        if (-not $packaged) { throw "$($component.Port) is an LGPL library but not in $($SourcePackage.File)." }
        foreach ($text in $component.LicenceTexts) { $textLines.Add("    $text ($($component.Port))") }
        $componentLines.Add("$($component.Port) $($component.Version), $($component.LicenseConcluded)")
        $dlls = @($component.Files | Where-Object { $_ -match '\.dll$' })
        foreach ($line in (Format-WrappedList -Prefix '  DLLs: ' -Items $dlls)) { $componentLines.Add($line) }
        if ($component.Files.Count -gt $dlls.Count) {
            $componentLines.Add("  and $($component.Files.Count - $dlls.Count) QML module files (qmldir, type information)")
        }
        foreach ($source in $component.Sources) {
            $componentLines.Add("  Source: $(Format-SourceDownload $source.Location)")
            $componentLines.Add("          SHA-512 $($source.Sha512)")
        }
        if ($component.RecipeRef.Overlay) {
            # A port Lucent overlays: vcpkg's recipe with Lucent's build changes, shipped with Lucent.
            $componentLines.Add("  Build recipe: vcpkg port '$($component.Port)' version $($component.PortVersion) with Lucent's")
            $componentLines.Add("                changes (patches and build options), installed with Lucent in")
            $componentLines.Add("                $($component.RecipeRef.ShippedRecipe) (the overlay port $($component.RecipeRef.Overlay)")
            $componentLines.Add("                of Lucent's source" + $(if ($GitCommit) { " at commit $GitCommit)" } else { ')' }))
        }
        else {
            $recipe = "  Build recipe: vcpkg port '$($component.Port)' version $($component.PortVersion)"
            if ($component.RecipeRef.Tree) { $recipe += ", port tree $($component.RecipeRef.Tree)" }
            $componentLines.Add($recipe)
            if ($component.RecipeRef.Url) { $componentLines.Add("                $($component.RecipeRef.Url)") }
            else { $componentLines.Add("                (git tree object in https://github.com/microsoft/vcpkg; not the port folder at the registry baseline)") }
        }
        # The changes the recipe makes to the library's source, each with the files, who and when (LIC-1).
        $modifications = @()
        if ($packaged.PSObject.Properties.Name -contains 'Modifications') { $modifications = @($packaged.Modifications) }
        foreach ($line in (Format-RecipeModifications -Modifications $modifications -Prefix '  Modified: ' -Baseline $Baseline)) { $componentLines.Add($line) }
        $inPackage = @($packaged.Archives | ForEach-Object { $_.file.Replace('/', '\') }) + @("ports\$($component.Port)\")
        foreach ($line in (Format-WrappedList -Prefix "  In $($SourcePackage.File): " -Items $inPackage)) { $componentLines.Add($line) }
        $componentLines.Add('')
    }
    $staticNote = ''
    $allStatics = @($Components | ForEach-Object { $_.StaticLibraries } | Where-Object { $_ })
    if ($allStatics.Count -gt 0) {
        $staticNote = "`n" + (@($allStatics | ForEach-Object {
            # Assigned, not an if's value: one program would come back as a string, none as $null.
            $programs = @()
            if ($_.PSObject.Properties.Name -contains 'Executables') { $programs = @($_.Executables | Where-Object { $_ }) }
            Format-Paragraph "$(Get-StaticLibraryProgramsText $_ $Info) contain$(if ($programs.Count -eq 1) { 's' }) $($_.Name), $($_.File), linked statically. It is offered under $($_.LicenseDeclared), and Lucent uses it under $($_.LicenseConcluded) (THIRD-PARTY-NOTICES.txt)."
        }) -join "`n`n") + "`n"
    }
    $template = [System.IO.File]::ReadAllText((Join-Path $NoticesDir 'LGPL-NOTICE.txt.in'))
    $lgplText = Set-TemplateTitle -Template $template -Version $Info.version
    $replacements = [ordered]@{
        '@LICENCE_TEXTS@' = ($textLines -join "`n")
        '@COMPONENTS@' = ($componentLines -join "`n").TrimEnd()
        '@STATIC_LINKED@' = $staticNote
        '@VCPKG_BASELINE@' = $(if ($Baseline) { $Baseline } else { '(unknown)' })
        # Grouped the same way on every machine: the format operator follows the regional format (W12
        # review LIC-7: an Estonian machine wrote "132 480 210").
        '@SOURCE_PACKAGE_SIZE@' = ([int64]$SourcePackage.Size).ToString('N0', [System.Globalization.CultureInfo]::InvariantCulture)
        '@SOURCE_PACKAGE_SHA256@' = $SourcePackage.Sha256
        '@SOURCE_PACKAGE@' = $SourcePackage.File
        '@HOMEPAGE@' = $Info.homepage }
    foreach ($pair in $replacements.GetEnumerator()) { $lgplText = $lgplText.Replace([string]$pair.Key, [string]$pair.Value) }
    if ($lgplText -cmatch '@[A-Z_0-9]+@') { throw "LGPL-NOTICE.txt.in has a placeholder that was not filled: $($Matches[0])" }
    Write-TextFile -Path (Join-Path $licenses 'LGPL-NOTICE.txt') -Text $lgplText
}

# --- Inventory -----------------------------------------------------------------------------------

function Get-FileInventory {
    # In ordinal order: the package manifest lists the files the same way on every machine (LIC-7).
    param([Parameter(Mandatory)][string]$Root)
    return @(ConvertTo-OrdinalOrder -Items @(Get-ChildItem -LiteralPath $Root -Recurse -File) -Property FullName | ForEach-Object {
        [ordered]@{ path = (Get-RelativePathText -Root $Root -Path $_.FullName); size = $_.Length; sha256 = (Get-Sha256 $_.FullName) }
    })
}

Export-ModuleMember -Function Read-LucentPackageInfo, Get-PeHeader, Get-PeDependencies, Get-VcpkgFileIndex,
    Get-StagedFileOrigins, Invoke-LayoutAudit, Write-AuditReport, Invoke-SmokeRun, Get-StaticVcpkgLibraries, Get-LucentExecutablesText,
    Get-AssetLicences, New-LicenseBundle, New-PackageSbom, Write-InstalledFileList, Test-InstalledFileList, Find-InnoSetupCompiler, Get-FileInventory,
    Write-TextFile, Write-JsonFile, Get-Sha256, Get-Sha1, Get-RelativePathText, Invoke-NativeCommand,
    Get-BuilderPathRules, New-BuilderPathScanner, Invoke-BuilderPathAudit, Write-BuilderPathReport,
    Find-BuilderPathsInContent, New-CompressedContentTally, Format-CompressedContentTally, Initialize-CompressedContentType, New-CompressedContentBudget,
    Get-CodeSigningCertificate, Test-AuthenticodeFile, Get-UpdateKeyId, Get-LucentUpdateChannelRecord,
    ConvertFrom-UpdateChannelLogLine, Format-UpdateChannel, Compare-UpdateChannel, Read-ExpectedUpdateChannels,
    Test-LucentUpdateChannelPolicy, New-DeterministicZip, Test-CopyleftLicence, Get-SourceProvisionPlan,
    Get-SourceProvisionExpectation, ConvertFrom-SourcePackageRecord, Test-CorrespondingSourcePackage,
    New-CorrespondingSourcePackage, Write-LgplNotice, Format-SourceDownload, Get-RecipeCopy, Get-LicenceKind, ConvertTo-OrdinalOrder,
    Get-PatchedFiles, Get-RecipeModifications, Format-RecipeModifications, Format-StaticCodeText, Get-InnoSetupLicence, Test-InnoSetupLicence
