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
    - New-LicenseBundle           writes licenses\ (Lucent's licence, each shipped port's copyright
                                  file, licence texts missing from vcpkg, the third-party code inside
                                  Qt from Qt's own SBOM, LGPL notice, notices file).
    - New-PackageSbom             aggregates vcpkg's per-port SPDX documents and Qt's SBOMs into one
                                  SPDX 2.3 SBOM, and checks it structurally.
    - Write-InstalledFileList     writes Lucent-files.txt, the list the next Setup's upgrade uses.
    - Get-BuilderPathRules, New-BuilderPathScanner, Invoke-BuilderPathAudit
                                  scan every shipped file (and the installer, and the package
                                  manifest) for the build machine's folders: the user profile, the
                                  repository, the build tree and the vcpkg folders (W12).
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
        than DLL imports (lib /list).
    #>
    param([Parameter(Mandatory)]$Info, [Parameter(Mandatory)][hashtable]$FileIndex)
    $binaryDir = ConvertTo-NativePath $Info.binaryDir
    $ninja = Join-Path $binaryDir 'build.ninja'
    if (-not (Test-Path -LiteralPath $ninja)) { throw "$ninja is missing: the statically linked libraries cannot be determined (the presets use Ninja)." }
    $outputs = @{}
    foreach ($built in @(@($Info.builtExecutable) + @($Info.builtHelperExecutables) | Where-Object { $_ })) {
        $outputs[(Get-RelativePathText -Root $binaryDir -Path (ConvertTo-NativePath $built)).ToLowerInvariant()] = $null
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
    $libraries = ($outputs.Values -join ' ')
    $tripletDir = [System.IO.Path]::GetFullPath((Join-Path (ConvertTo-NativePath $Info.vcpkgInstalledDir) $Info.vcpkgTriplet)).TrimEnd('\') + '\'
    $lib = Join-Path (Split-Path -Parent (ConvertTo-NativePath $Info.dumpbin)) 'lib.exe'
    if (-not (Test-Path -LiteralPath $lib)) { throw "lib.exe was not found next to dumpbin ($lib)." }
    $found = @{}
    $escapedSpace = [string][char]0
    foreach ($token in ($libraries.Replace('$ ', $escapedSpace) -split ' ')) {
        $path = $token.Replace($escapedSpace, ' ') -replace '\$:', ':' -replace '\$\$', '$'
        if ($path -notmatch '\.lib$') { continue }
        if (-not [System.IO.Path]::IsPathRooted($path)) { $path = Join-Path $binaryDir $path }
        $full = [System.IO.Path]::GetFullPath($path)
        if (-not $full.StartsWith($tripletDir, [System.StringComparison]::OrdinalIgnoreCase) -or $found.ContainsKey($full.ToLowerInvariant())) { continue }
        $members = @(& $lib /nologo /list $full | Where-Object { $_.Trim() } | Sort-Object -Unique)
        if ($LASTEXITCODE -ne 0) { throw "lib /list failed on $full" }
        $objects = @($members | Where-Object { $_ -notmatch '\.dll$' })
        if ($objects.Count -eq 0) { continue }   # an import library
        $relative = $full.Substring($tripletDir.Length).Replace('\', '/')
        $owner = $FileIndex[$relative.ToLowerInvariant()]
        if (-not $owner) { throw "$full is linked into $($Info.executable) but belongs to no vcpkg port." }
        $found[$full.ToLowerInvariant()] = [pscustomobject]@{ Path = $relative; Name = [System.IO.Path]::GetFileName($full); Port = $owner.Port; Members = $objects }
    }
    return @($found.Values | Sort-Object Path)
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
        Targets = @($targets.Values | ForEach-Object { $_.Package.PackageName } | Sort-Object)
        Packages = $byName
        Attributions = @($attributions.Values | Sort-Object Name)
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
    if ($Policy.ports.PSObject.Properties.Name -contains $Port) { return $Policy.ports.$Port }
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
    return @($names | Where-Object { -not $binaries.ContainsKey($_) } | Sort-Object)
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

function Get-RecipeReference {
    <#
    .SYNOPSIS
        Identifies a port's vcpkg recipe. vcpkg's SPDX downloadLocation ends in the git tree hash of
        the port folder, not a commit; when the registry baseline commit holds exactly that tree, the
        port's folder at that commit is a browsable, checkable link.
    #>
    param([Parameter(Mandatory)]$Component, [string]$VcpkgRoot, [string]$Baseline)
    $tree = if ($Component.Recipe -match '@([0-9a-f]{40})$') { $Matches[1] } else { $null }
    $atBaseline = $false
    if ($tree -and $VcpkgRoot -and $Baseline -and (Test-Path -LiteralPath (Join-Path $VcpkgRoot '.git'))) {
        $probe = Invoke-NativeCommand -FilePath 'git' -Arguments @('-C', $VcpkgRoot, 'rev-parse', "$($Baseline):ports/$($Component.Port)")
        $atBaseline = ($probe.ExitCode -eq 0 -and $probe.Output.Count -gt 0 -and $probe.Output[0].Trim() -eq $tree)
    }
    return [pscustomobject]@{
        Tree = $tree; AtBaseline = $atBaseline
        Url = $(if ($atBaseline) { "https://github.com/microsoft/vcpkg/tree/$Baseline/ports/$($Component.Port)" } else { $null }) }
}

function Get-CopyrightSectionNames {
    # Names of the "<licence id>.txt:" sections of a Qt copyright file (vcpkg concatenates LICENSES\*.txt).
    param([Parameter(Mandatory)][string]$Path)
    return @([System.IO.File]::ReadAllLines($Path) | ForEach-Object { if ($_ -match '^([A-Za-z0-9.+-]+)\.txt:$') { $Matches[1] } })
}

function New-LicenseBundle {
    <#
    .SYNOPSIS
        Writes <stage>\licenses and returns the component list (used for the SBOM).
    .DESCRIPTION
        For each shipped port and each header-only direct dependency: its copyright file, licence
        texts named by installer\license-policy.json, credit lines, the libraries the executable links
        statically (each must be reviewed in the policy), and for Qt ports the third-party code inside
        the shipped Qt files as recorded by Qt's own SBOM. Then LGPL-NOTICE.txt and
        THIRD-PARTY-NOTICES.txt.
    #>
    param(
        [Parameter(Mandatory)][string]$StageDir,
        [Parameter(Mandatory)]$Info,
        [Parameter(Mandatory)]$Report,
        [Parameter(Mandatory)]$Policy,
        [Parameter(Mandatory)][string]$RepoRoot,
        [Parameter(Mandatory)][string]$DownloadsDir,
        [Parameter(Mandatory)][string]$NoticesDir,
        [AllowEmptyCollection()][object[]]$StaticLibraries = @(),
        [string]$VcpkgRoot,
        [string]$Baseline)

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
    foreach ($port in (@($dynamicPorts) + @($compiledIn) + @($staticPorts) | Sort-Object -Unique)) {
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
                throw "$(Get-LucentExecutablesText $Info) link $($library.Path) of port $port statically ($($library.Members -join ', ')); installer\license-policy.json has no reviewed entry for it (ports.$port.staticLibraries)."
            }
            if ($review.concluded -match 'GPL') {
                throw "$($library.Path) is used under $($review.concluded) but is linked statically into $(Get-LucentExecutablesText $Info); LGPL and GPL code must stay in replaceable DLLs (ADR 0003)."
            }
            $statics.Add([pscustomobject]@{
                Path = $library.Path; File = $library.Name; Name = $review.name; LicenseDeclared = $review.declared; LicenseConcluded = $review.concluded
                Copyright = $review.copyright; Comment = $review.comment
                QtSbomPackage = $(if ($review.PSObject.Properties.Name -contains 'qtSbomPackage') { $review.qtSbomPackage } else { $null }) })
        }

        # Qt's own SBOM: the third-party code compiled into the shipped Qt files.
        $files = @($Report.Files | Where-Object { $_.Port -eq $port } | ForEach-Object Path | Sort-Object)
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
            Sources = @($sources | ForEach-Object { [pscustomobject]@{ Name = $_.Name; Location = $_.Location; Sha512 = $_.Sha512 } })
            Spdx = $spdx })
        $components[$components.Count - 1].RecipeRef = Get-RecipeReference -Component $components[$components.Count - 1] -VcpkgRoot $VcpkgRoot -Baseline $Baseline
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

    # LGPL notice.
    $lgplComponents = @($components | Where-Object { $_.Lgpl })
    $textLines = New-Object System.Collections.Generic.List[string]
    $componentLines = New-Object System.Collections.Generic.List[string]
    foreach ($component in $lgplComponents) {
        foreach ($text in $component.LicenceTexts) { $textLines.Add("    $text ($($component.Port))") }
        $componentLines.Add("$($component.Port) $($component.Version), $($component.LicenseConcluded)")
        $dlls = @($component.Files | Where-Object { $_ -match '\.dll$' })
        foreach ($line in (Format-WrappedList -Prefix '  DLLs: ' -Items $dlls)) { $componentLines.Add($line) }
        if ($component.Files.Count -gt $dlls.Count) {
            $componentLines.Add("  and $($component.Files.Count - $dlls.Count) QML module files (qmldir, type information)")
        }
        foreach ($source in $component.Sources) {
            $componentLines.Add("  Source: $($source.Location)")
            $componentLines.Add("          SHA-512 $($source.Sha512)")
        }
        $recipe = "  Build recipe: vcpkg port '$($component.Port)' version $($component.PortVersion)"
        if ($component.RecipeRef.Tree) { $recipe += ", port tree $($component.RecipeRef.Tree)" }
        $componentLines.Add($recipe)
        if ($component.RecipeRef.Url) { $componentLines.Add("                $($component.RecipeRef.Url)") }
        else { $componentLines.Add("                (git tree object in https://github.com/microsoft/vcpkg; not the port folder at the registry baseline)") }
        $componentLines.Add('')
    }
    $staticNote = ''
    $allStatics = @($components | ForEach-Object { $_.StaticLibraries } | Where-Object { $_ })
    if ($allStatics.Count -gt 0) {
        $staticNote = "`n" + (@($allStatics | ForEach-Object {
            Format-Paragraph "$(Get-LucentExecutablesText $Info) contain $($_.Name), $($_.File), linked statically. It is offered under $($_.LicenseDeclared), and Lucent uses it under $($_.LicenseConcluded) (THIRD-PARTY-NOTICES.txt)."
        }) -join "`n`n") + "`n"
    }
    $template = [System.IO.File]::ReadAllText((Join-Path $NoticesDir 'LGPL-NOTICE.txt.in'))
    $lgplText = (Set-TemplateTitle -Template $template -Version $Info.version).Replace('@LICENCE_TEXTS@', ($textLines -join "`n")).Replace('@COMPONENTS@', ($componentLines -join "`n").TrimEnd()).Replace('@STATIC_LINKED@', $staticNote).Replace('@VCPKG_BASELINE@', $(if ($Baseline) { $Baseline } else { '(unknown)' }))
    Write-TextFile -Path (Join-Path $licenses 'LGPL-NOTICE.txt') -Text $lgplText

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
            $listLines.Add("  Linked statically into $(Get-LucentExecutablesText $Info): $($static.Name) ($($static.File)), $($static.LicenseConcluded)")
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
    if ($noticeLines.Count -eq 0) { $noticeLines.Add('(none)') }
    $template = [System.IO.File]::ReadAllText((Join-Path $NoticesDir 'THIRD-PARTY-NOTICES.txt.in'))
    $noticesText = (Set-TemplateTitle -Template $template -Version $Info.version).Replace('@NOTICES@', ($noticeLines -join "`n")).Replace('@COMPONENTS@', ($listLines -join "`n").TrimEnd())
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
        shipped Qt files, libraries linked statically into the executable, and the Visual C++
        redistributable bundled in Setup.
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
        Where-Object { -not $vcpkgPaths.ContainsKey($_.Relative.ToLowerInvariant()) -and $_.Relative -ine $sbomRelative } |
        Sort-Object Relative)
    $lucentId = 'SPDXRef-Lucent'
    $ownEntries = New-Object System.Collections.Generic.List[object]
    $counter = 0
    foreach ($file in $ownFiles) {
        $counter++
        $sha1 = Get-Sha1 $file.Full
        $ownEntries.Add([ordered]@{
            fileName = './' + $file.Relative.Replace('\', '/'); SPDXID = "SPDXRef-Lucent-file-$counter"
            checksums = @([ordered]@{ algorithm = 'SHA1'; checksumValue = $sha1 },
                          [ordered]@{ algorithm = 'SHA256'; checksumValue = (Get-Sha256 $file.Full) })
            licenseConcluded = $(if ($file.Relative -ieq $Info.executable -or @($Info.helperExecutables) -contains $file.Relative -or $file.Relative -like 'assets\*') { 'MIT' } else { 'NOASSERTION' })
            copyrightText = 'NOASSERTION' })
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
        licenseConcluded = 'MIT'; licenseDeclared = 'MIT'
        copyrightText = 'Copyright (c) 2026 Fan Zhu'
        comment = "Installed layout of Lucent $($Info.version) for x64 Windows, built from commit $GitCommit. Contains Lucent's own files (the executable, qt.conf, the installed-file list, the bundled data assets such as the camera support database, and the licence bundle); the third-party files are listed under each component's binary package." })
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
                $copy.comment = 'The vcpkg port (recipe). Its downloadLocation names the git tree of the port folder in the vcpkg registry' +
                    $(if ($component.RecipeRef -and $component.RecipeRef.Url) { ", which is the port at the registry baseline: $($component.RecipeRef.Url)" } elseif ($tree) { " ($tree)" } else { '' }) + '.'
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
                # from the vcpkg ports tree when that file is byte-identical, else leave the file out.
                $expected = (@($file.checksums) | Where-Object { $_.algorithm -eq 'SHA256' } | Select-Object -First 1).checksumValue
                $candidate = if ($VcpkgRoot) { Join-Path $VcpkgRoot ("ports\$($component.Port)\" + (ConvertTo-NativePath ($file.fileName -replace '^\./', ''))) } else { $null }
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
                comment = "Static library $($static.Path) of the vcpkg port $($component.Port), linked into $(Get-LucentExecutablesText $Info). $($static.Comment)" })
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
    foreach ($id in ($usedExtracted.Keys | Sort-Object)) {
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
            comment = "Aggregated from the vcpkg SPDX documents (share/<port>/vcpkg.spdx.json) of the ports whose files ship, triplet $($Info.vcpkgTriplet), and from Qt's own SBOMs (sbom/<qt port>-<version>.spdx) for the third-party code inside the shipped Qt files. Only shipped binary files are listed; recipe files carry the SHA-1 of the identical file in the vcpkg ports tree ($droppedRecipeFiles left out because that file differs or is missing). Licence conclusions follow installer/license-policy.json (ADR 0003, ADR 0009)." }
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
        $pattern = "(?<=[A-Za-z]$($nul):$nul|[\\/]$nul[A-Za-z]$nul)$separator$(& $word 'Users')$separator$excluded" +
                   "(?:[^\\/\x00-\x1F:*?`"<>|]$nul){1,64}[\\/]$nul"
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

function Invoke-BuilderPathAudit {
    <#
    .SYNOPSIS
        Scans every byte of every file below -Root (or the single file -Path, or the text -Text as
        UTF-8) for the build machine's folders (New-BuilderPathScanner), whatever the file type:
        the PDB paths in PE debug directories, __FILE__ strings, licence texts, the SBOM, QML type
        information. Returns a report; each hit becomes an error naming the file and the byte offset.
    #>
    param([Parameter(Mandatory)]$Scanner, [string]$Root, [string]$Path, [string]$Text, [string]$Name, [string]$Label = 'Scan')
    $hits = New-Object System.Collections.Generic.List[object]
    $files = 0
    $bytes = [int64]0
    $timer = [System.Diagnostics.Stopwatch]::StartNew()
    if ($Root) {
        # -Force: hidden and system files are scanned too.
        foreach ($file in (Get-ChildItem -LiteralPath $Root -Recurse -File -Force | Sort-Object FullName)) {
            $content = [System.IO.File]::ReadAllBytes($file.FullName)
            foreach ($hit in (Find-BuilderPathsInBytes -Scanner $Scanner -Bytes $content -Name (Get-RelativePathText -Root $Root -Path $file.FullName))) { $hits.Add($hit) }
            $files++
            $bytes += $content.Length
        }
    }
    if ($Path) {
        $content = [System.IO.File]::ReadAllBytes($Path)
        foreach ($hit in (Find-BuilderPathsInBytes -Scanner $Scanner -Bytes $content -Name $(if ($Name) { $Name } else { Split-Path -Leaf $Path }))) { $hits.Add($hit) }
        $files++
        $bytes += $content.Length
    }
    if ($PSBoundParameters.ContainsKey('Text')) {
        $content = (New-Object System.Text.UTF8Encoding($false)).GetBytes($Text)
        foreach ($hit in (Find-BuilderPathsInBytes -Scanner $Scanner -Bytes $content -Name $(if ($Name) { $Name } else { 'text' }))) { $hits.Add($hit) }
        $files++
        $bytes += $content.Length
    }
    $errors = New-Object System.Collections.Generic.List[string]
    foreach ($hit in $hits) {
        $errors.Add(('{0} at offset 0x{1:X8} ({2}, {3}) names the {4} {5}: {6}' -f $hit.File, $hit.Offset, $hit.Encoding, $hit.Location, $hit.Rule, $hit.Folder, $hit.Context))
    }
    return [pscustomobject]@{
        Label = $Label; Files = $files; Bytes = $bytes; Seconds = [Math]::Round($timer.Elapsed.TotalSeconds, 1)
        Hits = $hits.ToArray(); Errors = $errors.ToArray(); HitFiles = @($hits | ForEach-Object File | Sort-Object -Unique) }
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
    foreach ($report in $Reports) {
        $lines.Add(('{0}: {1} file(s), {2:N0} bytes, {3} s; {4} hit(s) in {5} file(s)' -f $report.Label, $report.Files, $report.Bytes, $report.Seconds, @($report.Hits).Count, @($report.HitFiles).Count))
        foreach ($problem in $report.Errors) { $lines.Add("  $problem") }
    }
    Write-TextFile -Path $TextPath -Text (($lines -join "`n") + "`n")
    Write-JsonFile -Path $JsonPath -Value ([ordered]@{
        folders = @($Scanner.Rules | ForEach-Object { [ordered]@{ name = $_.Name; path = $_.Path; driveOptional = $_.DriveOptional } })
        scans = @($Reports | ForEach-Object { [ordered]@{
            label = $_.Label; files = $_.Files; bytes = $_.Bytes; seconds = $_.Seconds
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

# --- Inventory -----------------------------------------------------------------------------------

function Get-FileInventory {
    param([Parameter(Mandatory)][string]$Root)
    return @(Get-ChildItem -LiteralPath $Root -Recurse -File | Sort-Object FullName | ForEach-Object {
        [ordered]@{ path = (Get-RelativePathText -Root $Root -Path $_.FullName); size = $_.Length; sha256 = (Get-Sha256 $_.FullName) }
    })
}

Export-ModuleMember -Function Read-LucentPackageInfo, Get-PeHeader, Get-PeDependencies, Get-VcpkgFileIndex,
    Get-StagedFileOrigins, Invoke-LayoutAudit, Write-AuditReport, Invoke-SmokeRun, Get-StaticVcpkgLibraries, Get-LucentExecutablesText,
    New-LicenseBundle, New-PackageSbom, Write-InstalledFileList, Test-InstalledFileList, Find-InnoSetupCompiler, Get-FileInventory,
    Write-TextFile, Write-JsonFile, Get-Sha256, Get-Sha1, Get-RelativePathText, Invoke-NativeCommand,
    Get-BuilderPathRules, New-BuilderPathScanner, Invoke-BuilderPathAudit, Write-BuilderPathReport,
    Get-CodeSigningCertificate, Test-AuthenticodeFile, Get-UpdateKeyId, Get-LucentUpdateChannelRecord,
    ConvertFrom-UpdateChannelLogLine, Format-UpdateChannel, Compare-UpdateChannel, Read-ExpectedUpdateChannels,
    Test-LucentUpdateChannelPolicy
