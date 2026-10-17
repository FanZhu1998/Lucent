<#
.SYNOPSIS
    Checks the corresponding-source package of the LGPL libraries (W12, ADR 0024). Without arguments,
    the packaging functions themselves on synthetic data (no build needed); with -PackageManifest, a
    real package's Lucent-<version>-corresponding-source.zip against its package manifest (a release
    gate, docs\release\checklist.md).

.DESCRIPTION
    Self-test (installer\scripts\LucentPackaging.psm1):
    1. New-DeterministicZip: the same entries given in another order give byte-identical archives.
       Read back with System.IO.Compression, the entries are sorted by name, stored, dated
       1980-02-01 00:00 and hold their contents; every local header has no flags, method 0 and no
       extra field. Names that are not plain relative ASCII paths, two names that differ only in
       case and an entry without content or source file are refused. CRC-32("123456789") is
       CBF43926.
    2. Test-CopyleftLicence on SPDX expressions: AND needs one copyleft operand, OR all of them,
       WITH keeps the licence's kind, parentheses group.
    3. Get-SourceProvisionPlan: LGPL ports are included, permissive ones are not; a port whose
       licence, a library linked statically from it or third-party code inside it (Qt's SBOM)
       leaves no choice but a copyleft licence is an error unless the policy provides for it
       ("lgpl" or "correspondingSource").
    4. Test-CorrespondingSourcePackage on synthetic packages: a complete one passes; a missing
       source archive, recipe file, MANIFEST.json component or build record, a changed recipe file,
       a wrong SHA256SUMS line, a compressed entry, entries out of order and another time each fail
       with their own message.
    5. ConvertFrom-SourcePackageRecord turns a package manifest's record into the same expectations
       as Get-SourceProvisionExpectation does from vcpkg's SPDX data.
    6. Get-RecipeCopy copies a recipe folder only as vcpkg built with it: a changed, missing or extra
       file is refused.
    7 to 10. Orders independent of the regional format, the patches a recipe applies, the code linked
       into the programs, and the Inno Setup compiler's licence (the W12 release review).
    11. What the gate-8 reading of 2026-10-16 found (D1 to D11): a part's terms are taken verbatim from
       the verified source archive (Get-LicenceExcerpt, Find-SourceArchiveMember, Format-PartLicenceText)
       and a compiled part's licence must be in the concluded expression (Test-PartLicences), which for
       LibRaw is "LGPL-2.1-only AND BSD-3-Clause AND MIT" and still needs its source provision; vcpkg's
       licence is the registry's LICENSE.txt at the baseline by its git blob (Get-VcpkgLicence, on a test
       registry) and ships in the archive; the recipe files and Lucent's own files get their licences
       in the SBOM (Get-RecipeFileLicence, Get-OwnFileLicence); THIRD-PARTY-NOTICES.txt names the
       redistributable's and Inno Setup's versions, the recipes' licences and every data asset
       (Complete-ThirdPartyNotices, Format-RecipeNotice, Format-DataNotices); a library's note names
       every patch that changes more than build files (Test-ModificationNotes).
    12. What the review of that reading found, round 1 (R1 to R14), and its sweep of every shipped
       component: a part is taken from the archive of its port that holds it (PCRE2's JIT compiler),
       its licence's text from the same archive or other components' files, a notice ending in a word
       its lines repeat; the parts of FreeType (R1), PCRE2 (R2), nlohmann-json (R3), Lensfun (R4),
       libtiff (R5), HarfBuzz, libjpeg-turbo and GNU libintl are refused under the licences the
       package had and accepted under the policy's; licences with exceptions and choices in
       expressions (Test-LicenceInExpression, Test-LicenceCovered); Qt's third-party code against a Qt
       component's licence (Test-AttributionLicences); the compiler's library code in the programs
       (Get-ToolchainLibraries: reviewed toolset, pinned header, probed table); Inno Setup's licence
       (Get-InnoSetupLicenceFile, R14); a recipe's scripted edits listed with its patches
       (Get-RecipeScripts, Test-ScriptedEdits, Get-RecipeModifications, R6); a registry port's recipe
       files' licence (R9); the notices' wording (R7, R11) and the notes read from a package (R12).
    13. What the gate-8 review, round 2, found (F1 to F21): each patch's licence from the files it
       changes, with Lucent's elections and the policy's rules for files without an SPDX line
       (Get-PatchLicences, Select-ElectedLicence, Test-PathPattern, Test-FileLicenceRules), and the
       GPL-2.0 text the archive needs for LibRaw-cmake (licenceTexts); files of published sources
       kept in installer\third-party, checked by their git blob ids (Get-ThirdPartySourceFile); Qt's
       records reviewed (Resolve-AttributionPolicy: a corrected licence with its evidence, NOTICE
       lines, the Source Code Form of copyleft code at the commit Qt's SBOM records); the Windows
       SDK's C++/WinRT in Qt's files (Get-WindowsSdkLibrary); the code of others inside Inno Setup
       with its licences and, under a copyleft licence, its source (Get-InnoSetupParts); each
       program's whole licence (Get-ProgramLicences); no part's licence text supplied by Lucent
       ("terms" refused); the counts of Qt Declarative's other files, the notices' and README's
       wording, the programs' and Setup's copyright lines and the redistributable's terms.

    With -PackageManifest <Lucent-Setup-<version>-x64.manifest.json> (schema 5): the archive beside it
    (or -SourcePackage) must have the size and SHA-256 the manifest records and hold exactly the files
    it lists; every component the manifest marks "correspondingSource" must be in the archive with its
    source archives (by SHA-512) and recipe files (by SHA-256), and vcpkg's and Lucent's licences beside
    them, and no component whose licence leaves no choice but a copyleft licence may lack the mark
    (Test-CorrespondingSourcePackage); each library's note names every patch that changes more than
    build files, the note as the package ships it (components[].note of the manifest, else the
    "Note:" lines of its THIRD-PARTY-NOTICES.txt; never the working tree's policy: the review of the
    gate-8 reading, round 1, R12), and GNU libintl's scripted edit is listed (R6). When the
    installation folder is beside the manifest (stage\, or -StageDir), its licences too:
    THIRD-PARTY-NOTICES.txt names the redistributable's and Inno Setup's versions the manifest records
    and every bundled asset; each library's parts (components[].parts, else the policy's for an older
    manifest) have their notices in licenses\<port>\ and their licences in its expression; the
    compiled-in code is named as part of both programs, and LGPL-NOTICE.txt says neither links LGPL
    code statically (R7); the wording R11 corrected; the compiler's library parts the manifest records
    ship with their pinned notices; vcpkg-LICENSE.txt is beside the shipped recipes; the SBOM concludes
    a licence for the recipe files, qt.conf and Lucent-files.txt, gives each component's port and
    binary package its licence, leaves NOASSERTION for licence texts and notices only and gives the
    port packages' recipe files their shipped copies' licences (R9), gives no part its library's
    version (R13), has an Inno Setup package with the recorded version whose licence ships (R14), and
    each Qt component's licence covers the third-party code Qt's SBOM records inside it. Since the
    gate-8 review, round 2: the Windows SDK's code the manifest records ships with its licence file
    and is a package of the binary that holds it; copyleft code inside Qt has its Source Code Form at
    a commit; Khronos' headers are MIT-Khronos-old and Tika's NOTICE lines ship; the redistributable's
    terms are named (and the owner's review of them is printed); every part inside Inno Setup has its
    file, notice and, under a copyleft licence, its source, and is a package Inno Setup contains; each
    patch's licence is the same in the archive's MANIFEST.json and the SBOM; each program's file
    carries its whole licence and its version resource names the logo's exception; Qt Declarative's
    QML files are counted; and the archive's README says where licence texts its archives lack are.

    Registered with CTest as lucent_source_package_tests (label "installer"). Exit code 0 when every
    check passed. Windows PowerShell 5.1 and PowerShell 7. Keep this file ASCII.

.EXAMPLE
    powershell -NoProfile -File installer\tests\Test-SourcePackage.ps1
.EXAMPLE
    powershell -NoProfile -File installer\tests\Test-SourcePackage.ps1 -PackageManifest build\release\package\Lucent-Setup-0.1.0-x64.manifest.json
.PARAMETER StageDir
    With -PackageManifest: the installation folder whose licences are checked (default: stage\ beside
    the manifest, skipped when it is not there).
#>
[CmdletBinding()]
param(
    [string]$PackageManifest,
    [string]$SourcePackage,
    [string]$StageDir
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Import-Module (Join-Path $repoRoot 'installer\scripts\LucentPackaging.psm1') -Force
Add-Type -AssemblyName System.IO.Compression

$script:checks = 0
$script:failures = 0
function Assert-True([bool]$Condition, [string]$What) {
    $script:checks++
    if (-not $Condition) { $script:failures++; Write-Host "FAILED: $What" }
}
function Assert-Throws([scriptblock]$Block, [string]$What) {
    $script:checks++
    try { & $Block; $script:failures++; Write-Host "FAILED (no error): $What" } catch { }
}
function Assert-Problem([object[]]$Errors, [string]$Pattern, [string]$What) {
    $script:checks++
    if (-not (@($Errors) | Where-Object { $_ -match $Pattern })) {
        $script:failures++
        Write-Host "FAILED: $What (no error matching '$Pattern'; errors: $(@($Errors) -join ' | '))"
    }
}
function Get-BytesSha([byte[]]$Bytes, [string]$Algorithm) {
    $hasher = [System.Security.Cryptography.HashAlgorithm]::Create($Algorithm)
    # The cast picks ComputeHash(byte[]) for an empty array too.
    try { return [System.BitConverter]::ToString($hasher.ComputeHash([byte[]]$Bytes)).Replace('-', '').ToLowerInvariant() } finally { $hasher.Dispose() }
}
function Get-NoticesBlock([string]$Text, [string]$Port) {
    # The lines of one component of a THIRD-PARTY-NOTICES.txt ("<port> <version>", "  Licence: ...", up to the
    # next empty line), whitespace folded, or '' (the gate-8 review, round 2).
    $lines = @($Text -split "`r?`n")
    for ($i = 0; $i -lt $lines.Count - 1; $i++) {
        if ($lines[$i] -match ('^' + [regex]::Escape($Port) + ' \S+$') -and $lines[$i + 1].StartsWith('  Licence: ')) {
            $end = $i
            while ($end -lt $lines.Count -and $lines[$end]) { $end++ }
            return ((@($lines[$i..($end - 1)]) -join ' ') -replace '\s+', ' ')
        }
    }
    return ''
}
function Get-NoticesNotes([string]$Text) {
    # Each component's "Note:" line of a THIRD-PARTY-NOTICES.txt, by its port: a component starts with a line
    # "<port> <version>" followed by "  Licence: ..." (W12 gate-8 review, round 1, R12: the notes a package
    # ships, not the working tree's).
    $notes = @{}
    $lines = @($Text -split "`r?`n")
    $current = $null
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^([a-z0-9][a-z0-9._+-]*) \S+$' -and $i + 1 -lt $lines.Count -and $lines[$i + 1].StartsWith('  Licence: ')) { $current = $Matches[1]; continue }
        if (-not $lines[$i]) { $current = $null; continue }
        if ($current -and $lines[$i].StartsWith('  Note: ')) { $notes[$current] = $lines[$i].Substring(8) }
    }
    return $notes
}

# --- A real package against its manifest ---------------------------------------------------------

if ($PackageManifest) {
    $manifestPath = (Resolve-Path -LiteralPath $PackageManifest).Path
    $manifest = [System.IO.File]::ReadAllText($manifestPath) | ConvertFrom-Json
    $names = @($manifest.PSObject.Properties.Name)
    if ($manifest.product -ne 'Lucent' -or $names -notcontains 'schema' -or [int]$manifest.schema -lt 5 -or $names -notcontains 'correspondingSource' -or -not $manifest.correspondingSource) {
        throw "$manifestPath is not a package manifest of schema 5 or later with a corresponding-source record (tools\package.ps1, ADR 0024)."
    }
    $record = $manifest.correspondingSource
    $zip = if ($SourcePackage) { (Resolve-Path -LiteralPath $SourcePackage).Path } else { Join-Path (Split-Path -Parent $manifestPath) $record.file }
    if (-not (Test-Path -LiteralPath $zip -PathType Leaf)) { throw "The corresponding-source archive $zip is missing." }
    $item = Get-Item -LiteralPath $zip
    Assert-True ($item.Name -eq $record.file) "the archive is named $($record.file) (found $($item.Name))"
    Assert-True ($item.Name -eq "Lucent-$($manifest.version)-corresponding-source.zip") "the archive's name carries the version $($manifest.version)"
    Assert-True ($item.Length -eq [int64]$record.size) "the archive has the recorded size $($record.size) ($($item.Length))"
    $sha = Get-Sha256 $zip
    Assert-True ($sha -eq $record.sha256) "the archive has the recorded SHA-256 $($record.sha256) ($sha)"
    $provided = @(@($record.components) | ForEach-Object { [string]$_.port })
    foreach ($component in @($manifest.components)) {
        $marked = ($component.PSObject.Properties.Name -contains 'correspondingSource') -and [bool]$component.correspondingSource
        if ($marked) { Assert-True ($provided -contains [string]$component.port) "$($component.port) is marked correspondingSource and recorded in the archive" }
        else { Assert-True (-not (Test-CopyleftLicence $component.licence)) "$($component.port) ($($component.licence)) needs no source provision" }
    }
    $expected = @(ConvertFrom-SourcePackageRecord -Record $record)
    $result = Test-CorrespondingSourcePackage -Path $zip -Version $manifest.version -Expected $expected
    foreach ($problem in $result.Errors) { Write-Host "    $problem" }
    Assert-True (@($result.Errors).Count -eq 0) 'the archive holds every recorded archive and recipe file, vcpkg''s and Lucent''s licences beside them, in its deterministic form'
    $listed = @{}
    foreach ($file in @($record.files)) { $listed[[string]$file.path] = $file }
    Assert-True (@($result.Entries).Count -eq $listed.Count) "the archive holds exactly the $($listed.Count) files the manifest lists ($(@($result.Entries).Count))"
    foreach ($entry in $result.Entries) {
        $file = $listed[$entry.Name]
        Assert-True ($null -ne $file -and [int64]$file.size -eq $entry.Length -and $file.sha256 -eq $entry.Sha256) "$($entry.Name) is listed with its size and SHA-256"
    }
    Write-Host ("{0}: {1:N1} MiB, {2} files, {3} components: {4}" -f $item.Name, ($item.Length / 1MB), @($result.Entries).Count, $provided.Count, ($provided -join ', '))

    # The licences, as the gate-8 reading of 2026-10-16 found them wanting: the libraries' notes name every
    # patch that changes more than build files (D6), and, in the installation folder beside the manifest
    # (or -StageDir), the notices name the recorded versions and every asset (D3, D4, D5), each library's
    # parts have their terms and their licences are in its expression (D1, D2), vcpkg's licence ships
    # beside its recipes (D9) and the SBOM concludes a licence for the recipes, qt.conf and
    # Lucent-files.txt (D9, D10).
    # The W12 gate-8 review, round 1 (R12): the notes and parts checked are the package's own, as its
    # manifest records them (components[].note, components[].parts) or, for a manifest that records no
    # notes, as its THIRD-PARTY-NOTICES.txt states them; never the working tree's policy.
    $stage = if ($StageDir) { (Resolve-Path -LiteralPath $StageDir).Path } else { Join-Path (Split-Path -Parent $manifestPath) 'stage' }
    $licencesDir = Join-Path $stage 'licenses'
    $noticesPath = Join-Path $licencesDir 'THIRD-PARTY-NOTICES.txt'
    $shippedNotices = if (Test-Path -LiteralPath $noticesPath -PathType Leaf) { [System.IO.File]::ReadAllText($noticesPath) } else { $null }
    $manifestComponents = @($manifest.components)
    $recordsNotes = @($manifestComponents | Where-Object { $_.PSObject.Properties.Name -contains 'note' }).Count -gt 0
    $notes = @{}
    if ($recordsNotes) { foreach ($component in $manifestComponents) { if ($component.note) { $notes[[string]$component.port] = [string]$component.note } } }
    elseif ($null -ne $shippedNotices) { $notes = Get-NoticesNotes -Text $shippedNotices }
    if ($result.Manifest -and ($result.Manifest.PSObject.Properties.Name -contains 'components')) {
        if (-not $recordsNotes -and $null -eq $shippedNotices) {
            Assert-True $false 'D6/R12: the libraries'' notes are checked as the package ships them, but neither the manifest (components[].note) nor an installation folder beside it records them'
        }
        else {
            $packagePolicy = [pscustomobject]@{ ports = [pscustomobject]@{} }
            foreach ($port in $notes.Keys) { $packagePolicy.ports | Add-Member -NotePropertyName $port -NotePropertyValue ([pscustomobject]@{ comment = $notes[$port] }) }
            $noteErrors = @(Test-ModificationNotes -Components @($result.Manifest.components) -Policy $packagePolicy -Where $(if ($recordsNotes) { 'the package manifest (components[].note)' } else { 'the package''s THIRD-PARTY-NOTICES.txt' }))
            foreach ($problem in $noteErrors) { Write-Host "    $problem" }
            Assert-True ($noteErrors.Count -eq 0) "D6/R12: each library's note, as the package $(if ($recordsNotes) { 'manifest records' } else { 'notices state' }) it, names every patch and script of its recipe that changes more than build files"
        }
        # R6: a scripted edit of a recipe is listed with its patches (GNU libintl's bashify.cmake).
        foreach ($component in @($result.Manifest.components | Where-Object { [string]$_.port -eq 'gettext-libintl' })) {
            $scripts = @(@($component.modifications) | Where-Object { $_ -and ($_.PSObject.Properties.Name -contains 'kind') -and $_.kind -eq 'script' } | ForEach-Object { [System.IO.Path]::GetFileName([string]$_.patch) })
            Assert-True ($scripts -contains 'bashify.cmake') "R6: MANIFEST.json lists gettext-libintl's bashify.cmake with its patches ($($scripts -join ', '))"
        }
    }
    if ($null -eq $shippedNotices) {
        if ($StageDir) { throw "$licencesDir holds no THIRD-PARTY-NOTICES.txt." }
        Write-Host "No installation folder beside the manifest ($stage): its licences are not checked."
    }
    else {
        $notices = $shippedNotices
        $flat = $notices -replace '\s+', ' '
        $policy = [System.IO.File]::ReadAllText((Join-Path $repoRoot 'installer\license-policy.json')) | ConvertFrom-Json
        $redistributable = "$($policy.vcRedistributable.name) $($manifest.vcRedistributable.version)"
        Assert-True ($flat.Contains("$redistributable (vc_redist.x64.exe)")) "D3: THIRD-PARTY-NOTICES.txt names $redistributable (vcRedistributable.version)"
        $inno = $null
        if (($names -contains 'toolchain') -and $manifest.toolchain -and ($manifest.toolchain.PSObject.Properties.Name -contains 'innoSetupVersion')) { $inno = $manifest.toolchain.innoSetupVersion }
        if ($inno) { Assert-True ($flat.Contains("are made with $inno, copyright")) "D4: THIRD-PARTY-NOTICES.txt names $inno (toolchain.innoSetupVersion)" }
        else { Assert-True ($flat.Contains('are made with Inno Setup, copyright')) 'D4: the manifest records no compiler, and THIRD-PARTY-NOTICES.txt names Inno Setup without a version' }
        foreach ($assetDir in @(Get-ChildItem -LiteralPath (Join-Path $stage 'assets') -Directory -ErrorAction SilentlyContinue)) {
            $assetVersion = ([System.IO.File]::ReadAllText((Join-Path $assetDir.FullName 'manifest.json')) | ConvertFrom-Json).version
            Assert-True ($flat.Contains(", asset $($assetDir.Name) $assetVersion, in assets\$($assetDir.Name)")) "D5: THIRD-PARTY-NOTICES.txt names the asset $($assetDir.Name) $assetVersion under Data"
        }
        # D1, D2 and the sweep of round 1: each component's parts, as the manifest records them (a manifest of
        # the gate-8 reading records none, and the working tree's policy is read for it), ship their notices,
        # their licences are in the component's, and the notices name them.
        $recordsParts = @($manifestComponents | Where-Object { $_.PSObject.Properties.Name -contains 'parts' }).Count -gt 0
        foreach ($component in $manifestComponents) {
            $parts = @()
            if ($recordsParts) {
                $parts = @(@($component.parts) | Where-Object { $_ } | ForEach-Object { [pscustomobject]@{ Name = [string]$_.name; Licence = [string]$_.licence
                    File = ([string]$_.file -replace '^licenses\\', ''); Compiled = [bool]$_.compiled; First = $null; Last = $null } })
            }
            else {
                $entry = $policy.ports.PSObject.Properties[[string]$component.port]
                if ($entry -and $entry.Value.PSObject.Properties.Name -contains 'parts') {
                    Write-Host "    $($component.port): the manifest records no parts; the working tree's policy names them (a package of the gate-8 reading)."
                    $parts = @(@($entry.Value.parts) | ForEach-Object { [pscustomobject]@{ Name = [string]$_.name; Licence = [string]$_.licence; File = "$($component.port)\$($_.as)"
                        Compiled = -not (($_.PSObject.Properties.Name -contains 'compiled') -and $_.compiled -eq $false); First = [string]$_.first; Last = [string]$_.last } })
                }
            }
            if ($parts.Count -eq 0) { continue }
            $outside = @($parts | Where-Object { $_.Compiled -and -not (Test-LicenceInExpression -Licence $_.Licence -Expression ([string]$component.licence)) } | ForEach-Object { "$($_.Name) ($($_.Licence))" })
            Assert-True ($outside.Count -eq 0) "D1: the licence of $($component.port), $($component.licence), names every compiled part's licence ($($outside -join '; '))"
            Assert-True ($notices -match ('(?m)^' + [regex]::Escape([string]$component.port) + ' .*\r?\n  Licence: ' + [regex]::Escape([string]$component.licence) + '\r?$')) "D1: THIRD-PARTY-NOTICES.txt gives $($component.port) as $($component.licence)"
            foreach ($part in $parts) {
                $partFile = Join-Path $licencesDir $part.File
                $partText = if (Test-Path -LiteralPath $partFile -PathType Leaf) { [System.IO.File]::ReadAllText($partFile) } else { '' }
                $marked = (-not $part.First -or $partText.Contains($part.First)) -and (-not $part.Last -or $partText.Contains($part.Last))
                Assert-True ($partText.Length -gt 0 -and $partText.Contains($part.Licence) -and $marked -and $flat.Contains("($($part.File))")) `
                    "D2: licenses\$($part.File) holds the notice of $($part.Name) ($($part.Licence)), and THIRD-PARTY-NOTICES.txt names it"
            }
        }
        # The round-1 findings, as the package states them (W12 gate-8 review, round 1).
        $programs = @(@($manifest.files) | ForEach-Object { [string]$_.path } | Where-Object { $_ -match '^[^\\]+\.exe$' })
        $programsText = if ($programs.Count -le 1) { $programs -join '' } else { (($programs[0..($programs.Count - 2)]) -join ', ') + ' and ' + $programs[-1] }
        Assert-True ($flat.Contains("whose code is part of $programsText;") -and -not $flat.Contains('whose code is part of Lucent.exe;')) "R7: THIRD-PARTY-NOTICES.txt says compiled-in code is part of $programsText"
        $lgplPath = Join-Path $licencesDir 'LGPL-NOTICE.txt'
        $lgplText = if (Test-Path -LiteralPath $lgplPath -PathType Leaf) { [System.IO.File]::ReadAllText($lgplPath) -replace '\s+', ' ' } else { '' }
        Assert-True ($lgplText.Contains("No LGPL-licensed code is linked statically into $programsText.")) "R7: LGPL-NOTICE.txt says no LGPL code is linked statically into $programsText"
        Assert-True (-not $flat.Contains('lib/compat.c added by') -and $flat -cnotmatch "with their terms from (its|[A-Za-z0-9-]+'s) source archive") 'R11: the notices neither read as Lucent adding lib/compat.c to libiconv nor claim every part''s terms come from its archive'
        # (Assigned, not an if-expression: Windows PowerShell 5.1 turns an empty array returned by one into $null.)
        $toolchainLibraries = @()
        if ($names -contains 'toolchainLibraries') { $toolchainLibraries = @($manifest.toolchainLibraries | Where-Object { $_ }) }
        foreach ($library in $toolchainLibraries) {
            $compiledParts = @(@($library.parts) | Where-Object { $_.compiled })
            if ($compiledParts.Count -eq 0) { continue }
            Assert-True ($notices -match ('(?m)^' + [regex]::Escape([string]$library.id) + ' ' + [regex]::Escape([string]$library.version) + '\r?\n  Licence: ' + [regex]::Escape([string]$library.licence) + '\r?$')) `
                "Sweep: THIRD-PARTY-NOTICES.txt lists the compiler's library $($library.id) $($library.version) with its licence"
            foreach ($part in $compiledParts) {
                $partFile = Join-Path $stage ([string]$part.file)
                $partText = if (Test-Path -LiteralPath $partFile -PathType Leaf) { [System.IO.File]::ReadAllText($partFile) } else { '' }
                Assert-True ($partText.Contains([string]$part.licence) -and $partText.Contains([string]$part.sha256) -and (Test-LicenceInExpression -Licence ([string]$part.licence) -Expression ([string]$library.licence))) `
                    "Sweep: $($part.file) holds the notice of $($part.name) from the reviewed header (SHA-256 $(([string]$part.sha256).Substring(0, 12))...), and $($library.id)'s licence names $($part.licence)"
            }
        }
        $recipeFolders = @(Get-ChildItem -LiteralPath $licencesDir -Directory | Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'recipe') -PathType Container } | ForEach-Object { "licenses\$($_.Name)\recipe" })
        if ($recipeFolders.Count -gt 0) {
            $vcpkgLicencePath = Join-Path $licencesDir 'vcpkg-LICENSE.txt'
            $vcpkgText = if (Test-Path -LiteralPath $vcpkgLicencePath -PathType Leaf) { [System.IO.File]::ReadAllText($vcpkgLicencePath) } else { '' }
            Assert-True ($vcpkgText.Contains('Permission is hereby granted') -and $vcpkgText.Contains('Copyright (c) Microsoft Corporation') -and $flat.Contains('vcpkg-LICENSE.txt')) `
                "D9: vcpkg's licence ships beside its recipes ($($recipeFolders -join ', ')), and THIRD-PARTY-NOTICES.txt names it"
        }
        $sbom = [System.IO.File]::ReadAllText((Join-Path $licencesDir 'Lucent.spdx.json')) | ConvertFrom-Json
        $checked = @($sbom.files | Where-Object { $_.SPDXID -like 'SPDXRef-Lucent-file-*' -and ($_.fileName -match '^\./licenses/[^/]+/recipe/' -or $_.fileName -in './qt.conf', './Lucent-files.txt') })
        $unasserted = @($checked | Where-Object { $_.licenseConcluded -eq 'NOASSERTION' } | ForEach-Object { $_.fileName })
        $recipeCount = @($checked | Where-Object { $_.fileName -match '/recipe/' }).Count
        Assert-True ($checked.Count -ge 2 -and $unasserted.Count -eq 0) "D9/D10: the SBOM concludes a licence for the $recipeCount shipped recipe files, qt.conf and Lucent-files.txt (NOASSERTION: $($unasserted -join ', '))"
        $notMit = @($checked | Where-Object { $_.fileName -match '/recipe/' -and $_.licenseConcluded -notmatch '^MIT( AND |$)' } | ForEach-Object { "$($_.fileName): $($_.licenseConcluded)" })
        Assert-True ($notMit.Count -eq 0) "D9: every shipped recipe file is MIT, a patch also under its library's licence ($($notMit -join '; '))"
        $sbomMismatch = @(foreach ($component in @($manifest.components)) {
            $prefix = 'SPDXRef-' + ([string]$component.port -replace '[^A-Za-z0-9.-]', '-') + '-'
            foreach ($kind in 'port', 'binary') {
                $package = @($sbom.packages | Where-Object { $_.SPDXID -ceq ($prefix + $kind) }) | Select-Object -First 1
                if ($package -and $package.licenseConcluded -cne $component.licence) { "$($package.SPDXID) $($package.licenseConcluded)" }
            } })
        Assert-True ($sbomMismatch.Count -eq 0) "D1: the SBOM's port and binary packages carry each component's licence ($($sbomMismatch -join '; '))"
        # R9: NOASSERTION is left for the licence texts and notices among Lucent's files only; every file of
        # vcpkg's port packages has a licence, and a recipe file Lucent ships has the same in both places.
        $noAssertion = @($sbom.files | Where-Object { $_.licenseConcluded -eq 'NOASSERTION' })
        # (Since the gate-8 review, round 2, also a patch that quotes a file whose licence its archive states
        # nowhere, which the SBOM's comment says: Lensfun's fix_build.patch.)
        $unstatedPatches = @($noAssertion | Where-Object { $_.SPDXID -match '-port-file-' -and $_.fileName -match '\.(patch|diff)$' -and ($_.PSObject.Properties.Name -contains 'comment') -and ([string]$_.comment).Contains('states nowhere') })
        $outsideTexts = @($noAssertion | Where-Object { ($_.SPDXID -notlike 'SPDXRef-Lucent-file-*' -or $_.fileName -notmatch '^\./licenses/' -or $_.fileName -match '/recipe/') -and $unstatedPatches -notcontains $_ } | ForEach-Object { "$($_.SPDXID) $($_.fileName)" })
        Assert-True ($outsideTexts.Count -eq 0) "R9: NOASSERTION is left for the $($noAssertion.Count - $unstatedPatches.Count) licence texts and notices only, and for $($unstatedPatches.Count) patch(es) quoting lines whose licence their archive states nowhere ($(@($outsideTexts | Select-Object -First 5) -join '; '))"
        $shippedRecipe = @{}
        foreach ($file in @($sbom.files | Where-Object { $_.SPDXID -like 'SPDXRef-Lucent-file-*' -and $_.fileName -match '^\./licenses/([^/]+)/recipe/(.+)$' })) {
            $null = $file.fileName -match '^\./licenses/([^/]+)/recipe/(.+)$'
            $shippedRecipe["$($Matches[1])|./$($Matches[2])"] = [string]$file.licenseConcluded
        }
        $recipeMismatch = @(foreach ($file in @($sbom.files | Where-Object { $_.SPDXID -match '^SPDXRef-(.+)-port-file-' })) {
            $null = $file.SPDXID -match '^SPDXRef-(.+)-port-file-'
            $key = "$($Matches[1])|$($file.fileName)"
            if ($shippedRecipe.ContainsKey($key) -and $shippedRecipe[$key] -cne [string]$file.licenseConcluded) { "$key ($($file.licenseConcluded) / $($shippedRecipe[$key]))" } })
        Assert-True ($shippedRecipe.Count -gt 0 -and $recipeMismatch.Count -eq 0) "R9: the port packages' recipe files have the licences of their shipped copies ($($recipeMismatch -join '; '))"
        $repeated = @($sbom.files | Where-Object { [string]$_.licenseConcluded -cnotmatch '\sOR\s' -and @(@(([string]$_.licenseConcluded -replace '[()]', '') -csplit '\s+AND\s+') | Group-Object | Where-Object { $_.Count -gt 1 }).Count -gt 0 } | ForEach-Object { "$($_.fileName): $($_.licenseConcluded)" })
        Assert-True ($repeated.Count -eq 0) "R9: no file's licence in the SBOM names a licence twice (a patch of an MIT library is MIT) ($(@($repeated | Select-Object -First 3) -join '; '))"
        # R13: a part's package has its own version or none, never its library's (unless they are the same).
        $versions = @{}
        foreach ($component in $manifestComponents) { $versions['SPDXRef-' + ([string]$component.port -replace '[^A-Za-z0-9.-]', '-') + '-part-'] = [string]$component.version }
        $borrowed = @(foreach ($package in @($sbom.packages | Where-Object { $_.SPDXID -match '-part-' })) {
            foreach ($prefix in $versions.Keys) {
                if ($package.SPDXID.StartsWith($prefix) -and ($package.PSObject.Properties.Name -contains 'versionInfo') -and [string]$package.versionInfo -ceq $versions[$prefix]) { "$($package.name) $($package.versionInfo)" }
            } })
        Assert-True ($borrowed.Count -eq 0) "R13: no part's SBOM package carries its library's version ($($borrowed -join '; '))"
        # R14: Inno Setup, whose loader is in Setup and whose uninstaller Setup installs, is a package of the
        # SBOM with the recorded version, and its licence ships when a compiler made the package.
        $innoPackage = @($sbom.packages | Where-Object { $_.SPDXID -ceq 'SPDXRef-InnoSetup' }) | Select-Object -First 1
        $innoNumber = if ($inno -and $inno -match '(\d+\.\d+(\.\d+)*)') { $Matches[1] } else { $null }
        Assert-True ($null -ne $innoPackage -and [string]$innoPackage.licenseConcluded -ceq 'InnoSetup' -and
                     ((-not $innoNumber -and -not ($innoPackage.PSObject.Properties.Name -contains 'versionInfo')) -or ($innoPackage.PSObject.Properties.Name -contains 'versionInfo' -and [string]$innoPackage.versionInfo -ceq $innoNumber))) `
            "R14: the SBOM has an Inno Setup package with the version the manifest records ($inno) and its licence"
        $innoFile = if (($names -contains 'toolchain') -and ($manifest.toolchain.PSObject.Properties.Name -contains 'innoSetupLicenceFile')) { $manifest.toolchain.innoSetupLicenceFile } else { $null }
        if ($innoFile) {
            $innoPath = Join-Path $stage ([string]$innoFile.file)
            Assert-True ((Test-Path -LiteralPath $innoPath -PathType Leaf) -and (Get-Sha256 $innoPath) -eq [string]$innoFile.sha256 -and $flat.Contains('inno-setup\license.txt')) `
                "R14: $($innoFile.file) is the compiler's license.txt with the recorded SHA-256, and THIRD-PARTY-NOTICES.txt names it"
        }
        elseif ($inno) { Assert-True $false "R14: the manifest records Inno Setup $inno but no licence file of it (toolchain.innoSetupLicenceFile)" }
        # The sweep of round 1: the third-party code Qt's SBOM records inside a Qt component's files is covered
        # by the component's licence.
        foreach ($component in @($manifestComponents | Where-Object { [string]$_.port -like 'qt*' })) {
            $prefix = 'SPDXRef-' + ([string]$component.port -replace '[^A-Za-z0-9.-]', '-') + '-qt-'
            $uncovered = @($sbom.packages | Where-Object { $_.SPDXID.StartsWith($prefix) -and -not (Test-LicenceCovered -Licence ([string]$_.licenseConcluded) -Expression ([string]$component.licence)) } | ForEach-Object { "$($_.name) ($($_.licenseConcluded))" })
            Assert-True ($uncovered.Count -eq 0) "Sweep: the licence of $($component.port) covers the third-party code Qt's SBOM records inside it ($($uncovered -join '; '))"
        }

        # The gate-8 review, round 2, as the package states it.
        $relationships = @($sbom.relationships)
        # Code of a Windows SDK library inside a component's files (C++/WinRT in Qt6Core.dll and qwindows.dll): its
        # licence ships, the notices name it with the component, the SBOM's package is inside the component's.
        $sdkLibraries = @($toolchainLibraries | Where-Object { ($_.PSObject.Properties.Name -contains 'binaries') -and @($_.binaries).Count -gt 0 })
        if (@($toolchainLibraries).Count -gt 0) { Assert-True ($sdkLibraries.Count -gt 0) "Round 2: the manifest records the Windows SDK's code inside the shipped binaries (C++/WinRT: toolchainLibraries[].binaries)" }
        foreach ($library in $sdkLibraries) {
            $licencePath = Join-Path $stage ([string]$library.licenceFile.file)
            Assert-True ((Test-Path -LiteralPath $licencePath -PathType Leaf) -and (Get-Sha256 $licencePath) -eq [string]$library.licenceFile.sha256) "Round 2: $($library.licenceFile.file), the licence of $($library.name), ships with the recorded SHA-256"
            $libraryId = 'SPDXRef-toolchain-' + ([string]$library.id -replace '[^A-Za-z0-9.-]', '-')
            foreach ($place in @($library.inside)) {
                $block = Get-NoticesBlock $notices ([string]$place.port)
                $licenceName = ([string]$library.licenceFile.file) -replace '^licenses\\', ''
                Assert-True ($block.Contains("$($library.name) $($library.version), in") -and $block.Contains($licenceName)) "Round 2: THIRD-PARTY-NOTICES.txt names $($library.name) $($library.version) inside the files of $($place.port) ($(@($place.files) -join ', '))"
                $binaryId = 'SPDXRef-' + ([string]$place.port -replace '[^A-Za-z0-9.-]', '-') + '-binary'
                Assert-True (@($relationships | Where-Object { $_.spdxElementId -ceq $binaryId -and $_.relationshipType -ceq 'CONTAINS' -and $_.relatedSpdxElement -ceq $libraryId }).Count -eq 1) "Round 2: the SBOM's package of $($library.name) is inside $($place.port)'s binary package"
            }
        }
        # Qt's records reviewed: the Source Code Form of copyleft code at its commit (the Public Suffix List),
        # Khronos' licence, Tika's NOTICE lines.
        foreach ($package in @($sbom.packages | Where-Object { $_.SPDXID -match '-qt-' -and (Test-CopyleftLicence ([string]$_.licenseConcluded)) })) {
            $commit = if ([string]$package.downloadLocation -match '^git\+https://\S+@([0-9a-f]{40})$') { $Matches[1] } else { $null }
            Assert-True ($commit -and $flat.Contains("at commit $commit") -and ($package.PSObject.Properties.Name -contains 'sourceInfo')) "Round 2: the Source Code Form of $($package.name) ($($package.licenseConcluded)) is named at its commit in the SBOM and THIRD-PARTY-NOTICES.txt ($($package.downloadLocation))"
        }
        $khronos = @($sbom.packages | Where-Object { $_.SPDXID -match '-qt-' -and $_.name -match '^OpenGL' })
        if ($khronos.Count -gt 0) { Assert-True (@($khronos | Where-Object { $_.licenseConcluded -cne 'MIT-Khronos-old' }).Count -eq 0) "Round 2: Khronos' OpenGL headers in Qt are MIT-Khronos-old, not Qt's MIT ($(@($khronos | ForEach-Object { "$($_.name): $($_.licenseConcluded)" }) -join '; '))" }
        foreach ($noticeName in @([regex]::Matches($notices, '(?m)^\s+NOTICE: (\S+)\s*$') | ForEach-Object { $_.Groups[1].Value })) {
            $noticePath = Join-Path $licencesDir $noticeName
            Assert-True ((Test-Path -LiteralPath $noticePath -PathType Leaf) -and [System.IO.File]::ReadAllText($noticePath).Contains('verbatim')) "Round 2: the NOTICE lines THIRD-PARTY-NOTICES.txt names ship ($noticeName)"
        }
        # Microsoft's terms for the redistributable Setup runs silently.
        $vcExtracted = @($sbom.hasExtractedLicensingInfos | Where-Object { $_.licenseId -ceq 'LicenseRef-Microsoft-VC-Redistributable' }) | Select-Object -First 1
        $vcRecord = $manifest.vcRedistributable
        Assert-True ($null -ne $vcExtracted -and -not ([string]$vcExtracted.extractedText).Contains('The terms are shown by the redistributable installer') -and
                     ($vcRecord.PSObject.Properties.Name -contains 'licenceTerms') -and $flat.Contains([string]$vcRecord.licenceTerms)) "Round 2: the redistributable's terms are named truthfully in the SBOM and THIRD-PARTY-NOTICES.txt ($(if ($vcRecord.PSObject.Properties.Name -contains 'licenceTerms') { $vcRecord.licenceTerms }))"
        if ($vcRecord.PSObject.Properties.Name -contains 'termsReview') { Write-Host "    The owner's review of the redistributable's terms, as the manifest records it: $($vcRecord.termsReview)" }
        # The code of others inside Inno Setup, each with its licence's file, its notice and (copyleft) its source.
        $innoParts = @()
        if (($names -contains 'toolchain') -and ($manifest.toolchain.PSObject.Properties.Name -contains 'innoSetupParts')) { $innoParts = @($manifest.toolchain.innoSetupParts | Where-Object { $_ }) }
        if ($inno) { Assert-True ($innoParts.Count -gt 0) "Round 2: the manifest records the code of others inside Inno Setup (toolchain.innoSetupParts)" }
        foreach ($part in $innoParts) {
            $partPath = Join-Path $stage ([string]$part.file)
            $sources = @(@($part.sourceFiles) | Where-Object { $_ } | Where-Object { -not (Test-Path -LiteralPath (Join-Path $stage ([string]$_)) -PathType Leaf) })
            $partName = ([string]$part.file) -replace '^licenses\\', ''
            Assert-True ((Test-Path -LiteralPath $partPath -PathType Leaf) -and $flat.Contains($partName) -and (-not $part.notice -or $flat.Contains([string]$part.notice)) -and $sources.Count -eq 0 -and
                         (-not (Test-CopyleftLicence ([string]$part.licence)) -or @($part.sourceFiles).Count -gt 0)) "Round 2: $($part.name) inside Inno Setup ($($part.licence)) has its licence's file, its notice and, under a copyleft licence, its source in the installation"
        }
        if ($innoParts.Count -gt 0) {
            Assert-True (@($relationships | Where-Object { $_.spdxElementId -ceq 'SPDXRef-InnoSetup' -and $_.relationshipType -ceq 'CONTAINS' }).Count -eq $innoParts.Count) "Round 2: the SBOM's Inno Setup package contains its $($innoParts.Count) parts"
        }
        # Each patch's licence is that of the files it changes: the archive's MANIFEST.json and the SBOM agree, and
        # the patches this review found are no longer their library's licence.
        $patchLicences = @{}
        foreach ($component in @(if ($result.Manifest -and ($result.Manifest.PSObject.Properties.Name -contains 'components')) { $result.Manifest.components })) {
            foreach ($modification in @(if ($component.PSObject.Properties.Name -contains 'modifications') { $component.modifications })) {
                if ($modification -and ($modification.PSObject.Properties.Name -contains 'licence') -and $modification.kind -eq 'patch') {
                    $patchLicences["$($component.port)|./$(([string]$modification.patch).Substring(('ports/' + $component.port + '/').Length))"] = [string]$modification.licence
                }
            }
        }
        Assert-True ($patchLicences.Count -gt 0) "Round 2: MANIFEST.json gives each patch its licence ($($patchLicences.Count))"
        $patchMismatch = @(foreach ($file in @($sbom.files | Where-Object { $_.SPDXID -match '^SPDXRef-(.+)-port-file-' -and $_.fileName -match '\.(patch|diff)$' })) {
            $null = $file.SPDXID -match '^SPDXRef-(.+)-port-file-'
            $key = "$($Matches[1])|$($file.fileName)"
            if ($patchLicences.ContainsKey($key) -and $patchLicences[$key] -cne [string]$file.licenseConcluded) { "$key ($($file.licenseConcluded) / $($patchLicences[$key]))" } })
        Assert-True ($patchMismatch.Count -eq 0) "Round 2: each patch has the same licence in the SBOM as in MANIFEST.json ($($patchMismatch -join '; '))"
        $expectedPatches = [ordered]@{ 'qtbase|./macdeployqt-symlinks.patch' = 'GPL-3.0-only WITH Qt-GPL-exception-1.0'; 'qtbase|./windeployqt-webengine-debug-resources.patch' = 'GPL-3.0-only WITH Qt-GPL-exception-1.0'
            'libraw|./dependencies.patch' = 'GPL-2.0-or-later'; 'libraw|./fix-install.patch' = 'GPL-2.0-or-later' }
        foreach ($case in $expectedPatches.GetEnumerator()) {
            if ($patchLicences.ContainsKey($case.Key)) { Assert-True ((Test-LicenceInExpression -Licence $case.Value -Expression $patchLicences[$case.Key]) -and -not (Test-LicenceInExpression -Licence 'LGPL-3.0-only' -Expression $patchLicences[$case.Key]) -and -not (Test-LicenceInExpression -Licence 'LGPL-2.1-only' -Expression $patchLicences[$case.Key])) "Round 2: $($case.Key) quotes lines under $($case.Value), not its library's licence ($($patchLicences[$case.Key]))" }
        }
        if ($patchLicences.ContainsKey('lensfun|./fix_build.patch')) { Assert-True ($patchLicences['lensfun|./fix_build.patch'] -ceq 'NOASSERTION') "Round 2: Lensfun's fix_build.patch, which quotes the CMakeLists.txt whose licence the archive states nowhere, makes no assertion ($($patchLicences['lensfun|./fix_build.patch']))" }
        # Lucent's programs carry the licences of the code compiled or linked into them.
        foreach ($program in $programs) {
            $file = @($sbom.files | Where-Object { $_.SPDXID -like 'SPDXRef-Lucent-file-*' -and $_.fileName -ceq "./$program" }) | Select-Object -First 1
            $terms = @()
            if ($file) { $terms = @(Split-LicenceConjunction ([string]$file.licenseConcluded)) }
            $wanted = @(foreach ($component in @($manifestComponents | Where-Object { $_.linkage -eq 'compiled-in' })) { Split-LicenceConjunction ([string]$component.licence) }) +
                      @(foreach ($static in @(if ($names -contains 'staticLibraries') { $manifest.staticLibraries })) { if ($program -ieq 'Lucent.exe') { Split-LicenceConjunction ([string]$static.licence) } }) +
                      @(foreach ($library in @($toolchainLibraries | Where-Object { ($_.PSObject.Properties.Name -contains 'programs') -and @($_.programs) -contains $program })) { Split-LicenceConjunction ([string]$library.licence) })
            $missing = @($wanted | Where-Object { $terms -cnotcontains $_ } | Select-Object -Unique)
            Assert-True ($null -ne $file -and $terms -ccontains 'MIT' -and @($terms | Where-Object { $_ -like 'LicenseRef-*' }).Count -gt 0 -and $missing.Count -eq 0) "Round 2: the SBOM's $program carries MIT, the logo's terms and the licences of the code compiled into it ($(if ($file) { $file.licenseConcluded }); missing: $($missing -join ', '))"
            $info = [System.Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $stage $program))
            Assert-True ([string]$info.LegalCopyright -ceq 'Copyright (c) 2026 Fan Zhu. MIT License.') "$program's copyright line is the one the owner kept on 2026-10-15; the notices state the exceptions ('$($info.LegalCopyright)')"
        }
        # The notices' wording: the files of Qt Declarative counted by kind, the sentence on copyright.txt limited to
        # vcpkg's components, no licence words Lucent supplies, the SIMD holders, the Adobe code's MIT text.
        # (Not -Filter '*.qml': Windows matches a three-letter extension's longer ones too, .qmltypes.)
        $qmlCount = @(Get-ChildItem -LiteralPath (Join-Path $stage 'qml') -Recurse -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '\.qml$' }).Count
        Assert-True (-not $flat.Contains('(QML module definitions and type information)') -and ($qmlCount -eq 0 -or $flat.Contains("$qmlCount QML source files (.qml)"))) "Round 2: THIRD-PARTY-NOTICES.txt counts the $qmlCount QML source files among Qt Declarative's files"
        Assert-True ($flat.Contains('The full copyright and licence text of each vcpkg component is in the folder named after it') -and -not $flat.Contains("standard words")) 'Round 2: the notices limit the sentence on copyright.txt to vcpkg components and name no licence words Lucent supplies'
        $adobePath = Join-Path $licencesDir 'libraw\Adobe-DNG-SDK-MIT.txt'
        if (Test-Path -LiteralPath $adobePath -PathType Leaf) {
            $adobeText = [System.IO.File]::ReadAllText($adobePath) -replace '\s+', ' '
            Assert-True (-not $adobeText.Contains('Lucent supplies') -and $adobeText.Contains('nlohmann-json\copyright.txt')) 'Round 2: the Adobe DNG SDK code''s MIT text is a verified source''s (nlohmann-json''s licence), not Lucent''s words'
        }
        $simdBlock = Get-NoticesBlock $notices 'libjpeg-turbo'
        if ($simdBlock) { Assert-True ($simdBlock.Contains('Intel Corporation') -and $simdBlock.Contains('Institute of Software, Chinese Academy of Sciences')) 'Round 2: the SIMD part names the holders of the compiled SIMD files' }
        # The archive's README says where a library's licence text is when its archive lacks it.
        $readmeName = "$($record.root)/README.txt"
        $zipStream = [System.IO.File]::OpenRead($zip)
        try {
            $zipArchive = New-Object System.IO.Compression.ZipArchive($zipStream, [System.IO.Compression.ZipArchiveMode]::Read)
            $readmeEntry = $zipArchive.GetEntry($readmeName)
            $readmeText = ''
            if ($readmeEntry) { $reader = New-Object System.IO.StreamReader($readmeEntry.Open()); try { $readmeText = $reader.ReadToEnd() } finally { $reader.Dispose() } }
            $zipArchive.Dispose()
        }
        finally { $zipStream.Dispose() }
        $flatArchiveReadme = $readmeText -replace '\s+', ' '
        Assert-True ($flatArchiveReadme.Length -gt 0 -and -not $flatArchiveReadme.Contains("its texts are in its source archive, and in licenses\<port>\") -and $flatArchiveReadme.Contains('Its texts are in its source archive where that archive holds them.')) `
            'Round 2: the archive''s README no longer says every licence text is in its library''s archive'
    }
    Write-Host "$($script:checks) checks, $($script:failures) failed"
    if ($script:failures -gt 0) { exit 1 }
    exit 0
}

# --- Self-test -----------------------------------------------------------------------------------

$work = Join-Path ([System.IO.Path]::GetTempPath()) ('lucent-source-package-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
$utf8 = New-Object System.Text.UTF8Encoding($false)
try {
    # 1. The deterministic archive.
    $big = Join-Path $work 'big.bin'
    $bytes = New-Object byte[] (3MB + 17)
    (New-Object System.Random 7).NextBytes($bytes)
    [System.IO.File]::WriteAllBytes($big, $bytes)
    $entries = @(
        [pscustomobject]@{ Name = 'root/b/second.txt'; Content = $utf8.GetBytes("second`n") },
        [pscustomobject]@{ Name = 'root/a.txt'; Content = $utf8.GetBytes('first') },
        [pscustomobject]@{ Name = 'root/sources/big.bin'; SourcePath = $big },
        [pscustomobject]@{ Name = 'root/B.txt'; Content = $utf8.GetBytes('upper case sorts first') })
    $one = Join-Path $work 'one.zip'
    $two = Join-Path $work 'two.zip'
    New-DeterministicZip -Path $one -Entries $entries
    [array]::Reverse($entries)
    Start-Sleep -Milliseconds 1100   # another wall-clock second must not change the bytes
    New-DeterministicZip -Path $two -Entries $entries
    Assert-True ((Get-Sha256 $one) -eq (Get-Sha256 $two)) 'the same entries in another order, a second later, give byte-identical archives'
    Assert-True (-not (Test-Path -LiteralPath "$one.partial")) 'no partial file is left behind'
    $stream = [System.IO.File]::OpenRead($one)
    try {
        $zip = New-Object System.IO.Compression.ZipArchive($stream, [System.IO.Compression.ZipArchiveMode]::Read)
        $names = @($zip.Entries | ForEach-Object FullName)
        Assert-True (($names -join '|') -ceq 'root/B.txt|root/a.txt|root/b/second.txt|root/sources/big.bin') "entries sorted by name, ordinal: $($names -join ', ')"
        foreach ($entry in $zip.Entries) {
            Assert-True ($entry.CompressedLength -eq $entry.Length) "$($entry.FullName) is stored"
            Assert-True ($entry.LastWriteTime.DateTime -eq (New-Object DateTime(1980, 2, 1, 0, 0, 0))) "$($entry.FullName) is dated 1980-02-01 00:00 ($($entry.LastWriteTime))"
        }
        $read = $zip.GetEntry('root/sources/big.bin').Open()
        $copy = New-Object System.IO.MemoryStream
        try { $read.CopyTo($copy) } finally { $read.Dispose() }
        Assert-True ((Get-BytesSha $copy.ToArray() 'SHA256') -eq (Get-BytesSha $bytes 'SHA256')) 'a file entry holds the file byte for byte'
        $read = $zip.GetEntry('root/b/second.txt').Open()
        $text = (New-Object System.IO.StreamReader($read)).ReadToEnd()
        $read.Dispose()
        Assert-True ($text -ceq "second`n") 'a content entry holds its bytes'
        $zip.Dispose()
    }
    finally { $stream.Dispose() }
    $raw = [System.IO.File]::ReadAllBytes($one)
    $offset = 0
    $headers = 0
    while ($offset + 30 -le $raw.Length -and [System.BitConverter]::ToUInt32($raw, $offset) -eq 0x04034B50) {
        $headers++
        Assert-True ([System.BitConverter]::ToUInt16($raw, $offset + 6) -eq 0) "local header $headers has no flags (no data descriptor)"
        Assert-True ([System.BitConverter]::ToUInt16($raw, $offset + 8) -eq 0) "local header $headers uses method 0 (stored)"
        Assert-True ([System.BitConverter]::ToUInt16($raw, $offset + 28) -eq 0) "local header $headers has no extra field"
        $offset += 30 + [System.BitConverter]::ToUInt16($raw, $offset + 26) + [System.BitConverter]::ToUInt32($raw, $offset + 18)
    }
    Assert-True ($headers -eq 4) "four local headers ($headers)"
    $crc = [LucentPackaging.StoredZip]::Crc32([System.Text.Encoding]::ASCII.GetBytes('123456789'))
    Assert-True (('{0:X8}' -f $crc) -eq 'CBF43926') ('CRC-32 check value ({0:X8})' -f $crc)
    $bad = Join-Path $work 'bad.zip'
    foreach ($name in 'root\a.txt', '/root/a.txt', 'root/../a.txt', 'root/./a.txt', 'root//a.txt', 'root/a/', 'C:/root/a.txt', ('root/caf' + [char]0xE9)) {
        Assert-Throws { New-DeterministicZip -Path $bad -Entries @([pscustomobject]@{ Name = $name; Content = $utf8.GetBytes('x') }) } "the entry name '$name' is refused"
    }
    Assert-Throws { New-DeterministicZip -Path $bad -Entries @([pscustomobject]@{ Name = 'root/A.txt'; Content = $utf8.GetBytes('x') }, [pscustomobject]@{ Name = 'root/a.txt'; Content = $utf8.GetBytes('y') }) } 'names that differ only in case are refused'
    Assert-Throws { New-DeterministicZip -Path $bad -Entries @([pscustomobject]@{ Name = 'root/a.txt'; SourcePath = (Join-Path $work 'missing.bin') }) } 'an entry without content or source file is refused'
    Assert-True (-not (Test-Path -LiteralPath $bad)) 'a refused archive is not written'

    # 2. Copyleft licence expressions.
    $cases = [ordered]@{
        'LGPL-2.1-only' = $true; 'LGPL-3.0-only' = $true; 'GPL-2.0-or-later' = $true; 'MPL-2.0' = $true; 'CC-BY-SA-3.0' = $true
        'LGPL-2.1-only AND BSD-3-Clause' = $true; 'MIT AND MIT-Modern-Variant' = $false; 'MIT' = $false; 'FTL' = $false
        'FTL OR GPL-2.0-or-later' = $false; 'LicenseRef-Qt-Commercial OR BSD-3-Clause' = $false; 'CC0-1.0 OR Apache-2.0' = $false
        'BSD-3-Clause WITH PCRE2-exception' = $false; 'GPL-3.0-only WITH Qt-GPL-exception-1.0' = $true
        'LGPL-2.1-or-later OR MPL-1.1' = $true; '(MIT OR LGPL-2.1-or-later) AND MPL-2.0' = $true
        '(MIT OR LGPL-2.1-or-later) AND Zlib' = $false; 'LGPL-2.1-or-later AND (LGPL-2.1-only OR MPL-1.1)' = $true
        'NOASSERTION' = $false; '' = $false; 'blessing' = $false; 'libpng-2.0' = $false; 'X11 AND HPND' = $false }
    foreach ($case in $cases.GetEnumerator()) {
        Assert-True ((Test-CopyleftLicence $case.Key) -eq $case.Value) "Test-CopyleftLicence '$($case.Key)' is $($case.Value)"
    }
    Assert-Throws { Test-CopyleftLicence '(MIT OR GPL-2.0-only' } 'unbalanced parentheses are refused'
    Assert-Throws { Test-CopyleftLicence 'MIT OR' } 'an incomplete expression is refused'
    # A LicenseRef- alternative (such as a vendor's commercial licence, which Lucent does not hold) or an
    # unknown identifier is no alternative Lucent can take (W12 review LIC-4).
    $kinds = [ordered]@{
        'LicenseRef-Qt-Commercial OR LGPL-3.0-only' = 'copyleft'; 'LicenseRef-Qt-Commercial OR GPL-3.0-only' = 'strong-copyleft'
        'GPL-3.0-or-later OR LicenseRef-Proprietary' = 'strong-copyleft'; 'Unknown-Licence-1.0 OR LGPL-2.1-only' = 'copyleft'
        'LicenseRef-Qt-Commercial OR BSD-3-Clause' = 'permissive'; 'LicenseRef-Custom' = 'unknown'; 'Unknown-Licence-1.0' = 'unknown'
        'GPL-2.0-only' = 'strong-copyleft'; 'GPL-2.0+' = 'strong-copyleft'; 'AGPL-3.0-only' = 'strong-copyleft'
        'GPL-3.0-only WITH Qt-GPL-exception-1.0' = 'copyleft'; 'LGPL-2.1-only' = 'copyleft'; 'MPL-2.0 AND GPL-2.0-only' = 'strong-copyleft'
        'FTL OR GPL-2.0-or-later' = 'permissive'; 'MIT AND LicenseRef-Custom' = 'unknown'; 'NOASSERTION' = 'unknown' }
    foreach ($case in $kinds.GetEnumerator()) {
        Assert-True ((Get-LicenceKind $case.Key) -ceq $case.Value) "Get-LicenceKind '$($case.Key)' is $($case.Value) ($(Get-LicenceKind $case.Key))"
    }
    foreach ($expression in 'LicenseRef-Qt-Commercial OR LGPL-3.0-only', 'LicenseRef-Qt-Commercial OR GPL-3.0-only', 'GPL-3.0-or-later OR LicenseRef-Proprietary') {
        Assert-True (Test-CopyleftLicence $expression) "LIC-4: '$expression' leaves no choice but a copyleft licence"
    }

    # 3. Which components need their corresponding source.
    function New-TestComponent([string]$Port, [string]$Licence, [bool]$Lgpl, [object[]]$Statics = @(), [object[]]$Attributions = @(), [string]$Linkage = 'dynamic') {
        $qt = if ($Attributions.Count -gt 0) { [pscustomobject]@{ Attributions = $Attributions } } else { $null }
        return [pscustomobject]@{ Port = $Port; Version = '1.0'; LicenseConcluded = $Licence; Lgpl = $Lgpl; StaticLibraries = $Statics; QtSbom = $qt; Linkage = $Linkage }
    }
    $policy = [pscustomobject]@{ ports = [pscustomobject]@{ 'mpl-data' = [pscustomobject]@{ reviewedVersion = '1.0'; correspondingSource = $true } } }
    $plan = Get-SourceProvisionPlan -Policy $policy -Components @(
        (New-TestComponent 'qtbase' 'LGPL-3.0-only' $true @() @([pscustomobject]@{ Name = 'Public Suffix List'; License = 'MPL-2.0' })),
        (New-TestComponent 'zlib' 'Zlib' $false),
        (New-TestComponent 'freetype' 'FTL' $false @() @([pscustomobject]@{ Name = 'x'; License = 'FTL OR GPL-2.0-only' })),
        (New-TestComponent 'mpl-data' 'MPL-2.0' $false))
    Assert-True (@($plan.Errors).Count -eq 0) "no errors when every copyleft component is provided for ($(@($plan.Errors) -join ' | '))"
    Assert-True ((@($plan.Components | ForEach-Object Port) -join ',') -eq 'mpl-data,qtbase') "the LGPL port and the port the policy provides for are included ($(@($plan.Components | ForEach-Object Port) -join ','))"
    $plan = Get-SourceProvisionPlan -Policy ([pscustomobject]@{ ports = [pscustomobject]@{ 'other' = [pscustomobject]@{ reviewedVersion = '1.0' } } }) -Components @(
        (New-TestComponent 'libfoo' 'LGPL-2.1-or-later' $false),
        (New-TestComponent 'qtlike' 'BSD-3-Clause' $false @() @([pscustomobject]@{ Name = 'Inner'; License = 'MPL-2.0' })),
        (New-TestComponent 'linked' 'MIT' $false @([pscustomobject]@{ Name = 'Static part'; LicenseConcluded = 'GPL-2.0-only' })))
    Assert-Problem $plan.Errors 'libfoo .*its licence LGPL-2\.1-or-later' 'an unmarked LGPL port is an error'
    Assert-Problem $plan.Errors 'qtlike .*Inner inside it \(MPL-2\.0\)' 'copyleft code inside an unmarked port is an error'
    Assert-Problem $plan.Errors 'linked .*Static part linked from it' 'a copyleft library linked statically from an unmarked port is an error'
    Assert-True (@($plan.Components).Count -eq 0) 'unmarked ports are not included'
    # W12 review LIC-4: a commercial alternative is none; a mark is no remedy for GPL code or for copyleft
    # code compiled into the programs.
    $plan = Get-SourceProvisionPlan -Policy ([pscustomobject]@{ ports = [pscustomobject]@{ 'other' = [pscustomobject]@{ reviewedVersion = '1.0' } } }) -Components @(
        (New-TestComponent 'dualref' 'LicenseRef-Vendor-Commercial OR LGPL-3.0-only' $false))
    Assert-Problem $plan.Errors 'dualref .*does not provide its corresponding source' 'LIC-4: "LicenseRef-...-Commercial OR LGPL-3.0-only" unmarked is an error (Lucent holds no commercial licence)'
    $policy = [pscustomobject]@{ ports = [pscustomobject]@{
        'gpldll' = [pscustomobject]@{ reviewedVersion = '1.0'; correspondingSource = $true }
        'gplok' = [pscustomobject]@{ reviewedVersion = '1.0'; correspondingSource = $true; strongCopyleftAdr = 'ADR 0026 (a test)' }
        'headeronly' = [pscustomobject]@{ reviewedVersion = '1.0'; correspondingSource = $true }
        'mplheaders' = [pscustomobject]@{ reviewedVersion = '1.0'; correspondingSource = $true; fileLevelCopyleftReview = 'Reviewed: MPL-2.0 covers its own files only.' }
        'mplunreviewed' = [pscustomobject]@{ reviewedVersion = '1.0'; correspondingSource = $true } } }
    $plan = Get-SourceProvisionPlan -Policy $policy -Components @(
        (New-TestComponent 'gpldll' 'GPL-3.0-only' $false),
        (New-TestComponent 'gplok' 'GPL-3.0-only' $false),
        (New-TestComponent 'headeronly' 'LGPL-3.0-only' $true -Linkage 'compiled-in'),
        (New-TestComponent 'mplheaders' 'MPL-2.0' $false -Linkage 'compiled-in'),
        (New-TestComponent 'mplunreviewed' 'MPL-2.0' $false -Linkage 'compiled-in'),
        (New-TestComponent 'qtlike2' 'LGPL-3.0-only' $true @() @([pscustomobject]@{ Name = 'GPL tool code'; License = 'LicenseRef-Qt-Commercial OR GPL-3.0-only' })))
    Assert-Problem $plan.Errors 'gpldll .*GPL or AGPL licence without an exception.*strongCopyleftAdr' 'LIC-4: a GPL DLL marked correspondingSource is still an error (it needs an ADR)'
    Assert-True (-not (@($plan.Errors) -match '^Port gplok ')) 'LIC-4: a GPL port whose policy names its ADR is accepted'
    Assert-Problem $plan.Errors 'headeronly .*compiled into Lucent''s programs' 'LIC-4: LGPL code compiled into the programs (header-only) is an error, marked or not'
    Assert-True (-not (@($plan.Errors) -match '^Port mplheaders ')) 'LIC-4: file-level copyleft compiled in passes with its review recorded'
    Assert-Problem $plan.Errors 'mplunreviewed .*fileLevelCopyleftReview' 'LIC-4: file-level copyleft compiled in without a review is an error'
    Assert-Problem $plan.Errors 'qtlike2 .*GPL tool code inside it' 'LIC-4: strong copyleft code inside a marked port is an error'
    $plan = Get-SourceProvisionPlan -Policy ([pscustomobject]@{ ports = [pscustomobject]@{ 'other' = [pscustomobject]@{ reviewedVersion = '1.0' } } }) -Components @(
        (New-TestComponent 'gplunmarked' 'LicenseRef-Vendor-Commercial OR GPL-3.0-only' $false))
    Assert-Problem $plan.Errors 'gplunmarked .*GPL or AGPL licence without an exception' 'LIC-4: an unmarked GPL port (its commercial alternative unavailable) is refused as GPL'
    Assert-True (@($plan.Errors | Where-Object { $_ -match 'set ports\.gplunmarked\.(lgpl|correspondingSource)' }).Count -eq 0) "LIC-4: the mark is not offered as the remedy for GPL code ($(@($plan.Errors) -join ' | '))"

    # 4. Synthetic packages.
    $archive = New-Object byte[] 4096
    (New-Object System.Random 11).NextBytes($archive)
    $portfile = $utf8.GetBytes("vcpkg_from_github(OUT_SOURCE_PATH SOURCE_PATH REPO example/liba REF v1.0 SHA512 $(Get-BytesSha $archive 'SHA512'))`n")
    $portJson = $utf8.GetBytes('{ "name": "liba", "version": "1.0" }')
    $abiInfo = $utf8.GetBytes("features core`ntriplet x64-windows-lucent`n")
    $expected = @([pscustomobject]@{
        Port = 'liba'; Version = '1.0'; Licence = 'LGPL-2.1-or-later'
        Archives = @([pscustomobject]@{ Name = 'example/liba'; Location = 'git+https://github.com/example/liba@v1.0'; Sha512 = (Get-BytesSha $archive 'SHA512') })
        RecipeFiles = @([pscustomobject]@{ Path = 'portfile.cmake'; Sha256 = (Get-BytesSha $portfile 'SHA256') },
                        [pscustomobject]@{ Path = 'vcpkg.json'; Sha256 = (Get-BytesSha $portJson 'SHA256') }) })
    $version = '9.8.7'
    $rootName = "Lucent-$version-corresponding-source"
    $mitTerms = 'Permission is hereby granted, free of charge, to any person obtaining a copy of this software (the MIT License, shortened for the test).'
    function New-TestPackage([string]$Name, [string[]]$Omit = @(), [hashtable]$Replace = @{}, [string[]]$Components = @('liba'), [string]$Writer = 'deterministic',
                             [switch]$Reverse, [switch]$BadSums, [AllowNull()][string]$ManifestText = $null) {
        # Assigned, not the value of an if: an empty array would become $null.
        $manifestBytes = $utf8.GetBytes((ConvertTo-Json -Depth 5 -InputObject ([ordered]@{ schema = 'lucent.corresponding-source'; schemaVersion = 1; productVersion = $version
            components = @($Components | ForEach-Object { [ordered]@{ port = $_ } }) })))
        if ($PSBoundParameters.ContainsKey('ManifestText')) { $manifestBytes = $utf8.GetBytes([string]$ManifestText) }
        $files = [ordered]@{
            'README.txt' = $utf8.GetBytes("readme`r`n")
            'MANIFEST.json' = $manifestBytes
            'sources/example-liba-v1.0.tar.gz' = $archive
            'ports/liba/portfile.cmake' = $portfile
            'ports/liba/vcpkg.json' = $portJson
            'build/liba/vcpkg_abi_info.txt' = $abiInfo
            'licenses/vcpkg-LICENSE.txt' = $utf8.GetBytes("MIT License`n`nCopyright (c) Microsoft Corporation`n`n$mitTerms`n")
            'licenses/Lucent-LICENSE.txt' = $utf8.GetBytes("MIT License`r`n`r`nCopyright (c) 2026 Fan Zhu`r`n`r`n$mitTerms`r`n") }
        foreach ($key in $Replace.Keys) { $files[$key] = $Replace[$key] }
        foreach ($key in $Omit) { $files.Remove($key) }
        $sumNames = [string[]]@($files.Keys)
        [Array]::Sort($sumNames, [System.StringComparer]::Ordinal)
        $sums = @($sumNames | ForEach-Object { "$(Get-BytesSha $files[$_] 'SHA256') *$_" })
        if ($BadSums) { $sums[0] = ('0' * 64) + ' *' + $sumNames[0] }
        $files['SHA256SUMS.txt'] = $utf8.GetBytes((($sums -join "`n") + "`n"))
        $path = Join-Path $work "$Name.zip"
        $list = @($files.Keys | ForEach-Object { [pscustomobject]@{ Name = "$rootName/$_"; Content = $files[$_] } })
        if ($Writer -eq 'deterministic') { New-DeterministicZip -Path $path -Entries $list; return $path }
        # Another writer: System.IO.Compression, in the order given (reversed with -Reverse).
        $ordered = [string[]]@($list | ForEach-Object Name)
        [Array]::Sort($ordered, [System.StringComparer]::Ordinal)
        if ($Reverse) { [array]::Reverse($ordered) }
        $level = if ($Writer -eq 'compressed') { [System.IO.Compression.CompressionLevel]::Optimal } else { [System.IO.Compression.CompressionLevel]::NoCompression }
        $stream = [System.IO.File]::Open($path, [System.IO.FileMode]::CreateNew)
        try {
            $zip = New-Object System.IO.Compression.ZipArchive($stream, [System.IO.Compression.ZipArchiveMode]::Create)
            foreach ($entryName in $ordered) {
                $entry = $zip.CreateEntry($entryName, $level)
                $entry.LastWriteTime = $(if ($Writer -eq 'late') { New-Object DateTimeOffset(2026, 10, 10, 12, 0, 0, [TimeSpan]::Zero) } else { New-Object DateTimeOffset((New-Object DateTime(1980, 2, 1, 0, 0, 0))) })
                $content = ($list | Where-Object { $_.Name -ceq $entryName }).Content
                $out = $entry.Open()
                try { $out.Write($content, 0, $content.Length) } finally { $out.Dispose() }
            }
            $zip.Dispose()
        }
        finally { $stream.Dispose() }
        return $path
    }
    $complete = Test-CorrespondingSourcePackage -Path (New-TestPackage 'complete') -Version $version -Expected $expected
    Assert-True (@($complete.Errors).Count -eq 0) "a complete package passes ($(@($complete.Errors) -join ' | '))"
    Assert-True (@($complete.Entries).Count -eq 9) "it lists its nine files ($(@($complete.Entries).Count))"
    Assert-True ($complete.Sha256 -match '^[0-9a-f]{64}$' -and $complete.Size -gt 4096) 'it reports the archive''s size and SHA-256'
    Assert-Problem (Test-CorrespondingSourcePackage -Path (New-TestPackage 'no-archive' -Omit 'sources/example-liba-v1.0.tar.gz') -Version $version -Expected $expected).Errors `
        'liba 1\.0 .*source archive example/liba .* is not in sources/' 'a missing source archive fails'
    Assert-Problem (Test-CorrespondingSourcePackage -Path (New-TestPackage 'no-recipe' -Omit 'ports/liba/vcpkg.json') -Version $version -Expected $expected).Errors `
        'recipe file ports/liba/vcpkg\.json is missing' 'a missing recipe file fails'
    Assert-Problem (Test-CorrespondingSourcePackage -Path (New-TestPackage 'changed-recipe' -Replace @{ 'ports/liba/portfile.cmake' = $utf8.GetBytes('changed') }) -Version $version -Expected $expected).Errors `
        'ports/liba/portfile\.cmake is not the file vcpkg built it with' 'a changed recipe file fails'
    Assert-Problem (Test-CorrespondingSourcePackage -Path (New-TestPackage 'no-component' -Components @()) -Version $version -Expected $expected).Errors `
        'liba 1\.0 \(LGPL-2\.1-or-later\) needs its corresponding source, but MANIFEST\.json does not list it' 'a component missing from MANIFEST.json fails'
    Assert-Problem (Test-CorrespondingSourcePackage -Path (New-TestPackage 'no-build-record' -Omit 'build/liba/vcpkg_abi_info.txt') -Version $version -Expected $expected).Errors `
        'build/liba/vcpkg_abi_info\.txt is missing' 'a missing build record fails'
    # The W12 gate-8 reading of 2026-10-16 (D9): vcpkg's licence ships with its recipes, Lucent's with its files.
    Assert-Problem (Test-CorrespondingSourcePackage -Path (New-TestPackage 'no-vcpkg-licence' -Omit 'licenses/vcpkg-LICENSE.txt') -Version $version -Expected $expected).Errors `
        "licenses/vcpkg-LICENSE\.txt is missing: vcpkg's licence" 'D9: an archive without vcpkg''s licence beside its recipes fails'
    Assert-Problem (Test-CorrespondingSourcePackage -Path (New-TestPackage 'no-lucent-licence' -Omit 'licenses/Lucent-LICENSE.txt') -Version $version -Expected $expected).Errors `
        "licenses/Lucent-LICENSE\.txt is missing: Lucent's licence" 'D9: an archive without Lucent''s licence beside its overlay ports fails'
    Assert-Problem (Test-CorrespondingSourcePackage -Path (New-TestPackage 'not-mit' -Replace @{ 'licenses/vcpkg-LICENSE.txt' = $utf8.GetBytes("Some other terms`n") }) -Version $version -Expected $expected).Errors `
        'licenses/vcpkg-LICENSE\.txt is not the MIT License with its copyright line' 'D9: a vcpkg licence file that is no MIT licence fails'
    Assert-Problem (Test-CorrespondingSourcePackage -Path (New-TestPackage 'bad-sums' -BadSums) -Version $version -Expected $expected).Errors `
        'but SHA256SUMS\.txt lists 0{64}' 'a wrong SHA256SUMS line fails'
    Assert-Problem (Test-CorrespondingSourcePackage -Path (New-TestPackage 'compressed' -Writer compressed) -Version $version -Expected $expected).Errors `
        'is compressed; every entry is stored' 'a compressed entry fails'
    Assert-Problem (Test-CorrespondingSourcePackage -Path (New-TestPackage 'unsorted' -Writer stored -Reverse) -Version $version -Expected $expected).Errors `
        'the entries are not sorted' 'entries out of order fail'
    Assert-Problem (Test-CorrespondingSourcePackage -Path (New-TestPackage 'late' -Writer late) -Version $version -Expected $expected).Errors `
        'has the time 2026-10-10 12:00:00, not 1980-02-01' 'another time fails'
    Assert-Problem (Test-CorrespondingSourcePackage -Path (New-TestPackage 'other-version') -Version '9.8.8' -Expected $expected).Errors `
        'is not inside the folder Lucent-9\.8\.8-corresponding-source/' 'another version''s package fails'
    $notZip = Join-Path $work 'not-a.zip'
    [System.IO.File]::WriteAllBytes($notZip, $utf8.GetBytes('not a zip'))
    Assert-Problem (Test-CorrespondingSourcePackage -Path $notZip -Version $version -Expected $expected).Errors 'is not a readable ZIP archive' 'a file that is not a ZIP archive fails'
    # W12 review LIC-3: a MANIFEST.json that is valid JSON but no object, or an object without members,
    # fails, and the expected archive, recipe and build record are checked all the same (here the
    # source archive is missing too).
    $falsy = [ordered]@{ 'null' = 'null'; 'an empty array' = '[]'; 'zero' = '0'; 'false' = 'false'; 'an empty string' = '""'; 'an empty file' = '' }
    $index = 0
    foreach ($case in $falsy.GetEnumerator()) {
        $index++
        $threw = $null
        $errors = @()
        try { $errors = @((Test-CorrespondingSourcePackage -Path (New-TestPackage "falsy-$index" -ManifestText $case.Value -Omit 'sources/example-liba-v1.0.tar.gz') -Version $version -Expected $expected).Errors) }
        catch { $threw = $_.Exception.Message }
        Assert-True (-not $threw) "LIC-3: MANIFEST.json of $($case.Key) is checked without an exception ($threw)"
        Assert-Problem $errors 'MANIFEST\.json is not a JSON object' "LIC-3: MANIFEST.json of $($case.Key) is no manifest"
        Assert-Problem $errors 'source archive example/liba .* is not in sources/' "LIC-3: with MANIFEST.json of $($case.Key), a missing source archive is still found"
    }
    $threw = $null
    $errors = @()
    try { $errors = @((Test-CorrespondingSourcePackage -Path (New-TestPackage 'empty-object' -ManifestText '{}' -Omit 'ports/liba/vcpkg.json') -Version $version -Expected $expected).Errors) }
    catch { $threw = $_.Exception.Message }
    Assert-True (-not $threw) "LIC-3: MANIFEST.json of {} is checked without an exception ($threw)"
    Assert-Problem $errors 'MANIFEST\.json is not a lucent\.corresponding-source version 1' 'LIC-3: MANIFEST.json of {} is no manifest of this schema'
    Assert-Problem $errors 'recipe file ports/liba/vcpkg\.json is missing' 'LIC-3: with MANIFEST.json of {}, a missing recipe file is still found'

    # 5. A package manifest's record gives the same expectations as vcpkg's SPDX data.
    $spdxComponent = [pscustomobject]@{
        Port = 'liba'; Version = '1.0'; LicenseConcluded = 'LGPL-2.1-or-later'
        Sources = @([pscustomobject]@{ Name = 'example/liba'; Location = 'git+https://github.com/example/liba@v1.0'; Sha512 = (Get-BytesSha $archive 'SHA512').ToUpperInvariant() })
        Spdx = [pscustomobject]@{ files = @(
            [pscustomobject]@{ SPDXID = 'SPDXRef-port-file-1'; fileName = './vcpkg.json'; checksums = @([pscustomobject]@{ algorithm = 'SHA256'; checksumValue = (Get-BytesSha $portJson 'SHA256') }) },
            [pscustomobject]@{ SPDXID = 'SPDXRef-port-file-0'; fileName = './portfile.cmake'; checksums = @([pscustomobject]@{ algorithm = 'SHA256'; checksumValue = (Get-BytesSha $portfile 'SHA256') }) },
            [pscustomobject]@{ SPDXID = 'SPDXRef-binary-file-0'; fileName = './bin/liba.dll'; checksums = @() }) } }
    $fromSpdx = Get-SourceProvisionExpectation -Component $spdxComponent
    $record = [pscustomobject]@{ components = @([pscustomobject]@{ port = 'liba'; version = '1.0'; licence = 'LGPL-2.1-or-later'
        archives = @([pscustomobject]@{ file = 'sources/example-liba-v1.0.tar.gz'; url = 'git+https://github.com/example/liba@v1.0'; sha512 = (Get-BytesSha $archive 'SHA512') })
        recipe = [pscustomobject]@{ files = @([pscustomobject]@{ path = 'ports/liba/portfile.cmake'; sha256 = (Get-BytesSha $portfile 'SHA256') },
                                              [pscustomobject]@{ path = 'ports/liba/vcpkg.json'; sha256 = (Get-BytesSha $portJson 'SHA256') }) } }) }
    $fromRecord = @(ConvertFrom-SourcePackageRecord -Record $record)[0]
    Assert-True (($fromSpdx.RecipeFiles | ForEach-Object { "$($_.Path)=$($_.Sha256)" }) -join ';' -ceq (($fromRecord.RecipeFiles | ForEach-Object { "$($_.Path)=$($_.Sha256)" }) -join ';')) 'the same recipe files from SPDX data and from a package manifest'
    Assert-True ($fromSpdx.Archives[0].Sha512 -ceq $fromRecord.Archives[0].Sha512) 'the same archive SHA-512 (lower case) from both'
    Assert-True (@($fromSpdx.RecipeFiles).Count -eq 2) 'binary files of the SPDX document are not recipe files'
    Assert-True (@((Test-CorrespondingSourcePackage -Path (New-TestPackage 'from-record') -Version $version -Expected @($fromRecord)).Errors).Count -eq 0) 'a complete package passes against the record''s expectations'
    Assert-True ((Format-SourceDownload 'git+https://github.com/lensfun/lensfun@v0.3.4') -ceq 'https://github.com/lensfun/lensfun/archive/v0.3.4.tar.gz') 'a GitHub source is shown as its archive URL'

    # 6. A recipe folder is copied only exactly as vcpkg built with it (Get-RecipeCopy).
    $recipe = Join-Path $work 'recipe'
    New-Item -ItemType Directory -Path (Join-Path $recipe 'detect') -Force | Out-Null
    [System.IO.File]::WriteAllBytes((Join-Path $recipe 'portfile.cmake'), $portfile)
    [System.IO.File]::WriteAllBytes((Join-Path $recipe 'detect\CMakeLists.txt'), $portJson)
    $recorded = @([pscustomobject]@{ Path = 'portfile.cmake'; Sha256 = (Get-BytesSha $portfile 'SHA256') },
                  [pscustomobject]@{ Path = 'detect/CMakeLists.txt'; Sha256 = (Get-BytesSha $portJson 'SHA256') })
    $problems = New-Object System.Collections.Generic.List[string]
    $copy = @(Get-RecipeCopy -Port 'liba' -Folder $recipe -Describe 'the test recipe' -RecipeFiles $recorded -Errors $problems)
    Assert-True ($problems.Count -eq 0 -and $copy.Count -eq 2) "the recipe as built is copied ($($problems -join ' | '))"
    Assert-True ((@($copy | ForEach-Object Name) -join ',') -ceq 'ports/liba/portfile.cmake,ports/liba/detect/CMakeLists.txt') "entries below ports/liba/ ($(@($copy | ForEach-Object Name) -join ','))"
    [System.IO.File]::WriteAllBytes((Join-Path $recipe 'extra.patch'), $portJson)
    [System.IO.File]::WriteAllBytes((Join-Path $recipe 'portfile.cmake'), $utf8.GetBytes('changed'))
    $problems = New-Object System.Collections.Generic.List[string]
    $copy = @(Get-RecipeCopy -Port 'liba' -Folder $recipe -Describe 'the test recipe' -RecipeFiles $recorded -Errors $problems)
    Assert-Problem $problems.ToArray() 'portfile\.cmake in the test recipe is not the file vcpkg built it with' 'a changed recipe file is refused'
    Assert-Problem $problems.ToArray() 'extra\.patch is in the test recipe, but vcpkg did not build with it' 'a file vcpkg did not build with is refused'
    Remove-Item -LiteralPath (Join-Path $recipe 'detect\CMakeLists.txt')
    $problems = New-Object System.Collections.Generic.List[string]
    $copy = @(Get-RecipeCopy -Port 'liba' -Folder $recipe -Describe 'the test recipe' -RecipeFiles $recorded -Errors $problems)
    Assert-Problem $problems.ToArray() 'detect/CMakeLists\.txt, which vcpkg built it with, is missing' 'a missing recipe file is refused'
    Assert-True ((Format-SourceDownload 'https://download.gnome.org/sources/glib/2.90/glib-2.90.0.tar.xz') -ceq 'https://download.gnome.org/sources/glib/2.90/glib-2.90.0.tar.xz') 'a download URL is shown as it is'

    # 7. Orders that reach the archive and the notices do not depend on the regional format (W12 review
    # LIC-7): Estonian sorts z between s and t, ordinal order does not.
    $names = @('zlib', 'tiff', 'sqlite3', 'tl-expected', 'vcpkg-cmake', 'QTBUG-145703.patch', 'allow_outside_prefix.patch')
    $components = @($names | ForEach-Object { New-TestComponent $_ 'MIT' $false })
    $orders = @{}
    foreach ($culture in 'en-US', 'et-EE') {
        $orders[$culture] = & {
            $saved = [System.Threading.Thread]::CurrentThread.CurrentCulture
            try {
                [System.Threading.Thread]::CurrentThread.CurrentCulture = [System.Globalization.CultureInfo]::GetCultureInfo($culture)
                [pscustomobject]@{
                    SortObject = (@($names | Sort-Object) -join '|')
                    Ordinal = (@(ConvertTo-OrdinalOrder -Items $names) -join '|')
                    Plan = (@((Get-SourceProvisionPlan -Policy ([pscustomobject]@{ ports = [pscustomobject]@{} }) -Components $components).Components) | ForEach-Object Port) -join '|'
                    ByProperty = (@(ConvertTo-OrdinalOrder -Items $components -Property Port) | ForEach-Object Port) -join '|' }
            }
            finally { [System.Threading.Thread]::CurrentThread.CurrentCulture = $saved }
        }
    }
    Assert-True ($orders['en-US'].SortObject -cne $orders['et-EE'].SortObject) "LIC-7: Sort-Object orders by the culture ($($orders['en-US'].SortObject) / $($orders['et-EE'].SortObject))"
    Assert-True ($orders['en-US'].Ordinal -ceq $orders['et-EE'].Ordinal -and $orders['en-US'].Ordinal -ceq 'QTBUG-145703.patch|allow_outside_prefix.patch|sqlite3|tiff|tl-expected|vcpkg-cmake|zlib') "LIC-7: the ordinal order is the same under en-US and et-EE ($($orders['et-EE'].Ordinal))"
    Assert-True ($orders['en-US'].ByProperty -ceq $orders['et-EE'].ByProperty -and $orders['en-US'].ByProperty -ceq $orders['en-US'].Ordinal) 'LIC-7: ordering records by a property is ordinal too'
    $plan = Get-SourceProvisionPlan -Policy ([pscustomobject]@{ ports = [pscustomobject]@{ zlib = [pscustomobject]@{ correspondingSource = $true }; tiff = [pscustomobject]@{ correspondingSource = $true }; sqlite3 = [pscustomobject]@{ correspondingSource = $true } } }) -Components $components
    $planned = & {
        $saved = [System.Threading.Thread]::CurrentThread.CurrentCulture
        try {
            [System.Threading.Thread]::CurrentThread.CurrentCulture = [System.Globalization.CultureInfo]::GetCultureInfo('et-EE')
            (@((Get-SourceProvisionPlan -Policy ([pscustomobject]@{ ports = [pscustomobject]@{ zlib = [pscustomobject]@{ correspondingSource = $true }; tiff = [pscustomobject]@{ correspondingSource = $true }; sqlite3 = [pscustomobject]@{ correspondingSource = $true } } }) -Components $components).Components) | ForEach-Object Port) -join '|'
        }
        finally { [System.Threading.Thread]::CurrentThread.CurrentCulture = $saved }
    }
    Assert-True ((@($plan.Components | ForEach-Object Port) -join '|') -ceq 'sqlite3|tiff|zlib' -and $planned -ceq 'sqlite3|tiff|zlib') "LIC-7: the corresponding-source components come in the same order under et-EE ($planned)"
    $unique = @(ConvertTo-OrdinalOrder -Items @('b', 'a', 'b', 'A') -Unique)
    Assert-True (($unique -join '|') -ceq 'A|a|b') "LIC-7: -Unique keeps one of each name ($($unique -join '|'))"
    # The other files packaging publishes: an Estonian machine wrote the archive's size in LGPL-NOTICE.txt
    # as "132 480 210" and listed mil-zeiss.xml before mil-tamron.xml in the package manifest.
    $inventoryRoot = Join-Path $work 'inventory'
    New-Item -ItemType Directory -Path $inventoryRoot -Force | Out-Null
    foreach ($name in 'mil-tamron.xml', 'mil-zeiss.xml', 'mil-tokina.xml', 'Lucent.exe') { [System.IO.File]::WriteAllText((Join-Path $inventoryRoot $name), $name) }
    $published = @{}
    foreach ($culture in 'en-US', 'et-EE') {
        $noticeStage = Join-Path $work "notice-$culture"
        $published[$culture] = & {
            $saved = [System.Threading.Thread]::CurrentThread.CurrentCulture
            try {
                [System.Threading.Thread]::CurrentThread.CurrentCulture = [System.Globalization.CultureInfo]::GetCultureInfo($culture)
                Write-LgplNotice -StageDir $noticeStage -Info ([pscustomobject]@{ version = '9.8.7'; homepage = 'https://example.org/lucent'; executable = 'Lucent.exe'; helperExecutables = @() }) `
                    -Components @([pscustomobject]@{ Port = 'liba'; Lgpl = $false; StaticLibraries = @() }) -NoticesDir (Join-Path $repoRoot 'installer\notices') `
                    -SourcePackage ([pscustomobject]@{ File = 'Lucent-9.8.7-corresponding-source.zip'; Size = [int64]132480210; Sha256 = ('ab' * 32); Components = @() }) `
                    -Baseline ('e143402b' + '0' * 32) | Out-Null
                [pscustomobject]@{
                    Inventory = (@(Get-FileInventory -Root $inventoryRoot) | ForEach-Object { $_.path }) -join '|'
                    Notice = [System.IO.File]::ReadAllText((Join-Path $noticeStage 'licenses\LGPL-NOTICE.txt')) }
            }
            finally { [System.Threading.Thread]::CurrentThread.CurrentCulture = $saved }
        }
    }
    Assert-True ($published['en-US'].Inventory -ceq $published['et-EE'].Inventory -and $published['et-EE'].Inventory -ceq 'Lucent.exe|mil-tamron.xml|mil-tokina.xml|mil-zeiss.xml') `
        "LIC-7: the package manifest's file list is ordinal under en-US and et-EE ($($published['et-EE'].Inventory))"
    Assert-True ($published['en-US'].Notice -ceq $published['et-EE'].Notice -and $published['et-EE'].Notice.Contains('(132,480,210 bytes, SHA-256')) `
        'LIC-7: LGPL-NOTICE.txt is the same under en-US and et-EE (the archive''s size grouped with commas)'

    # 8. What a recipe changes in its library's source (W12 review LIC-1): the patches its portfile
    # names, with the files each changes, who made it and when.
    $recipeDir = Join-Path $work 'modified-recipe'
    New-Item -ItemType Directory -Path (Join-Path $recipeDir 'patches') -Force | Out-Null
    $vcpkgPatch = "From vcpkg`n--- a/glib/gconvert.c`n+++ b/glib/gconvert.c`n@@ -1 +1 @@`n-a`n+b`n--- a/meson.build`t2024-01-01`n+++ b/meson.build`t2024-01-02`n@@ -1 +1 @@`n-a`n+b`n"
    $lucentPatch = "--- a/meson.build`n+++ b/meson.build`n@@ -1 +1,2 @@`n+# Changed by the Lucent project, 2026-10-10: the locale folder.`n a`n"
    $removing = "--- a/old/file.c`n+++ /dev/null`n@@ -1 +0,0 @@`n-gone`n"
    [System.IO.File]::WriteAllText((Join-Path $recipeDir 'use-libiconv-on-windows.patch'), $vcpkgPatch)
    [System.IO.File]::WriteAllText((Join-Path $recipeDir 'lucent-neutral-locale-dir.patch'), $lucentPatch)
    [System.IO.File]::WriteAllText((Join-Path $recipeDir 'patches\remove-old.diff'), $removing)
    [System.IO.File]::WriteAllText((Join-Path $recipeDir 'unused.patch'), $vcpkgPatch)
    [System.IO.File]::WriteAllText((Join-Path $recipeDir 'portfile.cmake'), "vcpkg_from_gitlab(`n    PATCHES`n        use-libiconv-on-windows.patch`n        lucent-neutral-locale-dir.patch`n        patches/remove-old.diff`n)`n")
    $copyList = @(Get-ChildItem -LiteralPath $recipeDir -Recurse -File | ForEach-Object {
        [pscustomobject]@{ Name = 'ports/glib/' + $_.FullName.Substring($recipeDir.Length + 1).Replace('\', '/'); SourcePath = $_.FullName } })
    $comparison = [pscustomobject]@{ Files = @(
        [pscustomobject]@{ File = 'use-libiconv-on-windows.patch'; Status = 'unchanged' }, [pscustomobject]@{ File = 'lucent-neutral-locale-dir.patch'; Status = 'added' },
        [pscustomobject]@{ File = 'patches/remove-old.diff'; Status = 'line endings' }, [pscustomobject]@{ File = 'unused.patch'; Status = 'unchanged' },
        [pscustomobject]@{ File = 'portfile.cmake'; Status = 'changed' }) }
    Assert-True ((@(Get-PatchedFiles -Text $vcpkgPatch) -join '|') -ceq 'glib/gconvert.c|meson.build') "LIC-1: a patch's files (its +++ lines, a/ b/ and time stamps removed): $(@(Get-PatchedFiles -Text $vcpkgPatch) -join '|')"
    Assert-True ((@(Get-PatchedFiles -Text $removing) -join '|') -ceq 'old/file.c') 'LIC-1: a deleted file is named by its --- line'
    $modifications = @(Get-RecipeModifications -Port 'glib' -RecipeFiles $copyList -Comparison $comparison -Baseline ('e143402b' + '0' * 32) -BaselineDate '2026-09-24')
    $summary = @($modifications | ForEach-Object { "$([System.IO.Path]::GetFileName($_.patch))=$($_.by)/$($_.date)/$(@($_.changes) -join ',')" }) -join '; '
    Assert-True ($modifications.Count -eq 3) "LIC-1: the three patches the portfile names, not the unused one ($summary)"
    Assert-True ($summary -cmatch 'use-libiconv-on-windows\.patch=vcpkg/2026-09-24/glib/gconvert\.c,meson\.build') "LIC-1: vcpkg's patch, dated by the registry commit ($summary)"
    Assert-True ($summary -cmatch 'lucent-neutral-locale-dir\.patch=the Lucent project/2026-10-10/meson\.build') "LIC-1: Lucent's patch, dated as it states ($summary)"
    Assert-True ($summary -cmatch 'remove-old\.diff=vcpkg/2026-09-24/old/file\.c') "LIC-1: a patch in a subfolder, its line endings only changed, is vcpkg's ($summary)"
    $lines = @(Format-RecipeModifications -Modifications $modifications -Prefix '  Modified:   ' -Baseline ('e143402b' + '0' * 32))
    Assert-True (($lines -join "`n") -match "lucent-neutral-locale-dir\.patch \(the Lucent project, 2026-10-10\) changes meson\.build" -and ($lines -join "`n") -match "registry at commit e143402b of 2026-09-24") ($lines -join ' / ')
    Assert-True ((@(Format-RecipeModifications -Modifications @() -Prefix '  Modified:   ') -join '') -match 'no: built from the upstream source archive as released') 'LIC-1: a library without patches says so'
    $registry = @(Get-RecipeModifications -Port 'glib' -RecipeFiles $copyList -Baseline ('e143402b' + '0' * 32) -BaselineDate '2026-09-24')
    Assert-True (@($registry | Where-Object { $_.by -ne 'vcpkg' }).Count -eq 0) 'LIC-1: in a registry port (no overlay) every patch is vcpkg''s'

    # 9. The code of these packages linked into Lucent's programs (W12 review LIC-5).
    $info = [pscustomobject]@{ executable = 'Lucent.exe'; helperExecutables = @('lucent-worker.exe') }
    $withEntry = [pscustomobject]@{ Port = 'qtbase'; StaticLibraries = @([pscustomobject]@{ Name = 'Qt Windows entry point (Qt6::EntryPoint)'; File = 'Qt6EntryPoint.lib'
        LicenseConcluded = 'BSD-3-Clause'; Executables = @('Lucent.exe') }) }
    $text = Format-StaticCodeText -Components @($withEntry, [pscustomobject]@{ Port = 'libraw'; StaticLibraries = @() }) -Info $info
    Assert-True ($text -match 'Qt6EntryPoint\.lib, from qtbase\), linked statically into Lucent\.exe and used under BSD-3-Clause' -and $text -notmatch 'lucent-worker') "LIC-5: the README names the entry point, the one program that links it and its licence ($text)"
    Assert-True ((Format-StaticCodeText -Components @([pscustomobject]@{ Port = 'libraw'; StaticLibraries = @() }) -Info $info) -match '^No code of them is linked into Lucent\.exe and lucent-worker\.exe\.$') 'LIC-5: without statically linked code it says none is linked'
    $readme = [System.IO.File]::ReadAllText((Join-Path $repoRoot 'installer\notices\CORRESPONDING-SOURCE-README.txt.in'))
    Assert-True ($readme -match '@STATIC_CODE@' -and $readme -notmatch 'no code of them is linked into Lucent\.exe') 'LIC-5: the README template states the linked code from the build, not "no code"'

    # 10. The Inno Setup compiler's licence (W12 review LIC-6): the whole banner, and a commercial
    # distribution only with a licence the banner states as commercial.
    $head = @('Inno Setup 6 Command-Line Compiler', 'Copyright (C) 1997-2026 Jordan Russell. All rights reserved.', 'Portions Copyright (C) 2000-2026 Martijn Laan. All rights reserved.', 'https://www.innosetup.com', '')
    $banners = [ordered]@{
        '6.7.3' = @($head + @('Compiler engine version: Inno Setup 6.7.3', 'Non-commercial use only', '', 'Preprocessing', 'Compiling [Setup] section'))
        'licence line elsewhere' = @(@('Inno Setup 6 Command-Line Compiler', 'Non-commercial use only') + $head + @('Compiler engine version: Inno Setup 6.8.0', '', 'Preprocessing'))
        'commercial' = @($head + @('Compiler engine version: Inno Setup 6.8.0', 'Commercial license: Example Ltd', '', 'Preprocessing'))
        'none' = @($head + @('Compiler engine version: Inno Setup 6.8.0', '', 'Preprocessing', 'Reading file: Non-commercial-notes.iss'))
        'translated' = @($head + @('Compiler engine version: Inno Setup 6.8.0', 'Nur zur nichtkommerziellen Nutzung', '', 'Preprocessing')) }
    $expectedKinds = @{ '6.7.3' = 'non-commercial'; 'licence line elsewhere' = 'non-commercial'; 'commercial' = 'commercial'; 'none' = $null; 'translated' = $null }
    foreach ($case in $banners.GetEnumerator()) {
        $licence = Get-InnoSetupLicence -Output $case.Value
        Assert-True ($licence.Kind -eq $expectedKinds[$case.Key]) "LIC-6: the banner '$($case.Key)' states a licence of kind $($expectedKinds[$case.Key]) ($($licence.Kind): $($licence.Line))"
        $commercial = Test-InnoSetupLicence -Licence $licence -Commercial $true
        Assert-True ((@($commercial.Errors).Count -eq 0) -eq ($expectedKinds[$case.Key] -eq 'commercial')) "LIC-6: a commercial distribution with the banner '$($case.Key)' is $(if ($expectedKinds[$case.Key] -eq 'commercial') { 'accepted' } else { 'refused' })"
        $free = Test-InnoSetupLicence -Licence $licence -Commercial $false
        Assert-True (@($free.Errors).Count -eq 0 -and ((@($free.Warnings).Count -gt 0) -eq ($null -eq $expectedKinds[$case.Key]))) "LIC-6: a free distribution with the banner '$($case.Key)' passes$(if ($null -eq $expectedKinds[$case.Key]) { ', with a warning' })"
    }

    # 11. The gate-8 reading of 2026-10-16: LibRaw's licence and the terms of its parts (D1, D2), the
    # versions and data the notices name (D3, D4, D5), the notes that name every patch changing code
    # (D6, D8), vcpkg's licence beside its recipes and the recipe files' licences (D9), and Lucent's
    # own files in the SBOM (D10).
    $realPolicy = [System.IO.File]::ReadAllText((Join-Path $repoRoot 'installer\license-policy.json')) | ConvertFrom-Json

    # 11a. A part's terms are the lines from the one line holding the first marker to the next holding
    # the last, exactly as they are.
    $sample = "/*`r`n * Copyright (C) 2010, Somebody`r`n *`tRedistribution and use in source and binary forms`r`n * OF SUCH DAMAGE.`r`n */`r`n// Copyright (C) 2010, Somebody else`n"
    $excerpt = Get-LicenceExcerpt -Text $sample -First ' * Copyright (C) 2010, Somebody' -Last 'OF SUCH DAMAGE.'
    Assert-True ((@($excerpt.Lines) -join '|') -ceq " * Copyright (C) 2010, Somebody| *`tRedistribution and use in source and binary forms| * OF SUCH DAMAGE." -and $excerpt.FirstLine -eq 2 -and $excerpt.LastLine -eq 4) `
        "D2: an excerpt is its lines exactly, tabs kept (lines $($excerpt.FirstLine)-$($excerpt.LastLine): $(@($excerpt.Lines) -join '|'))"
    Assert-Throws { Get-LicenceExcerpt -Text $sample -First 'Copyright (C) 2010, Somebody' -Last 'OF SUCH DAMAGE.' } 'D2: a first marker on two lines is refused'
    Assert-Throws { Get-LicenceExcerpt -Text $sample -First 'Not in the text' -Last 'OF SUCH DAMAGE.' } 'D2: a first marker that is not there is refused'
    Assert-Throws { Get-LicenceExcerpt -Text $sample -First ' * Copyright (C) 2010, Somebody' -Last 'NOT IN THE TEXT' } 'D2: a last marker that does not follow is refused'

    # 11b. The terms come from the verified source archive (its SHA-512 as vcpkg recorded it), verbatim.
    $tree = Join-Path $work 'part-archive'
    New-Item -ItemType Directory -Path (Join-Path $tree 'pkg-1.0\src') -Force | Out-Null
    $author = 'Luis Sanz Rodr' + [char]0xED + 'guez'
    $partSource = "/*`n * Copyright (C) 2010, Jane Doe`n *`n *    Redistribution and use in source and binary forms, with or without`n * `tmodification, are permitted.`n *    OF SUCH DAMAGE.`n */`n// FBDD by Jane Doe and $author (jane@example.org)`nint code;`n"
    [System.IO.File]::WriteAllText((Join-Path $tree 'pkg-1.0\src\part.c'), $partSource, $utf8)
    [System.IO.File]::WriteAllText((Join-Path $tree 'pkg-1.0\COPYRIGHT'), "pkg uses pieces of code of Big Corp,`r`nCopyright (c) 2005 Big Corp, licensed under MIT license`r`n", $utf8)
    $downloadsDir = Join-Path $work 'downloads'
    New-Item -ItemType Directory -Path $downloadsDir -Force | Out-Null
    $tarball = Join-Path $downloadsDir 'example-pkg-v1.0.tar.gz'
    & (Join-Path $env:SystemRoot 'System32\tar.exe') -czf $tarball -C $tree 'pkg-1.0'
    Assert-True ($LASTEXITCODE -eq 0) 'D2: a test source archive is made with tar'
    $tarSha512 = (Get-FileHash -LiteralPath $tarball -Algorithm SHA512).Hash.ToLowerInvariant()
    $resource = [pscustomobject]@{ name = 'example/pkg'; downloadLocation = 'git+https://github.com/example/pkg@v1.0'; packageFileName = 'example-pkg-v1.0.tar.gz'
        checksums = @([pscustomobject]@{ algorithm = 'SHA512'; checksumValue = $tarSha512 }) }
    $partSources = @([pscustomobject]@{ Name = 'example/pkg'; Location = $resource.downloadLocation; Sha512 = $tarSha512; Resource = $resource })
    $found = Find-SourceArchiveMember -Sources $partSources -Member 'src/part.c' -DownloadsDir $downloadsDir
    Assert-True ($null -ne $found -and $found.Text -ceq $partSource -and $found.Archive -ceq 'example-pkg-v1.0.tar.gz' -and $found.Sha512 -ceq $tarSha512) `
        'D2: a member of the verified archive is read as it is (UTF-8 kept)'
    Assert-True ($null -eq (Find-SourceArchiveMember -Sources $partSources -Member 'src/missing.c' -DownloadsDir $downloadsDir)) 'D2: a member the archive does not hold is none'
    $badResource = [pscustomobject]@{ name = $resource.name; downloadLocation = $resource.downloadLocation; packageFileName = $resource.packageFileName
        checksums = @([pscustomobject]@{ algorithm = 'SHA512'; checksumValue = ('0' * 128) }) }
    Assert-Throws { Find-SourceArchiveMember -Sources @([pscustomobject]@{ Name = 'example/pkg'; Location = $resource.downloadLocation; Sha512 = ('0' * 128); Resource = $badResource }) -Member 'src/part.c' -DownloadsDir $downloadsDir } `
        'D2: an archive whose SHA-512 is not the one vcpkg recorded is refused'
    $testParts = @(
        [pscustomobject]@{ name = 'Part demosaic'; copyright = 'Copyright (C) 2010, Jane Doe'; licence = 'BSD-3-Clause'; member = 'src/part.c'
                           first = 'Copyright (C) 2010, Jane Doe'; last = '(jane@example.org)'; as = 'Part-BSD-3-Clause.txt'; comment = 'Compiled into pkg.dll.' },
        [pscustomobject]@{ name = 'Code of Big Corp'; copyright = 'Copyright (c) 2005 Big Corp'; licence = 'MIT'; member = 'COPYRIGHT'
                           first = 'pkg uses pieces of code of Big Corp,'; last = 'licensed under MIT license'; as = 'BigCorp-MIT.txt'
                           textIn = @([pscustomobject]@{ file = 'other\copyright.txt'; marker = 'Permission is hereby granted'; holds = "the MIT License, another component's licence" }) })
    $partText = Format-PartLicenceText -Part $testParts[0] -Port 'pkg' -Version '1.0' -ArchiveName $found.Archive -ArchiveSha512 $found.Sha512 `
        -Excerpt (Get-LicenceExcerpt -Text $found.Text -First $testParts[0].first -Last $testParts[0].last)
    Assert-True ($partText.Contains(" * Copyright (C) 2010, Jane Doe`n *`n *    Redistribution and use in source and binary forms, with or without`n * `tmodification, are permitted.`n *    OF SUCH DAMAGE.`n */`n// FBDD by Jane Doe and $author (jane@example.org)`n")) `
        'D2: the part''s file holds its terms verbatim, from the copyright line to its authors'
    $flatPart = $partText -replace '\s+', ' '
    Assert-True ($partText.StartsWith("pkg 1.0: Part demosaic, BSD-3-Clause`n") -and $flatPart.Contains("src/part.c states them, lines 2 to 8, in example-pkg-v1.0.tar.gz (SHA-512 $tarSha512)")) `
        'D2: the part''s file names the part, its licence, the member, the lines and the verified archive'
    $mitText = Format-PartLicenceText -Part $testParts[1] -Port 'pkg' -Version '1.0' -ArchiveName $found.Archive -ArchiveSha512 $found.Sha512 `
        -Excerpt (Get-LicenceExcerpt -Text (Find-SourceArchiveMember -Sources $partSources -Member 'COPYRIGHT' -DownloadsDir $downloadsDir).Text -First $testParts[1].first -Last $testParts[1].last)
    # Since the gate-8 review, round 2: a licence the archive names without its text is pointed at in another
    # component's licence file, never written as words Lucent supplies.
    Assert-True ($mitText.Contains("pkg uses pieces of code of Big Corp,`nCopyright (c) 2005 Big Corp, licensed under MIT license`n") -and
                 ($mitText -replace '\s+', ' ').Contains("The text of the MIT licence ships in this installation's licence folder, in another component's file: other\copyright.txt (the MIT License, another component's licence).") -and
                 -not $mitText.Contains('Lucent supplies')) `
        'D2/round 2: a licence the archive names without its text is the text of another component''s licence file, which the part''s file names'

    # 11c. A compiled part's licence must be in the concluded expression (LibRaw's MIT code was not).
    Assert-True (@(Test-PartLicences -Port 'pkg' -Concluded 'LGPL-2.1-only AND BSD-3-Clause AND MIT' -Parts $testParts).Count -eq 0) 'D1: parts whose licences the concluded expression names pass'
    Assert-Problem (Test-PartLicences -Port 'pkg' -Concluded 'LGPL-2.1-only AND BSD-3-Clause' -Parts $testParts) `
        "part 'Code of Big Corp' of pkg is compiled into its files under MIT, but the port's concluded licence 'LGPL-2\.1-only AND BSD-3-Clause' does not name MIT" `
        'D1: a compiled MIT part under "LGPL-2.1-only AND BSD-3-Clause" (LibRaw before 2026-10-16) is an error'
    $sourceOnly = [pscustomobject]@{ name = 'Source only'; copyright = 'Copyright (c) 2010, X'; licence = 'BSD-2-Clause'; member = 'src/x.c'; first = 'a'; last = 'b'; as = 'X.txt'; compiled = $false }
    Assert-True (@(Test-PartLicences -Port 'pkg' -Concluded 'LGPL-2.1-only' -Parts @($sourceOnly)).Count -eq 0) 'D1: a part that is not compiled into the files need not be in the concluded expression'
    Assert-Problem (Test-PartLicences -Port 'pkg' -Concluded 'MIT' -Parts @([pscustomobject]@{ name = 'Incomplete'; licence = 'MIT' })) "the part 'Incomplete' of pkg has no copyright, member, first, last, as" `
        'D2: a part without its copyright line, member, markers or file is refused'
    Assert-Problem (Test-PartLicences -Port 'pkg' -Concluded 'MIT' -Parts @([pscustomobject]@{ name = 'Path'; copyright = 'c'; licence = 'MIT'; member = 'm'; first = 'f'; last = 'l'; as = '..\x.txt' })) `
        'not a plain \.txt file name' 'D2: a part file name with a folder is refused'

    # 11d. LibRaw's entry in the policy: its parts, an exact expression, and LGPL with its source provision.
    $libraw = $realPolicy.ports.libraw
    Assert-True ($libraw.concluded -ceq 'LGPL-2.1-only AND BSD-3-Clause AND MIT') "D1: LibRaw's concluded licence names its MIT code ($($libraw.concluded))"
    Assert-True (@(Test-PartLicences -Port 'libraw' -Concluded $libraw.concluded -Parts @($libraw.parts)).Count -eq 0) 'D1: LibRaw''s parts and its concluded licence agree'
    Assert-True ((@($libraw.parts | ForEach-Object { $_.as }) -join '|') -ceq 'DCB-FBDD-BSD-3-Clause.txt|Adobe-DNG-SDK-MIT.txt|DNGLab-derived-decoders.txt|X3F-BSD-3-Clause.txt') 'D2/round 2: LibRaw''s parts (DCB/FBDD, the Adobe DNG SDK code, the DNGLab-derived decoders, X3F) each get their notice'
    Assert-True ((Get-LicenceKind $libraw.concluded) -ceq 'copyleft' -and (Test-CopyleftLicence $libraw.concluded)) 'D1: the copyleft guard still treats LibRaw as LGPL'
    $plan = Get-SourceProvisionPlan -Policy $realPolicy -Components @((New-TestComponent 'libraw' $libraw.concluded $true))
    Assert-True (@($plan.Errors).Count -eq 0 -and (@($plan.Components | ForEach-Object Port) -join ',') -ceq 'libraw') 'D1: LibRaw marked "lgpl" gets its source provision under the new expression'
    $plan = Get-SourceProvisionPlan -Policy ([pscustomobject]@{ ports = [pscustomobject]@{} }) -Components @((New-TestComponent 'libraw' $libraw.concluded $false))
    Assert-Problem $plan.Errors 'libraw .*its licence LGPL-2\.1-only AND BSD-3-Clause AND MIT.*does not provide its corresponding source' 'D1: LibRaw without its mark is refused under the new expression'
    $mitPart = @($libraw.parts | Where-Object { $_.licence -eq 'MIT' })[0]
    Assert-True (-not ($mitPart.PSObject.Properties.Name -contains 'terms') -and (@($mitPart.textIn | ForEach-Object { $_.file }) -join '|') -ceq 'nlohmann-json\copyright.txt|nlohmann-json\UTF-8-decoder-MIT.txt') `
        'Round 2: the Adobe DNG SDK code''s MIT text is nlohmann-json''s licence files (verified sources), not words in the policy'

    # 11e. vcpkg's licence is LICENSE.txt of the registry at the baseline, by its git blob (a test registry).
    $registry = Join-Path $work 'registry'
    New-Item -ItemType Directory -Path $registry | Out-Null
    $registryLicence = "MIT License`n`nCopyright (c) Example Corporation`n`n$mitTerms`n"
    [System.IO.File]::WriteAllText((Join-Path $registry 'LICENSE.txt'), $registryLicence, $utf8)
    # The registry's state is a git tree (written from the index; no commit, so no signing or hooks), which
    # stands for the baseline commit: "<tree>:LICENSE.txt" names the file as "<commit>:LICENSE.txt" does.
    $gitArgs = @('-C', $registry, '-c', 'core.autocrlf=false')
    $null = Invoke-NativeCommand -FilePath 'git' -Arguments ($gitArgs + @('init', '-q'))
    $null = Invoke-NativeCommand -FilePath 'git' -Arguments ($gitArgs + @('add', 'LICENSE.txt'))
    $written = Invoke-NativeCommand -FilePath 'git' -Arguments ($gitArgs + @('write-tree'))
    $head = if ($written.ExitCode -eq 0 -and @($written.Output).Count -gt 0) { ([string]$written.Output[0]).Trim() } else { '' }
    Assert-True ($head -match '^[0-9a-f]{40}$') "D9: a test registry is written ($(@($written.Output) -join ' '))"
    if ($head -match '^[0-9a-f]{40}$') {
        $licence = Get-VcpkgLicence -VcpkgRoot $registry -Baseline $head
        Assert-True ($licence.Copyright -ceq 'Copyright (c) Example Corporation' -and $utf8.GetString($licence.Bytes) -ceq $registryLicence -and $licence.BlobId -match '^[0-9a-f]{40}$') `
            'D9: vcpkg''s licence is the registry''s file at the baseline, with its copyright line'
        [System.IO.File]::WriteAllText((Join-Path $registry 'LICENSE.txt'), $registryLicence.Replace("`n", "`r`n"), $utf8)
        Assert-True ($utf8.GetString((Get-VcpkgLicence -VcpkgRoot $registry -Baseline $head).Bytes) -ceq $registryLicence) 'D9: a checkout with other line endings gives the blob''s bytes (the same archive on every machine)'
        [System.IO.File]::WriteAllText((Join-Path $registry 'LICENSE.txt'), $registryLicence.Replace('Example', 'Other'), $utf8)
        Assert-Throws { Get-VcpkgLicence -VcpkgRoot $registry -Baseline $head } 'D9: a LICENSE.txt that is not the baseline''s is refused'
        Remove-Item -LiteralPath (Join-Path $registry 'LICENSE.txt')
        Assert-Throws { Get-VcpkgLicence -VcpkgRoot $registry -Baseline $head } 'D9: a missing LICENSE.txt is refused'
    }

    # 11f. The licence of each file of a shipped recipe: vcpkg's MIT, Lucent's changes MIT, a patch also
    # under its library's licence; never NOASSERTION.
    $recipeArgs = @{ Port = 'glib'; LibraryLicence = 'LGPL-2.1-or-later'; VcpkgCopyright = 'Copyright (c) Microsoft Corporation'; LucentCopyright = 'Copyright (c) 2026 Fan Zhu'; Baseline = ('e143402b' + '0' * 32) }
    $recipeCases = @(
        @{ File = 'vcpkg.json'; Status = 'unchanged'; Licence = 'MIT'; Holders = 'Copyright (c) Microsoft Corporation' },
        @{ File = 'usage'; Status = 'line endings'; Licence = 'MIT'; Holders = 'Copyright (c) Microsoft Corporation' },
        @{ File = 'portfile.cmake'; Status = 'changed'; Licence = 'MIT'; Holders = "Copyright (c) Microsoft Corporation`nCopyright (c) 2026 Fan Zhu" },
        @{ File = 'lucent-neutral-locale-dir.patch'; Status = 'added'; Licence = 'MIT AND LGPL-2.1-or-later'; Holders = 'Copyright (c) 2026 Fan Zhu' },
        @{ File = 'use-libiconv-on-windows.patch'; Status = 'unchanged'; Licence = 'MIT AND LGPL-2.1-or-later'; Holders = 'Copyright (c) Microsoft Corporation' })
    foreach ($case in $recipeCases) {
        $result = Get-RecipeFileLicence -File $case.File -Status $case.Status @recipeArgs
        Assert-True ($result.Licence -ceq $case.Licence -and $result.Copyright -ceq $case.Holders -and $result.Comment) `
            "D9: the recipe file $($case.File) ($($case.Status)) is $($case.Licence) ($($result.Licence); $($result.Copyright -replace "`n", ' / '))"
    }
    $patchCases = [ordered]@{ 'LGPL-2.1-or-later AND MPL-1.1' = 'MIT AND LGPL-2.1-or-later AND MPL-1.1'; 'LGPL-2.1-only OR MPL-1.1' = 'MIT AND (LGPL-2.1-only OR MPL-1.1)'
        'MIT' = 'MIT'; 'MIT-Modern-Variant AND MIT' = 'MIT AND MIT-Modern-Variant'; 'BSD-3-Clause WITH PCRE2-exception' = 'MIT AND BSD-3-Clause WITH PCRE2-exception' }
    foreach ($case in $patchCases.GetEnumerator()) {
        $patchLicence = (Get-RecipeFileLicence -File 'fix.patch' -Status 'unchanged' -Port 'x' -LibraryLicence $case.Key -VcpkgCopyright 'c' -LucentCopyright 'l').Licence
        Assert-True ($patchLicence -ceq $case.Value) "D9/R9: a patch of a library under '$($case.Key)' is '$($case.Value)', each licence once, a choice grouped ($patchLicence)"
    }
    Assert-Throws { Get-RecipeFileLicence -File 'gone.patch' -Status 'removed' @recipeArgs } 'D9: a file the overlay removed has no licence to give'

    # 11g. Lucent's own files in the SBOM: its programs, its data and generated files, the recipes, the
    # licence texts.
    $sbomInfo = [pscustomobject]@{ executable = 'Lucent.exe'; helperExecutables = @('lucent-worker.exe') }
    $assetRecords = @(
        [pscustomobject]@{ Id = 'lens-database'; Folder = 'assets\lens-database'; ThirdParty = $true; Concluded = 'CC-BY-SA-3.0'; Copyright = 'Copyright (c) the Lensfun project'; LucentFiles = @('manifest.json', 'provenance.json') },
        [pscustomobject]@{ Id = 'camera-support'; Folder = 'assets\camera-support'; ThirdParty = $false; Concluded = 'MIT'; Copyright = $null; LucentFiles = @('manifest.json') })
    $recipeRecords = @([pscustomobject]@{ ShippedRecipe = 'licenses\glib\recipe'; RecipeFiles = @(
        [pscustomobject]@{ File = 'portfile.cmake'; Licence = 'MIT'; Copyright = "Copyright (c) Microsoft Corporation`nCopyright (c) 2026 Fan Zhu"; Comment = 'changed' },
        [pscustomobject]@{ File = 'detect/CMakeLists.txt'; Licence = 'MIT'; Copyright = 'Copyright (c) Microsoft Corporation'; Comment = 'unchanged' },
        [pscustomobject]@{ File = 'lucent-neutral-locale-dir.patch'; Licence = 'MIT AND LGPL-2.1-or-later'; Copyright = 'Copyright (c) 2026 Fan Zhu'; Comment = 'added' }) })
    $ownCases = [ordered]@{
        'Lucent.exe' = 'MIT AND LicenseRef-Logo'; 'lucent-worker.exe' = 'MIT AND LicenseRef-Logo'; 'qt.conf' = 'MIT'; 'Lucent-files.txt' = 'MIT'
        'assets\camera-support\camera-support.json' = 'MIT'; 'assets\lens-database\mil-zeiss.xml' = 'CC-BY-SA-3.0'; 'assets\lens-database\provenance.json' = 'MIT'
        'licenses\glib\recipe\portfile.cmake' = 'MIT'; 'licenses\glib\recipe\detect\CMakeLists.txt' = 'MIT'; 'licenses\glib\recipe\lucent-neutral-locale-dir.patch' = 'MIT AND LGPL-2.1-or-later'
        'licenses\THIRD-PARTY-NOTICES.txt' = 'NOASSERTION'; 'licenses\libraw\copyright.txt' = 'NOASSERTION' }
    foreach ($case in $ownCases.GetEnumerator()) {
        $own = Get-OwnFileLicence -Relative $case.Key -Info $sbomInfo -ProgramLicence 'MIT AND LicenseRef-Logo' -AssetLicences $assetRecords -Recipes $recipeRecords
        Assert-True ($own.Licence -ceq $case.Value) "D9/D10: the SBOM gives $($case.Key) $($case.Value) ($($own.Licence))"
    }
    $own = Get-OwnFileLicence -Relative 'licenses\glib\recipe\portfile.cmake' -Info $sbomInfo -ProgramLicence 'MIT' -AssetLicences $assetRecords -Recipes $recipeRecords
    Assert-True ($own.Copyright -ceq "Copyright (c) Microsoft Corporation`nCopyright (c) 2026 Fan Zhu" -and $own.Comment -ceq 'changed') 'D9: a recipe file carries its holders and comment into the SBOM'
    Assert-True ((Get-OwnFileLicence -Relative 'assets\lens-database\mil-zeiss.xml' -Info $sbomInfo -ProgramLicence 'MIT' -AssetLicences $assetRecords).Asset -ceq 'lens-database') 'a third-party asset''s file is linked to its asset'
    Assert-Throws { Get-OwnFileLicence -Relative 'licenses\glib\recipe\unknown.patch' -Info $sbomInfo -ProgramLicence 'MIT' -AssetLicences $assetRecords -Recipes $recipeRecords } `
        'D9: a file in a shipped recipe that its overlay port does not hold is refused'

    # 11h. THIRD-PARTY-NOTICES.txt names the versions the package manifest records, and the recipes'
    # licences (the template of the repository).
    $noticesTemplate = [System.IO.File]::ReadAllText((Join-Path $repoRoot 'installer\notices\THIRD-PARTY-NOTICES.txt.in'))
    $noticeArgs = @{ Template = $noticesTemplate; Version = '9.8.7'; Logo = 'The logo.'; Notices = @('- A credit.'); Components = @('liba 1.0', '  Licence: MIT'); Data = @('(none)')
                     Redistributable = 'Microsoft Visual C++ Redistributable (x64) 14.44.35211.0' }
    $withAll = Complete-ThirdPartyNotices @noticeArgs -InnoSetup 'Inno Setup 6.7.3' -Recipes '- The build recipes in licenses\glib\recipe are vcpkg''s (vcpkg-LICENSE.txt).' `
        -Programs 'Lucent.exe and lucent-worker.exe' -InnoSetupNote '  Its text is inno-setup\license.txt in this folder.'
    $flatNotices = $withAll -replace '\s+', ' '
    Assert-True ($flatNotices.Contains('Microsoft Visual C++ Redistributable (x64) 14.44.35211.0 (vc_redist.x64.exe)') -and $flatNotices.Contains('are made with Inno Setup 6.7.3, copyright')) `
        'D3/D4: the notices name the redistributable''s and Inno Setup''s versions'
    Assert-True ($withAll.Contains("`n- The build recipes in licenses\glib\recipe are vcpkg's (vcpkg-LICENSE.txt).`n") -and $withAll -notmatch '@[A-Z_]+@') 'D9: the recipes'' item is in the notices, every placeholder filled'
    Assert-True ($flatNotices.Contains('whose code is part of Lucent.exe and lucent-worker.exe;') -and -not $flatNotices.Contains('part of Lucent.exe;')) 'R7: compiled-in code is named as part of both programs'
    Assert-True ($withAll.Contains("free software.`n  Its text is inno-setup\license.txt in this folder.`n- The build recipes")) 'R14: the notices say where Inno Setup''s licence text is'
    $without = Complete-ThirdPartyNotices @noticeArgs -InnoSetup '' -Recipes '' -Programs 'Lucent.exe' -InnoSetupNote ''
    Assert-True (($without -replace '\s+', ' ').Contains('are made with Inno Setup, copyright') -and -not $without.Contains('@RECIPES@') -and -not $without.Contains('@INNO_SETUP_LICENCE@') -and -not $without.Contains("free software.`n`n`n")) `
        'D4: without a compiler (-StageOnly) Inno Setup is named without a version, and without recipes or a licence note their lines go'
    $noRedist = $noticeArgs.Clone()
    $noRedist['Template'] = $noticesTemplate.Replace('@VC_REDIST@', 'the runtime')
    Assert-Throws { Complete-ThirdPartyNotices @noRedist -InnoSetup '' -Recipes '' -Programs 'Lucent.exe' } 'D3: a template that does not name the redistributable''s version is refused'
    $noPrograms = $noticeArgs.Clone()
    $noPrograms['Template'] = $noticesTemplate.Replace('@PROGRAMS@', 'Lucent.exe')
    Assert-Throws { Complete-ThirdPartyNotices @noPrograms -InnoSetup '' -Recipes '' -Programs 'Lucent.exe and lucent-worker.exe' } 'R7: a template that names one program for the compiled-in code is refused'
    Assert-True ((Get-InnoSetupEngineVersion -Output $banners['6.7.3']) -ceq 'Inno Setup 6.7.3') 'D4: the engine version is read from the compiler''s banner'
    Assert-True ($null -eq (Get-InnoSetupEngineVersion -Output @('Inno Setup 6 Command-Line Compiler', 'Usage:  iscc [options] scriptfile.iss'))) 'D4: a banner without the engine line (what /? prints) gives none'
    $recipeNotice = Format-RecipeNotice -Folders @('licenses\glib\recipe', 'licenses\gettext-libintl\recipe') -Baseline ('e143402b' + '0' * 32) -VcpkgLicence ([pscustomobject]@{ Copyright = 'Copyright (c) Microsoft Corporation' })
    Assert-True ($recipeNotice.StartsWith('- The build recipes in licenses\gettext-libintl\recipe and licenses\glib\recipe are vcpkg') -and ($recipeNotice -replace '\s+', ' ').Contains("vcpkg's MIT License (Copyright (c) Microsoft Corporation; vcpkg-LICENSE.txt in this folder")) `
        "D9: the notices' item names the recipes and vcpkg's licence beside them"
    Assert-True ((Format-RecipeNotice -Folders @() -Baseline 'x' -VcpkgLicence $null) -ceq '') 'D9: without shipped recipes there is no such item'

    # 11i. The data the notices name: Lucent's own assets too.
    $dataLines = @(Format-DataNotices -AssetLicences @(
        [pscustomobject]@{ Id = 'lens-database'; Version = '1.0.0'; Folder = 'assets\lens-database'; ThirdParty = $true; Name = 'Lensfun lens database'; Concluded = 'CC-BY-SA-3.0'
                           LicenseFile = 'COPYING.CC_BY-SA_3.0'; Homepage = $null; Copyright = 'Copyright (c) the Lensfun project'; Comment = $null; Provenance = $null },
        [pscustomobject]@{ Id = 'camera-support'; Version = '3.0.1'; Folder = 'assets\camera-support'; ThirdParty = $false; Name = 'Camera support database'; Concluded = 'MIT'
                           LicenseFile = $null; Homepage = $null; Copyright = $null; Comment = 'The cameras Lucent supports.'; Provenance = $null }))
    $dataText = $dataLines -join "`n"
    Assert-True ($dataLines[0] -ceq 'Camera support database, asset camera-support 3.0.1, in assets\camera-support' -and $dataText.Contains("  Licence: MIT`n  Lucent's own data, under Lucent's MIT License (Lucent-LICENSE.txt in this folder).")) `
        'D5: Lucent''s own data asset is named under "Data" with its licence'
    Assert-True ($dataText.Contains('Lensfun lens database, asset lens-database 1.0.0, in assets\lens-database') -and $dataText.Contains('Licence text: lens-database\COPYING.CC_BY-SA_3.0')) 'D5: a third-party asset keeps its credit and licence text'
    Assert-True ($realPolicy.lucentAssets.'camera-support'.name -ceq 'Camera support database') 'D5: the policy names Lucent''s camera support database'

    # 11j. Each library's note names every patch that changes more than build files (qtbase's left out four).
    foreach ($file in 'CMakeLists.txt', 'cmake/QtBuildPathsHelpers.cmake', 'cmake/qt.toolchain.cmake.in', 'src/corelib/configure.cmake', 'meson.build', 'cmake/data/libraw.pc.cmake') {
        Assert-True (Test-BuildFile $file) "D6: $file is a build file"
    }
    foreach ($file in 'src/gui/text/qtextmarkdownimporter.cpp', 'src/corelib/platform/windows/qt_winrtbase_p.h', 'cmake/modulecppexports.h.in', 'srclib/fcntl.c') {
        Assert-True (-not (Test-BuildFile $file)) "D6: $file is code"
    }
    $noteComponents = @([pscustomobject]@{ Port = 'qtlike'; Modifications = @(
        [ordered]@{ patch = 'ports/qtlike/cmake-only.patch'; changes = @('cmake/A.cmake', 'CMakeLists.txt') },
        [ordered]@{ patch = 'ports/qtlike/named.diff'; changes = @('src/a.cpp') },
        [ordered]@{ patch = 'ports/qtlike/by-files.patch'; changes = @('src/b.c', 'configure.cmake') },
        [ordered]@{ patch = 'ports/qtlike/silence-warnings.diff'; changes = @('src/corelib/configure.cmake', 'src/corelib/platform/windows/qt_winrtbase_p.h') }) })
    $notePolicy = [pscustomobject]@{ ports = [pscustomobject]@{ qtlike = [pscustomobject]@{ reviewedVersion = '1.0'; comment = 'Mostly CMake build files; named.diff changes src/a.cpp; another patch changes src/b.c.' } } }
    $noteErrors = @(Test-ModificationNotes -Components $noteComponents -Policy $notePolicy)
    Assert-True ($noteErrors.Count -eq 1) "D6: one patch that changes code is left out of the note ($($noteErrors -join ' | '))"
    Assert-Problem $noteErrors 'does not name silence-warnings\.diff, which changes src/corelib/platform/windows/qt_winrtbase_p\.h' 'D6: a note that leaves out a patch changing a header is refused (qtbase before 2026-10-16)'
    $notePolicy.ports.qtlike.comment += ' silence-warnings.diff adds a #define.'
    Assert-True (@(Test-ModificationNotes -Components $noteComponents -Policy $notePolicy).Count -eq 0) 'D6: a note that names every code-changing patch passes'
    foreach ($patch in 'md4c.diff', 'silence-winrtbase-coroutine-warnings.diff', 'use_inotify_on_freebsd.patch', 'macdeployqt-symlinks.patch', 'windeployqt-webengine-debug-resources.patch') {
        Assert-True ($realPolicy.ports.qtbase.comment.Contains($patch)) "D6: the qtbase note names $patch"
    }
    Assert-True ($realPolicy.ports.libiconv.comment.Contains('lib/compat.c')) 'D8: the libiconv note names lib/compat.c among iconv-2.dll''s sources'

    # 12. The gate-8 review, round 1 (R1 to R14) and its sweep of every shipped component: parts from any of
    # a port's archives, with their licences' texts; the parts the review named, in the policy and refused
    # without their licences; Qt's third-party code against each Qt component's licence; the compiler's own
    # library code in the programs; Inno Setup's licence; a recipe's scripted edits; the wording of the notices.

    # 12a. A notice that ends with a word its earlier lines hold too (HarfBuzz's src/ms-use/COPYING ends "SOFTWARE").
    $copying = "    MIT License`n`n    Copyright (c) Microsoft Corporation.`n`n    THE SOFTWARE IS PROVIDED AS IS ... OTHER DEALINGS IN THE`n    SOFTWARE`n"
    $whole = Get-LicenceExcerpt -Text $copying -First 'Copyright (c) Microsoft Corporation.' -Last 'SOFTWARE' -LastWholeLine
    $partial = Get-LicenceExcerpt -Text $copying -First 'Copyright (c) Microsoft Corporation.' -Last 'SOFTWARE'
    Assert-True ($whole.FirstLine -eq 3 -and $whole.LastLine -eq 6 -and $partial.LastLine -eq 5) "Sweep: -LastWholeLine ends at the line that is the word (lines $($whole.FirstLine)-$($whole.LastLine); without it $($partial.LastLine))"

    # 12b. R2: a part in the second archive of a port is taken from that archive (PCRE2's JIT compiler).
    foreach ($pair in @(@('two-a', 'liba-1.0', 'library terms'), @('two-b', 'sljit-abc', 'jit terms'))) {
        New-Item -ItemType Directory -Path (Join-Path $work "$($pair[0])\$($pair[1])") -Force | Out-Null
        [System.IO.File]::WriteAllText((Join-Path $work "$($pair[0])\$($pair[1])\LICENSE"), "$($pair[2])`n", $utf8)
    }
    $tarA = Join-Path $downloadsDir 'example-liba-v1.0.tar.gz'
    $tarB = Join-Path $downloadsDir 'example-sljit-abc.tar.gz'
    & (Join-Path $env:SystemRoot 'System32\tar.exe') -czf $tarA -C (Join-Path $work 'two-a') 'liba-1.0'
    & (Join-Path $env:SystemRoot 'System32\tar.exe') -czf $tarB -C (Join-Path $work 'two-b') 'sljit-abc'
    $twoSources = @(
        foreach ($item in @(@('example/liba', 'git+https://github.com/example/liba@v1.0', $tarA), @('example/sljit', 'git+https://github.com/example/sljit@abc', $tarB))) {
            $sha = (Get-FileHash -LiteralPath $item[2] -Algorithm SHA512).Hash.ToLowerInvariant()
            [pscustomobject]@{ Name = $item[0]; Location = $item[1]; Sha512 = $sha
                Resource = [pscustomobject]@{ name = $item[0]; downloadLocation = $item[1]; packageFileName = [System.IO.Path]::GetFileName($item[2])
                    checksums = @([pscustomobject]@{ algorithm = 'SHA512'; checksumValue = $sha }) } }
        })
    $first = Find-SourceArchiveMember -Sources $twoSources -Member 'LICENSE' -DownloadsDir $downloadsDir
    $named = Find-SourceArchiveMember -Sources $twoSources -Member 'LICENSE' -DownloadsDir $downloadsDir -SourceName 'example/sljit'
    Assert-True ($first.Text -ceq "library terms`n" -and $named.Text -ceq "jit terms`n" -and $named.Source -ceq 'example/sljit' -and $named.Archive -ceq 'example-sljit-abc.tar.gz') `
        'R2: a part names the archive of its port that holds it (the first archive would have been taken otherwise)'

    # 12c. Licences in expressions: a term of its own, with its exception, or one alternative of a choice.
    $inExpression = [ordered]@{
        'MIT|FTL AND MIT AND MIT-open-group' = $true; 'MIT-open-group|FTL AND MIT' = $false; 'BSD-2-Clause|BSD-3-Clause WITH PCRE2-exception AND BSD-2-Clause' = $true
        'GPL-3.0-or-later WITH Bison-exception-2.2|LGPL-2.1-or-later AND GPL-3.0-or-later WITH Bison-exception-2.2' = $true
        'GPL-3.0-or-later|LGPL-2.1-or-later AND GPL-3.0-or-later WITH Bison-exception-2.2' = $false; 'LGPL-2.1-or-later|LGPL-3.0-only' = $false }
    foreach ($case in $inExpression.GetEnumerator()) {
        $licence, $expression = $case.Key -split '\|'
        Assert-True ((Test-LicenceInExpression -Licence $licence -Expression $expression) -eq $case.Value) "Test-LicenceInExpression '$licence' in '$expression' is $($case.Value)"
    }
    $covered = [ordered]@{
        'FTL OR GPL-2.0-only|LGPL-3.0-only AND FTL' = $true; 'CC0-1.0 OR Apache-2.0|LGPL-3.0-only AND CC0-1.0' = $true; 'X11 AND HPND|LGPL-3.0-only AND X11' = $false
        'X11 AND HPND|LGPL-3.0-only AND HPND AND X11' = $true; 'MPL-2.0|LGPL-3.0-only' = $false; 'NOASSERTION|MIT' = $false }
    foreach ($case in $covered.GetEnumerator()) {
        $licence, $expression = $case.Key -split '\|'
        Assert-True ((Test-LicenceCovered -Licence $licence -Expression $expression) -eq $case.Value) "Test-LicenceCovered '$licence' by '$expression' is $($case.Value)"
    }

    # 12d. The parts the review found, refused under the licences the package had (R1 to R5), accepted under
    # the policy's; a part's licence may carry an exception; its text and textIn are checked.
    $regressions = @(
        @('R1', 'freetype', 'FTL', 'MIT'), @('R1', 'freetype', 'FTL AND MIT', 'MIT-open-group'), @('R2', 'pcre2', 'BSD-3-Clause WITH PCRE2-exception', 'BSD-2-Clause'),
        @('R3', 'nlohmann-json', 'MIT', 'CC0-1.0'), @('R4', 'lensfun', 'LGPL-3.0-only', 'LGPL-2.1-or-later'), @('R5', 'tiff', 'libtiff', 'BSD-4.3TAHOE'),
        @('Sweep', 'harfbuzz', 'MIT AND MIT-Modern-Variant', 'ISC'), @('Sweep', 'libjpeg-turbo', 'BSD-3-Clause AND IJG', 'Zlib'),
        @('Sweep', 'gettext-libintl', 'LGPL-2.1-or-later', 'GPL-3.0-or-later WITH Bison-exception-2.2'))
    foreach ($case in $regressions) {
        $entry = $realPolicy.ports.($case[1])
        Assert-Problem (Test-PartLicences -Port $case[1] -Concluded $case[2] -Parts @($entry.parts)) ("is compiled into its files under " + [regex]::Escape($case[3]) + ", but the port's concluded licence") `
            "$($case[0]): $($case[1]) under '$($case[2])' leaves out its parts' $($case[3]), and is refused"
        Assert-True (@(Test-PartLicences -Port $case[1] -Concluded $entry.concluded -Parts @($entry.parts)).Count -eq 0) "$($case[0]): the policy's parts of $($case[1]) and its licence '$($entry.concluded)' agree"
    }
    $withPart = [pscustomobject]@{ name = 'Skeleton'; copyright = 'c'; licence = 'GPL-3.0-or-later WITH Bison-exception-2.2'; member = 'm'; first = 'f'; last = 'l'; as = 'S.txt'; text = [pscustomobject]@{ member = 'COPYING' } }
    Assert-True (@(Test-PartLicences -Port 'x' -Concluded 'LGPL-2.1-or-later AND GPL-3.0-or-later WITH Bison-exception-2.2' -Parts @($withPart)).Count -eq 0) 'Sweep: a part''s licence may carry its exception'
    Assert-Problem (Test-PartLicences -Port 'x' -Concluded 'MIT' -Parts @([pscustomobject]@{ name = 'T'; copyright = 'c'; licence = 'MIT'; member = 'm'; first = 'f'; last = 'l'; as = 'T.txt'; text = [pscustomobject]@{ first = 'a' } })) `
        'names no member of the source archive' 'Sweep: a licence text without its member is refused'
    Assert-Problem (Test-PartLicences -Port 'x' -Concluded 'MIT' -Parts @([pscustomobject]@{ name = 'T'; copyright = 'c'; licence = 'MIT'; member = 'm'; first = 'f'; last = 'l'; as = 'T.txt'; terms = @('words') })) `
        "gives its licence's words as 'terms'" 'Round 2: a part whose licence text is words Lucent supplies (terms) is refused'
    Assert-Problem (Test-PartLicences -Port 'x' -Concluded 'MIT' -Parts @([pscustomobject]@{ name = 'T'; copyright = 'c'; licence = 'MIT'; member = 'm'; first = 'f'; last = 'l'; as = 'T.txt'; textIn = @([pscustomobject]@{ file = '..\x.txt'; marker = 'm' }) })) `
        'names a licence file that is not' 'Sweep: a licence file outside licenses\<folder>\ is refused'

    # 12e. The policy records what the review and its sweep found.
    $ports = $realPolicy.ports
    Assert-True ((@($ports.freetype.parts | ForEach-Object { "$($_.as)=$($_.licence)" }) -join '|') -ceq 'BDF-driver-MIT.txt=MIT|BDF-library-MIT.txt=MIT|PCF-driver-MIT.txt=MIT|PCF-utilities-MIT-open-group.txt=MIT-open-group|fthash-MIT.txt=MIT' -and $ports.freetype.concluded -ceq 'FTL AND MIT AND MIT-open-group') `
        "R1: FreeType's BDF and PCF drivers, X11's pcfutil.c and fthash.c are parts of it, and its licence names them ($($ports.freetype.concluded))"
    $sljit = @($ports.pcre2.parts)[0]
    Assert-True ($sljit.source -ceq 'zherczeg/sljit' -and $sljit.licence -ceq 'BSD-2-Clause' -and $sljit.version -match '^45f910b7' -and $ports.pcre2.concluded -ceq 'BSD-3-Clause WITH PCRE2-exception AND BSD-2-Clause') `
        'R2: PCRE2''s JIT compiler is a part from the sljit archive, BSD-2-Clause, with the commit as its version'
    Assert-True ((@($ports.'nlohmann-json'.parts | ForEach-Object { "$($_.licence)" }) -join '|') -ceq 'MIT|MIT|CC0-1.0' -and $ports.'nlohmann-json'.comment.Contains('JSON_HAS_CPP_14') -and
                 $ports.'nlohmann-json'.comment.Contains('Abseil') -and @($ports.'nlohmann-json'.parts)[2].version -ceq '15') `
        'R3: nlohmann-json names Hoehrmann''s decoder and Loitsch''s Grisu2 (MIT) and Hedley (CC0-1.0, its version 15), and records why the Abseil code is not compiled'
    $tre = @($ports.lensfun.parts)[0]
    Assert-True ($tre.licence -ceq 'LGPL-2.1-or-later' -and $tre.member -ceq 'libs/regex/regcomp.c' -and $ports.lensfun.comment.Contains('tre_regex') -and @($tre.textIn | ForEach-Object { $_.file }) -contains 'gettext-libintl\copyright.txt') `
        'R4: Lensfun''s TRE subset is a part, LGPL-2.1-or-later, compiled as tre_regex, its licence text named'
    Assert-True (@($ports.tiff.parts | Where-Object { $_.licence -ceq 'BSD-4.3TAHOE' -and $_.member -ceq 'LICENSE.md' }).Count -eq 1 -and $ports.tiff.concluded -ceq 'libtiff AND BSD-4.3TAHOE AND MIT') `
        'R5: libtiff''s LZW code is a part under the Berkeley notice (BSD-4.3TAHOE), and its licence names it'
    $bashify = @($ports.'gettext-libintl'.scriptedEdits)[0]
    Assert-True ($bashify.script -ceq 'bashify.cmake' -and (@($bashify.changes) -join '|') -ceq 'gettext-runtime/configure') 'R6: GNU libintl''s recipe names bashify.cmake and the build script it rewrites'
    Assert-True ($ports.qtbase.comment.Contains('to its configure test')) 'R10: qtbase''s note names silence-winrtbase-coroutine-warnings.diff''s configure test'
    Assert-True (-not $ports.libiconv.comment.Contains('lib/compat.c added by') -and $ports.libiconv.comment.Contains('lib/compat.c')) 'R11: libiconv''s note names lib/compat.c without reading as Lucent adding it'
    $termsClaims = @($ports.PSObject.Properties | Where-Object { ($_.Value.PSObject.Properties.Name -contains 'comment') -and [string]$_.Value.comment -cmatch "with their terms from (its|[A-Za-z0-9-]+'s) source archive" } | ForEach-Object Name)
    Assert-True ($termsClaims.Count -eq 0) "R11: no port's note says all its parts' terms come from its source archive (LibRaw's Adobe MIT text is Lucent's) ($($termsClaims -join ', '))"
    $adobe = @($ports.libraw.parts | Where-Object { $_.licence -ceq 'MIT' })[0]
    $dcb = @($ports.libraw.parts)[0]
    Assert-True ($adobe.version -ceq '1.4' -and -not ($dcb.PSObject.Properties.Name -contains 'version')) 'R13: the Adobe DNG SDK code has its own version (1.4), DCB/FBDD none'
    $tool = $realPolicy.distribution.installerTool
    Assert-True ($tool.licenseId -ceq 'InnoSetup' -and $tool.reviewedVersion -ceq '6.7.3' -and $tool.licenseFile -ceq 'license.txt' -and $tool.licenseSha256 -match '^[0-9a-f]{64}$') 'R14: the policy pins the licence of the reviewed Inno Setup'
    Assert-True ($ports.qtbase.attributionsNotCompiled.PSObject.Properties.Name -contains 'vulkanmemoryallocator' -and $ports.qtdeclarative.concluded -ceq 'LGPL-3.0-only AND BSD-2-Clause AND MIT' -and $ports.qtsvg.concluded -ceq 'LGPL-3.0-only AND HPND-sell-variant') `
        'Sweep: the Qt components'' licences cover the code Qt''s SBOM records, and qtbase names what it does not compile'
    $stl = $realPolicy.toolchainLibraries.'msvc-stl'
    Assert-True ((@($stl.parts | ForEach-Object { $_.licence }) -join '|') -ceq 'Unicode-DFS-2016|MIT WITH fmt-exception|BSL-1.0' -and @($stl.parts | Where-Object { $_.sha256 -notmatch '^[0-9a-f]{64}$' }).Count -eq 0 -and
                 @(Test-PartLicences -Port 'msvc-stl' -Concluded $stl.concluded -Parts @($stl.parts)).Count -eq 0) 'Sweep: the C++ standard library''s parts (Unicode tables, {fmt}, Ryu) are pinned by their headers'' SHA-256'
    $unheld = @(foreach ($property in @($ports.PSObject.Properties)) {
        foreach ($part in @(if ($property.Value.PSObject.Properties.Name -contains 'parts') { $property.Value.parts })) {
            foreach ($where in @(if ($part.PSObject.Properties.Name -contains 'textIn') { $part.textIn })) {
                if (-not ($where.PSObject.Properties.Name -contains 'holds') -or -not $where.holds) { "$($property.Name): $($where.file)" } } } })
    Assert-True ($unheld.Count -eq 0) "Sweep: each licence file of another component that holds a part's licence says what it holds ($($unheld -join ', '))"
    $ccitt = @($ports.tiff.parts | Where-Object { $_.as -ceq 'CCITT-fax3-libtiff.txt' })[0]
    Assert-True ($ccitt.last -ceq 'OF THIS SOFTWARE.' -and $ccitt.text.member -ceq 'libtiff/tif_fax3.c' -and $ccitt.text.about) 'Sweep: libtiff''s fax decoder takes its notice and Frank Cringle''s copyright as two excerpts, without the code between them'
    foreach ($port in @($ports.PSObject.Properties.Name)) {
        $entry = $ports.$port
        if (-not ($entry.PSObject.Properties.Name -contains 'libraryLicence')) { continue }
        Assert-True (Test-LicenceCovered -Licence $entry.libraryLicence -Expression $entry.concluded) "Sweep: the library licence of $port ($($entry.libraryLicence)) is part of its licence"
    }

    # 12f. A part's file: its notice, then its licence's text from the same archive, Lucent's words, or the
    # files other components ship, each with what it holds; each says where it comes from (R11).
    $textPart = [pscustomobject]@{ name = 'Decoder'; version = '2'; copyright = 'Copyright (c) 2009 Someone'; licence = 'MIT'; member = 'README.md'; first = 'x'; last = 'x'; as = 'D.txt'
        textIn = @([pscustomobject]@{ file = 'tl-expected\copyright.txt'; marker = 'CC0 1.0 Universal'; holds = 'the CC0 legal code' }, [pscustomobject]@{ file = 'qtbase\copyright.txt'; marker = 'UNICODE LICENSE V3' }) }
    $textPartText = Format-PartLicenceText -Part $textPart -Port 'pkg' -Version '1.0' -ArchiveName 'pkg-1.0.tar.gz' -ArchiveSha512 ('ab' * 64) `
        -Excerpt ([pscustomobject]@{ Lines = @('- The class contains the decoder.'); FirstLine = 7; LastLine = 7 }) `
        -TextExcerpt ([pscustomobject]@{ Lines = @('Permission is hereby granted, for the test.'); FirstLine = 5; LastLine = 9 }) -TextMember 'LICENSES/MIT.txt'
    $flatText = $textPartText -replace '\s+', ' '
    Assert-True ($textPartText.StartsWith("pkg 1.0: Decoder, version 2, MIT`n") -and $flatText.Contains('What README.md says of it, line 7, in pkg-1.0.tar.gz') -and
                 $flatText.Contains('The text of the MIT licence, lines 5 to 9 of LICENSES/MIT.txt in the same archive:') -and $textPartText.Contains("`nPermission is hereby granted, for the test.`n") -and
                 $flatText.Contains("in other components' files: tl-expected\copyright.txt (the CC0 legal code) and qtbase\copyright.txt (at 'UNICODE LICENSE V3').")) `
        "Sweep: a part's file holds its notice (one line named as one) and its licence's text from the same archive, and names the licence files other components ship with what each holds"
    $aboutPart = [pscustomobject]@{ name = 'Fax'; copyright = 'c'; licence = 'libtiff'; member = 'f.c'; first = 'x'; last = 'y'; as = 'F.txt'
        text = [pscustomobject]@{ member = 'f.c'; first = 'a'; last = 'b'; about = "Someone's copyright on the decoder" } }
    $aboutText = (Format-PartLicenceText -Part $aboutPart -Port 'pkg' -Version '1.0' -ArchiveName 'pkg-1.0.tar.gz' -ArchiveSha512 ('ab' * 64) `
        -Excerpt ([pscustomobject]@{ Lines = @('x', 'y'); FirstLine = 2; LastLine = 3 }) -TextExcerpt ([pscustomobject]@{ Lines = @('a', 'b'); FirstLine = 30; LastLine = 31 }) -TextMember 'f.c') -replace '\s+', ' '
    Assert-True ($aboutText.Contains("Someone's copyright on the decoder, lines 30 to 31 of f.c in the same archive:") -and -not $aboutText.Contains('The text of the libtiff licence') -and $aboutText.Contains('Its terms as f.c states them, lines 2 to 3,')) `
        "Sweep: an excerpt that is not the licence's text says what it is, and the notice before it is the part's terms (libtiff's CCITT decoder: Frank Cringle's copyright)"
    $withText = (Format-PartLicenceText -Part $withPart -Port 'pkg' -Version '1.0' -ArchiveName 'pkg-1.0.tar.gz' -ArchiveSha512 ('ab' * 64) `
        -Excerpt ([pscustomobject]@{ Lines = @('notice'); FirstLine = 3; LastLine = 32 }) -TextExcerpt ([pscustomobject]@{ Lines = @('GNU GENERAL PUBLIC LICENSE') }) -TextMember 'COPYING') -replace '\s+', ' '
    Assert-True ($withText.Contains('The text of the GPL-3.0-or-later licence (without Bison-exception-2.2, its exception), COPYING in the same archive:')) `
        'Sweep: the text of a licence with an exception is named as the licence''s, without the exception (GNU libintl''s Bison skeleton)'
    Assert-Problem (Test-PartLicences -Port 'x' -Concluded 'MIT' -Parts @([pscustomobject]@{ name = 'T'; copyright = 'c'; licence = 'MIT'; member = 'm'; first = 'f'; last = 'l'; as = 'T.txt'; textIn = @([pscustomobject]@{ file = 'a\b.txt'; marker = 'm'; holds = '' }) })) `
        'says nothing of what' 'Sweep: a licence file whose "holds" is empty is refused'
    Assert-Problem (Test-PartLicences -Port 'x' -Concluded 'MIT' -Parts @([pscustomobject]@{ name = 'T'; copyright = 'c'; licence = 'MIT'; member = 'm'; first = 'f'; last = 'l'; as = 'T.txt'; text = [pscustomobject]@{ member = 'M'; about = '' } })) `
        "has an empty 'about'" 'Sweep: a text whose "about" is empty is refused'
    $termsParts = @(foreach ($property in @($ports.PSObject.Properties)) { foreach ($part in @(if ($property.Value.PSObject.Properties.Name -contains 'parts') { $property.Value.parts })) { if ($part.PSObject.Properties.Name -contains 'terms') { "$($property.Name): $($part.name)" } } })
    Assert-True ($termsParts.Count -eq 0) "R11/round 2: no part of the policy gives its licence's text as words of its own ($($termsParts -join ', '))"
    $lines = @(Format-PartLines -Parts @([pscustomobject]@{ Name = 'Hedley'; Version = '15'; Copyright = 'Copyright (c) Someone; Copyright (c) Another'; Licence = 'CC0-1.0'; File = 'nlohmann-json\Hedley-CC0-1.0.txt'; Compiled = $true }) -Inside 'Lucent.exe and lucent-worker.exe')
    Assert-True ($lines[0] -ceq '  Parts under their own licences, each with the file of its notice and terms:' -and (($lines -join ' ') -replace '\s+', ' ').Contains('Hedley, version 15, in Lucent.exe and lucent-worker.exe: CC0-1.0 (nlohmann-json\Hedley-CC0-1.0.txt)') -and
                 $lines[-2] -ceq '      Copyright (c) Someone' -and $lines[-1] -ceq '      Copyright (c) Another') `
        "R11/R3: the notices list parts under their own licences, with their own version, inside the programs for a header-only library, a holder to a line ($($lines -join ' / '))"

    # 12g. R7: LGPL-NOTICE.txt says that neither program links LGPL code statically.
    $r7Stage = Join-Path $work 'r7-notice'
    Write-LgplNotice -StageDir $r7Stage -Info ([pscustomobject]@{ version = '9.8.7'; homepage = 'https://example.org/lucent'; executable = 'Lucent.exe'; helperExecutables = @('lucent-worker.exe') }) `
        -Components @([pscustomobject]@{ Port = 'liba'; Lgpl = $false; StaticLibraries = @() }) -NoticesDir (Join-Path $repoRoot 'installer\notices') `
        -SourcePackage ([pscustomobject]@{ File = 'Lucent-9.8.7-corresponding-source.zip'; Size = [int64]1; Sha256 = ('ab' * 32); Components = @() }) -Baseline ('e143402b' + '0' * 32)
    $r7Text = [System.IO.File]::ReadAllText((Join-Path $r7Stage 'licenses\LGPL-NOTICE.txt')) -replace '\s+', ' '
    Assert-True ($r7Text.Contains('No LGPL-licensed code is linked statically into Lucent.exe and lucent-worker.exe.') -and $r7Text.Contains('script of its own')) 'R7/R6: LGPL-NOTICE.txt names both programs and the recipes'' scripted edits'

    # 12h. R6: a recipe's script that changes the library's files before the build is named and listed.
    $scriptRecipe = Join-Path $work 'script-recipe'
    New-Item -ItemType Directory -Path $scriptRecipe -Force | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $scriptRecipe 'portfile.cmake'), "vcpkg_extract_source_archive(SOURCE_PATH ARCHIVE x PATCHES uwp.patch)`nvcpkg_execute_required_process(COMMAND `"`${CMAKE_COMMAND}`" -DSOURCE_DIRS=gettext-runtime -P `"`${CMAKE_CURRENT_LIST_DIR}/bashify.cmake`" WORKING_DIRECTORY `"`${SOURCE_PATH}`")`n")
    [System.IO.File]::WriteAllText((Join-Path $scriptRecipe 'bashify.cmake'), "file(WRITE `"`${dir}/configure`" `"`${script}`")`n")
    [System.IO.File]::WriteAllText((Join-Path $scriptRecipe 'uwp.patch'), "--- a/gettext-runtime/intl/langprefs.c`n+++ b/gettext-runtime/intl/langprefs.c`n@@ -1 +1 @@`n-a`n+b`n")
    $scriptFiles = @(Get-ChildItem -LiteralPath $scriptRecipe -File | ForEach-Object { [pscustomobject]@{ Name = "ports/gettext-libintl/$($_.Name)"; SourcePath = $_.FullName } })
    $portfileText = [System.IO.File]::ReadAllText((Join-Path $scriptRecipe 'portfile.cmake'))
    Assert-True ((@(Get-RecipeScripts -PortfileText $portfileText) -join '|') -ceq 'bashify.cmake') 'R6: the scripts a portfile runs with cmake -P are found'
    Assert-Problem (Test-ScriptedEdits -Port 'gettext-libintl' -PortfileText $portfileText -ScriptedEdits @()) 'runs its script bashify\.cmake before the build' 'R6: a script the policy does not name is refused (the package of the gate-8 reading listed only the patches)'
    $edits = @([pscustomobject]@{ script = 'bashify.cmake'; changes = @('gettext-runtime/configure') })
    Assert-True (@(Test-ScriptedEdits -Port 'gettext-libintl' -PortfileText $portfileText -ScriptedEdits $edits).Count -eq 0) 'R6: a named script passes'
    $scripted = @(Get-RecipeModifications -Port 'gettext-libintl' -RecipeFiles $scriptFiles -Baseline ('e143402b' + '0' * 32) -BaselineDate '2026-09-24' -ScriptedEdits $edits)
    $scriptLines = (@(Format-RecipeModifications -Modifications $scripted -Prefix '  Modified: ' -Baseline ('e143402b' + '0' * 32)) -join ' ') -replace '\s+', ' '
    Assert-True ($scripted.Count -eq 2 -and $scripted[1].kind -ceq 'script' -and $scriptLines.Contains("bashify.cmake (vcpkg's script, as in its registry at commit e143402b of 2026-09-24; portfile.cmake runs it before the build) changes gettext-runtime/configure.") -and
                 $scriptLines.Contains("uwp.patch (vcpkg's, as in its registry")) "R6: the script is listed after the patches with the file it changes ($scriptLines)"
    Assert-Throws { Get-RecipeModifications -Port 'gettext-libintl' -RecipeFiles $scriptFiles -Baseline ('e143402b' + '0' * 32) -ScriptedEdits @([pscustomobject]@{ script = 'missing.cmake'; changes = @('x') }) } 'R6: a scripted edit the recipe does not hold is refused'

    # 12i. R9: a registry port's recipe file is vcpkg's (MIT), a patch also under the library's own licence.
    $registryPatch = Get-RecipeFileLicence -File 'fix_build.patch' -Status 'registry' -Port 'lensfun' -LibraryLicence 'LGPL-3.0-only' -VcpkgCopyright 'Copyright (c) Microsoft Corporation' -LucentCopyright 'n/a'
    Assert-True ($registryPatch.Licence -ceq 'MIT AND LGPL-3.0-only' -and $registryPatch.Copyright -ceq 'Copyright (c) Microsoft Corporation') "R9: a registry port's patch is MIT AND its library's licence ($($registryPatch.Licence))"

    # 12j. Qt's third-party code against a Qt component's licence.
    $attributions = @(
        [pscustomobject]@{ Id = 'Gui_Attribution_grayraster'; Name = 'Rasterizer'; License = 'FTL OR GPL-2.0-only'; Files = @('Qt6Gui.dll') },
        [pscustomobject]@{ Id = 'Gui_Attribution_vulkanmemoryallocator'; Name = 'Vulkan Memory Allocator'; License = 'MIT'; Files = @('Qt6Gui.dll') },
        [pscustomobject]@{ Id = 'Network_Attribution_psl-data'; Name = 'Public Suffix List'; License = 'MPL-2.0'; Files = @('Qt6Network.dll') })
    Assert-Problem (Test-AttributionLicences -Port 'qtlike' -Concluded 'LGPL-3.0-only AND FTL' -Attributions $attributions) 'records Public Suffix List \(MPL-2\.0\) inside Qt6Network\.dll of qtlike' 'Sweep: Qt''s third-party code the licence does not cover is refused'
    Assert-True (@(Test-AttributionLicences -Port 'qtlike' -Concluded 'LGPL-3.0-only AND FTL AND MPL-2.0' -Attributions $attributions -NotCompiled @{ vulkanmemoryallocator = 'Vulkan is off.' }).Count -eq 0) `
        'Sweep: the licence covers the compiled code (FTL elected), and code the policy names as not compiled is left out'
    Assert-Problem (Test-AttributionLicences -Port 'qtlike' -Concluded 'LGPL-3.0-only AND FTL AND MIT AND MPL-2.0' -Attributions $attributions -NotCompiled @{ yoga = 'not here' }) 'attributionsNotCompiled names yoga' 'Sweep: a stale "not compiled" name is refused'

    # 12k. R14: Inno Setup's licence is the compiler's license.txt, as reviewed for its version.
    $innoDir = Join-Path $work 'inno'
    New-Item -ItemType Directory -Path $innoDir -Force | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $innoDir 'license.txt'), "Inno Setup License`r`n`r`nCopyright (C) 1997-2026 Jordan Russell. All rights reserved.`r`nPortions Copyright (C) 2000-2026 Martijn Laan. All rights reserved.`r`n`r`nPermission is granted, for the test.`r`n", $utf8)
    $innoPolicy = [pscustomobject]@{ distribution = [pscustomobject]@{ installerTool = [pscustomobject]@{ reviewedVersion = '6.7.3'; licenseFile = 'license.txt'; licenseSha256 = (Get-Sha256 (Join-Path $innoDir 'license.txt')) } } }
    $iscc = [pscustomobject]@{ Path = (Join-Path $innoDir 'ISCC.exe'); Version = 'Inno Setup 6.7.3' }
    $innoRecord = Get-InnoSetupLicenceFile -Iscc $iscc -Policy $innoPolicy
    Assert-True ($innoRecord.Version -ceq '6.7.3' -and $innoRecord.Copyright -ceq "Copyright (C) 1997-2026 Jordan Russell. All rights reserved.`nPortions Copyright (C) 2000-2026 Martijn Laan. All rights reserved." -and $innoRecord.Text.Contains('Permission is granted')) `
        'R14: the compiler''s license.txt, its copyright lines and version'
    Assert-Throws { Get-InnoSetupLicenceFile -Iscc ([pscustomobject]@{ Path = $iscc.Path; Version = 'Inno Setup 6.8.0' }) -Policy $innoPolicy } 'R14: another compiler version is refused until its licence is reviewed'
    $innoPolicy.distribution.installerTool.licenseSha256 = '0' * 64
    Assert-Throws { Get-InnoSetupLicenceFile -Iscc $iscc -Policy $innoPolicy } 'R14: another licence text is refused'
    Assert-True ($null -eq (Get-InnoSetupLicenceFile -Iscc $null -Policy $innoPolicy)) 'R14: without a compiler there is no licence file'

    # 12l. The compiler's library code in the programs: its notices from the reviewed toolset's headers, a part
    # compiled in when its probed table is in a program.
    $toolset = Join-Path $work 'MSVC\14.44.35207'
    New-Item -ItemType Directory -Path (Join-Path $toolset 'bin\Hostx64\x64'), (Join-Path $toolset 'include') -Force | Out-Null
    $tables = "// generated`n// UNICODE, INC. LICENSE AGREEMENT - DATA FILES AND SOFTWARE`n// Permission is granted, for the test.`n// written authorization of the copyright holder.`n" +
              "inline constexpr char32_t _Width_estimate_intervals_v2[] = {0x1100, 0x1160, 0x231a, 0x231c, 0x2329, 0x232b, 0x23e9,`n    0x23ed, 0x23f0};`n"
    [System.IO.File]::WriteAllText((Join-Path $toolset 'include\__tables.hpp'), $tables, $utf8)
    $tcStage = Join-Path $work 'tc-stage'
    New-Item -ItemType Directory -Path $tcStage -Force | Out-Null
    $table = New-Object byte[] 32
    $probeValues = @(0x1100, 0x1160, 0x231a, 0x231c, 0x2329, 0x232b, 0x23e9, 0x23ed)
    for ($i = 0; $i -lt 8; $i++) { [System.BitConverter]::GetBytes([uint32]$probeValues[$i]).CopyTo($table, 4 * $i) }
    [System.IO.File]::WriteAllBytes((Join-Path $tcStage 'Lucent.exe'), [byte[]](@(0x4D, 0x5A) + @($table) + @(0, 0)))
    [System.IO.File]::WriteAllBytes((Join-Path $tcStage 'lucent-worker.exe'), [byte[]]@(0x4D, 0x5A, 0, 0))
    $tcPart = [pscustomobject]@{ name = 'Unicode tables'; copyright = 'Copyright (c) Unicode, Inc.'; licence = 'Unicode-DFS-2016'; member = 'include/__tables.hpp'
        sha256 = (Get-Sha256 (Join-Path $toolset 'include\__tables.hpp')); first = 'UNICODE, INC. LICENSE AGREEMENT'; last = 'written authorization of the copyright holder.'; probe = '_Width_estimate_intervals_v2'; as = 'Unicode.txt' }
    $tcPolicy = [pscustomobject]@{ toolchainLibraries = [pscustomobject]@{ 'msvc-stl' = [pscustomobject]@{ name = 'C++ library'; reviewedToolset = '14.44.35207'
        declared = 'Apache-2.0 WITH LLVM-exception'; concluded = 'Apache-2.0 WITH LLVM-exception AND Unicode-DFS-2016'; copyright = 'Copyright (c) Microsoft Corporation'; parts = @($tcPart) } } }
    $tcInfo = [pscustomobject]@{ dumpbin = (Join-Path $toolset 'bin\Hostx64\x64\dumpbin.exe'); executable = 'Lucent.exe'; helperExecutables = @('lucent-worker.exe') }
    $tcLibraries = @(Get-ToolchainLibraries -Info $tcInfo -Policy $tcPolicy -StageDir $tcStage)
    Assert-True ($tcLibraries.Count -eq 1 -and $tcLibraries[0].Version -ceq '14.44.35207' -and (@($tcLibraries[0].Programs) -join '|') -ceq 'Lucent.exe' -and $tcLibraries[0].Parts[0].Compiled -and
                 $tcLibraries[0].Parts[0].File -ceq 'msvc-stl\Unicode.txt' -and $tcLibraries[0].Parts[0].Text.Contains("// UNICODE, INC. LICENSE AGREEMENT - DATA FILES AND SOFTWARE`n") -and $tcLibraries[0].Parts[0].Text.Contains($tcPart.sha256)) `
        'Sweep: the Unicode tables'' notice comes from the reviewed header, and the part is compiled into the program that holds the table'
    [System.IO.File]::WriteAllBytes((Join-Path $tcStage 'Lucent.exe'), [byte[]]@(0x4D, 0x5A, 0, 0))
    $tcNone = @(Get-ToolchainLibraries -Info $tcInfo -Policy $tcPolicy -StageDir $tcStage)
    Assert-True (@($tcNone[0].Programs).Count -eq 0 -and -not $tcNone[0].Parts[0].Compiled) 'Sweep: without the table in a program, the part is not compiled in'
    $tcPolicy.toolchainLibraries.'msvc-stl'.reviewedToolset = '14.45.0'
    Assert-Throws { Get-ToolchainLibraries -Info $tcInfo -Policy $tcPolicy -StageDir $tcStage } 'Sweep: another toolset is refused until its headers are reviewed'
    $tcPolicy.toolchainLibraries.'msvc-stl'.reviewedToolset = '14.44.35207'
    $tcPart.sha256 = '0' * 64
    Assert-Throws { Get-ToolchainLibraries -Info $tcInfo -Policy $tcPolicy -StageDir $tcStage } 'Sweep: a header other than the reviewed one is refused'

    # 12m. R12: the notes gate 7 checks are the package's own (its manifest's or its notices').
    $shippedNoticesText = "Components`n----------`n`nqtlike 1.0`n  Licence: LGPL-3.0-only`n  Note: Mostly CMake build files; named.diff changes src/a.cpp; another patch changes src/b.c.`n  DLLs: x.dll`n`nzlib 1.3`n  Licence: Zlib`n"
    $parsed = Get-NoticesNotes -Text $shippedNoticesText
    Assert-True ($parsed.Count -eq 1 -and $parsed['qtlike'].StartsWith('Mostly CMake build files')) 'R12: the notes are read from the package''s THIRD-PARTY-NOTICES.txt'
    $notesPolicy = [pscustomobject]@{ ports = [pscustomobject]@{ qtlike = [pscustomobject]@{ comment = $parsed['qtlike'] } } }
    Assert-Problem (Test-ModificationNotes -Components $noteComponents -Policy $notesPolicy) 'does not name silence-warnings\.diff' 'R12: a note the package ships that leaves out a code patch is found, whatever the working tree''s policy says'

    # 13. The gate-8 review, round 2: each patch's licence from the files it changes, Qt's records reviewed (a
    # licence corrected, NOTICE lines, the Source Code Form of copyleft code), the Windows SDK's code in Qt's files,
    # the code of others inside Inno Setup, the programs' whole licences, and the notices' wording.

    # 13a. Expressions split and joined, elections, path patterns, notices read across line breaks.
    Assert-True ((@(Split-LicenceConjunction 'A AND (B OR C) AND D WITH E') -join '|') -ceq 'A|(B OR C)|D WITH E' -and (@(Split-LicenceConjunction 'B OR C') -join '|') -ceq '(B OR C)') `
        'Round 2: an expression''s AND terms, a choice kept whole in parentheses'
    $joined = Join-LicenceTerms @('MIT AND LicenseRef-Logo', 'MIT AND CC0-1.0', 'CC0-1.0', 'Apache-2.0 WITH LLVM-exception AND BSL-1.0', '')
    Assert-True ($joined -ceq 'MIT AND LicenseRef-Logo AND CC0-1.0 AND Apache-2.0 WITH LLVM-exception AND BSL-1.0') "Round 2: expressions joined with AND, each term once ($joined)"
    $elections = [ordered]@{
        'LicenseRef-Qt-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only' = 'LGPL-3.0-only'
        'LicenseRef-Qt-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0' = 'GPL-3.0-only WITH Qt-GPL-exception-1.0'
        'BSD-3-Clause' = 'BSD-3-Clause'; 'GPL-2.0-only OR GPL-3.0-only' = ''; '(MIT OR Apache-2.0) AND BSD-3-Clause' = '' }
    foreach ($case in $elections.GetEnumerator()) {
        $elected = Select-ElectedLicence -Expression $case.Key -LibraryLicence 'LGPL-3.0-only AND BSD-3-Clause'
        Assert-True ([string]$elected -ceq $case.Value) "Round 2: Lucent's election of '$($case.Key)' is '$($case.Value)' ('$elected')"
    }
    $patterns = [ordered]@{ 'cmake/a/b.cmake|cmake/**' = $true; 'cmake/a/b|cmake/*' = $false; 'cmake/qt.toolchain.cmake.in|cmake/**.in' = $true; 'libtiff/CMakeLists.txt|CMakeLists.txt' = $false; 'CMakeLists.txt|CMakeLists.txt' = $true }
    foreach ($case in $patterns.GetEnumerator()) {
        $path, $pattern = $case.Key -split '\|'
        Assert-True ((Test-PathPattern -Path $path -Pattern $pattern) -eq $case.Value) "Round 2: '$path' matches '$pattern': $($case.Value)"
    }
    $gplNotice = "# =====`n# @author Copyright (C) 2013 by Someone`n#`n# This program is free software; you can redistribute it`n# and/or modify it under the terms of the GNU General`n# Public License as published by the Free Software Foundation;`n# either version 2, or (at your option)`n# any later version.`n"
    Assert-True ((ConvertTo-NoticeText $gplNotice).Contains('under the terms of the GNU General Public License as published by the Free Software Foundation; either version 2, or (at your option) any later version.')) `
        'Round 2: a licence statement is found across the lines and comment marks of a notice'
    Assert-True ((ConvertTo-NoticeText "/*`n * one`n * two */`n// three`n; four`n").Contains('one two three four')) 'Round 2: C, C++, assembler and CMake comment marks are removed'

    # 13b. Each patch's licence comes from the files it changes, read from the verified archive (the macdeployqt
    # and windeployqt patches were given qtbase's library licence; LibRaw's patches of LibRaw-cmake, LibRaw's).
    $plTree = Join-Path $work 'pl-archive'
    New-Item -ItemType Directory -Path (Join-Path $plTree 'lib-2.0\src\tools'), (Join-Path $plTree 'lib-2.0\cmake') -Force | Out-Null
    $plFiles = [ordered]@{
        'src\core.cpp' = "// Copyright (C) The Lib Company`n// SPDX-License-Identifier: LicenseRef-Lib-Commercial OR LGPL-3.0-only OR GPL-2.0-only OR GPL-3.0-only`nint a;`n"
        'src\tools\tool.cpp' = "// SPDX-License-Identifier: LicenseRef-Lib-Commercial OR GPL-3.0-only WITH Qt-GPL-exception-1.0`nint b;`n"
        'CMakeLists.txt' = $gplNotice + "project(lib)`n"
        'cmake\config.cmake.in' = "set(X 1)`n"
        'LICENSE' = "Redistribution and use in source and binary forms, with or without`nmodification, are permitted.`n"
        'README' = "Also the build system (the build/ folder) is under the GPL.`n" }
    foreach ($name in $plFiles.Keys) { [System.IO.File]::WriteAllText((Join-Path $plTree "lib-2.0\$name"), $plFiles[$name], $utf8) }
    $plTarball = Join-Path $downloadsDir 'example-lib-v2.0.tar.gz'
    & (Join-Path $env:SystemRoot 'System32\tar.exe') -czf $plTarball -C $plTree 'lib-2.0'
    $plSha = (Get-FileHash -LiteralPath $plTarball -Algorithm SHA512).Hash.ToLowerInvariant()
    $plSources = @([pscustomobject]@{ Name = 'example/lib'; Location = 'git+https://github.com/example/lib@v2.0'; Sha512 = $plSha
        Resource = [pscustomobject]@{ name = 'example/lib'; downloadLocation = 'git+https://github.com/example/lib@v2.0'; packageFileName = 'example-lib-v2.0.tar.gz'
            checksums = @([pscustomobject]@{ algorithm = 'SHA512'; checksumValue = $plSha }) } })
    $plRecipe = Join-Path $work 'pl-recipe'
    New-Item -ItemType Directory -Path $plRecipe -Force | Out-Null
    $diff = { param([string]$File, [string]$Old, [string]$New) "--- a/$File`n+++ b/$File`n@@ -1 +1 @@`n-$Old`n+$New`n" }
    [System.IO.File]::WriteAllText((Join-Path $plRecipe 'portfile.cmake'), "vcpkg_from_github(OUT_SOURCE_PATH SOURCE_PATH REPO example/lib PATCHES core.patch tool.patch build.patch new.patch)`n")
    [System.IO.File]::WriteAllText((Join-Path $plRecipe 'core.patch'), (& $diff 'src/core.cpp' 'int a;' 'int a = 1;'))
    [System.IO.File]::WriteAllText((Join-Path $plRecipe 'tool.patch'), (& $diff 'src/tools/tool.cpp' 'int b;' 'int b = 1;'))
    [System.IO.File]::WriteAllText((Join-Path $plRecipe 'build.patch'), (& $diff 'CMakeLists.txt' 'project(lib)' 'project(lib C)') + (& $diff 'cmake/config.cmake.in' 'set(X 1)' 'set(X 2)'))
    [System.IO.File]::WriteAllText((Join-Path $plRecipe 'new.patch'), "--- /dev/null`n+++ b/src/new.cpp`n@@ -0,0 +1 @@`n+int c;`n")
    [System.IO.File]::WriteAllText((Join-Path $plRecipe 'unused.patch'), (& $diff 'src/core.cpp' 'int a;' 'int a = 2;'))
    $gplRule = [pscustomobject]@{ files = @('CMakeLists.txt'); licence = 'GPL-2.0-or-later'; comment = 'its notice'
        statement = @('under the terms of the GNU General Public License as published by the Free Software Foundation; either version 2, or (at your option) any later version.') }
    $bsdRule = [pscustomobject]@{ files = @('cmake/**'); licence = 'BSD-3-Clause'; comment = 'the LICENSE'
        evidence = @([pscustomobject]@{ member = 'LICENSE'; line = 'Redistribution and use in source and binary forms, with or without' }) }
    $pl = Get-PatchLicences -Port 'lib' -RecipeFolder $plRecipe -Sources $plSources -DownloadsDir $downloadsDir -Rules @($gplRule, $bsdRule) -LibraryLicence 'LGPL-3.0-only AND BSD-3-Clause'
    $plSummary = @($pl.Keys | ForEach-Object { "$_=$($pl[$_].Licence)" }) -join '; '
    Assert-True ((@($pl.Keys) -join '|') -ceq 'build.patch|core.patch|new.patch|tool.patch') "Round 2: every patch the portfile names, not the unused one ($plSummary)"
    Assert-True ($pl['core.patch'].Licence -ceq 'LGPL-3.0-only' -and $pl['tool.patch'].Licence -ceq 'GPL-3.0-only WITH Qt-GPL-exception-1.0' -and
                 $pl['build.patch'].Licence -ceq 'BSD-3-Clause AND GPL-2.0-or-later' -and $null -eq $pl['new.patch'].Licence) `
        "Round 2: each patch quotes lines under its files' licences: the SPDX line with Lucent's election, a rule's statement or evidence, nothing for a created file ($plSummary)"
    $toolFile = Get-RecipeFileLicence -File 'tool.patch' -Status 'unchanged' -Port 'lib' -Derived -QuotedLicence $pl['tool.patch'].Licence -QuotedText $pl['tool.patch'].Text -VcpkgCopyright 'Copyright (c) Microsoft Corporation' -LucentCopyright 'l'
    $oldFile = Get-RecipeFileLicence -File 'tool.patch' -Status 'unchanged' -Port 'lib' -LibraryLicence 'LGPL-3.0-only AND BSD-3-Clause' -VcpkgCopyright 'Copyright (c) Microsoft Corporation' -LucentCopyright 'l'
    Assert-True ($toolFile.Licence -ceq 'MIT AND GPL-3.0-only WITH Qt-GPL-exception-1.0' -and $oldFile.Licence -ceq 'MIT AND LGPL-3.0-only AND BSD-3-Clause' -and $toolFile.Comment.Contains('src/tools/tool.cpp')) `
        "Round 2: a patch of a GPL tool is 'MIT AND GPL-3.0-only WITH Qt-GPL-exception-1.0' ($($toolFile.Licence)), not the library's licence it had ($($oldFile.Licence))"
    Assert-True ((Get-RecipeFileLicence -File 'new.patch' -Status 'added' -Port 'lib' -Derived -QuotedLicence $null -VcpkgCopyright 'v' -LucentCopyright 'l').Licence -ceq 'MIT') 'Round 2: a patch that only creates files is its author''s (MIT)'
    $unstated = [pscustomobject]@{ files = @('CMakeLists.txt'); licence = 'NOASSERTION'; comment = 'the archive states none'
        evidence = @([pscustomobject]@{ member = 'README'; line = 'Also the build system (the build/ folder)' }) }
    $plNone = Get-PatchLicences -Port 'lib' -RecipeFolder $plRecipe -Sources $plSources -DownloadsDir $downloadsDir -Rules @($unstated, $bsdRule) -LibraryLicence 'LGPL-3.0-only AND BSD-3-Clause'
    Assert-True ($plNone['build.patch'].Licence -ceq 'NOASSERTION' -and (Get-RecipeFileLicence -File 'build.patch' -Status 'unchanged' -Port 'lib' -Derived -QuotedLicence 'NOASSERTION' -QuotedText 'x' -VcpkgCopyright 'v' -LucentCopyright 'l').Licence -ceq 'NOASSERTION') `
        'Round 2: a patch quoting a file whose licence its archive states nowhere makes no assertion (Lensfun''s CMakeLists.txt)'
    Assert-Throws { Get-PatchLicences -Port 'lib' -RecipeFolder $plRecipe -Sources $plSources -DownloadsDir $downloadsDir -Rules @($bsdRule) -LibraryLicence 'LGPL-3.0-only AND BSD-3-Clause' } 'Round 2: a file whose licence neither an SPDX line nor a rule gives stops packaging'
    $wrongStatement = [pscustomobject]@{ files = @('CMakeLists.txt'); licence = 'MIT'; comment = 'c'; statement = @('Permission is hereby granted') }
    Assert-Throws { Get-PatchLicences -Port 'lib' -RecipeFolder $plRecipe -Sources $plSources -DownloadsDir $downloadsDir -Rules @($wrongStatement, $bsdRule) -LibraryLicence 'LGPL-3.0-only' } 'Round 2: a rule whose statement the file does not hold stops packaging'
    $wrongEvidence = [pscustomobject]@{ files = @('cmake/**'); licence = 'BSD-3-Clause'; comment = 'c'; evidence = @([pscustomobject]@{ member = 'COPYING'; line = 'x' }) }
    Assert-Throws { Get-PatchLicences -Port 'lib' -RecipeFolder $plRecipe -Sources $plSources -DownloadsDir $downloadsDir -Rules @($gplRule, $wrongEvidence) -LibraryLicence 'LGPL-3.0-only' } 'Round 2: a rule whose evidence the archive does not hold stops packaging'
    $contradiction = [pscustomobject]@{ files = @('src/core.cpp'); licence = 'MIT'; comment = 'c'; statement = @('int a;') }
    Assert-Throws { Get-PatchLicences -Port 'lib' -RecipeFolder $plRecipe -Sources $plSources -DownloadsDir $downloadsDir -Rules @($contradiction, $gplRule, $bsdRule) -LibraryLicence 'LGPL-3.0-only AND BSD-3-Clause' } 'Round 2: a rule that contradicts a file''s SPDX line stops packaging'
    Assert-Throws { Get-PatchLicences -Port 'lib' -RecipeFolder $plRecipe -Sources $plSources -DownloadsDir $downloadsDir -Rules @($gplRule, $bsdRule) -LibraryLicence 'BSD-3-Clause' } 'Round 2: an SPDX choice the library''s licence does not settle stops packaging'
    Assert-Problem (Test-FileLicenceRules -Port 'lib' -Rules @([pscustomobject]@{ files = @('x'); licence = 'MIT'; comment = 'c' })) 'shows its licence by no statement' 'Round 2: a rule that shows its licence by nothing is refused'
    Assert-Problem (Test-FileLicenceRules -Port 'lib' -Rules @([pscustomobject]@{ licence = 'MIT'; comment = 'c'; statement = @('x') })) 'names no files' 'Round 2: a rule without files is refused'
    $realRules = @(foreach ($property in @($realPolicy.ports.PSObject.Properties)) { if ($property.Value.PSObject.Properties.Name -contains 'fileLicences') { Test-FileLicenceRules -Port $property.Name -Rules @($property.Value.fileLicences) } })
    Assert-True ($realRules.Count -eq 0) "Round 2: the policy's rules for the files patches change are complete ($($realRules -join ' | '))"
    Assert-True ((@($realPolicy.ports.libraw.fileLicences | ForEach-Object { "$($_.source):$($_.licence)" }) -join '|') -ceq 'LibRaw/LibRaw-cmake:GPL-2.0-or-later|LibRaw/LibRaw-cmake:BSD-3-Clause' -and
                 @($realPolicy.ports.lensfun.fileLicences)[0].licence -ceq 'NOASSERTION' -and @(@($realPolicy.ports.lensfun.fileLicences)[0].evidence).Count -eq 2) `
        'Round 2: LibRaw''s patches take LibRaw-cmake''s licences, and Lensfun''s CMakeLists.txt states none'

    # 13c. The corresponding source carries a licence text the conveyed files need and their archives lack
    # (LibRaw-cmake's GPL-2.0-or-later CMakeLists.txt, which LibRaw's patches quote).
    $gplManifest = { param([bool]$WithText) ConvertTo-Json -Depth 6 -InputObject ([ordered]@{ schema = 'lucent.corresponding-source'; schemaVersion = 1; productVersion = $version
        components = @([ordered]@{ port = 'liba'; modifications = @([ordered]@{ patch = 'ports/liba/fix.patch'; kind = 'patch'; licence = 'MIT AND GPL-2.0-or-later' }) })
        licenceTexts = @(if ($WithText) { [ordered]@{ file = 'licenses/GPL-2.0-only.txt'; covers = @('GPL-2.0-only', 'GPL-2.0-or-later') } }) }) }
    Assert-Problem (Test-CorrespondingSourcePackage -Path (New-TestPackage 'gpl-no-text' -ManifestText (& $gplManifest $false)) -Version $version -Expected $expected).Errors `
        'fix\.patch of liba quotes lines under GPL-2\.0-or-later, but the archive holds no text of it' 'Round 2: a patch quoting GPL-2.0 lines without the GPL''s text in the archive fails'
    Assert-Problem (Test-CorrespondingSourcePackage -Path (New-TestPackage 'gpl-text-missing' -ManifestText (& $gplManifest $true)) -Version $version -Expected $expected).Errors `
        'lists the licence text licenses/GPL-2\.0-only\.txt, which the archive does not hold' 'Round 2: a licence text MANIFEST.json lists but the archive lacks fails'
    $gplOk = Test-CorrespondingSourcePackage -Path (New-TestPackage 'gpl-text' -ManifestText (& $gplManifest $true) -Replace @{ 'licenses/GPL-2.0-only.txt' = $utf8.GetBytes("GNU GENERAL PUBLIC LICENSE`nVersion 2, June 1991`n") }) -Version $version -Expected $expected
    Assert-True (@($gplOk.Errors).Count -eq 0) "Round 2: with the GPL's text in licenses/ the archive passes ($(@($gplOk.Errors) -join ' | '))"
    $gplText = @($realPolicy.distribution.correspondingSource.licenceTexts)[0]
    Assert-True ($gplText.as -ceq 'GPL-2.0-only.txt' -and $gplText.port -ceq 'qtbase' -and $gplText.member -ceq 'LICENSES/GPL-2.0-only.txt' -and @($gplText.covers) -contains 'GPL-2.0-or-later') `
        'Round 2: the policy takes the GPL-2.0 text for the archive from qtbase''s verified archive'

    # 13d. Files kept from published sources are pinned by their git blob ids (packaging never downloads).
    $tpRoot = Join-Path $work 'tp-root'
    $tpFolder = Join-Path $tpRoot 'installer\third-party\upstream-abc'
    New-Item -ItemType Directory -Path (Join-Path $tpFolder 'src') -Force | Out-Null
    $noticeBody = "Upstream`r`nCopyright 2021 The Foundation`r`n`r`nThis product includes software developed at`r`nThe Foundation (https://example.org/).`r`n`r`nOther lines.`r`n`r`nData.xml includes definitions adapted from the Registry`r`nunder the Open Licence 3.0 (https://example.org/ol3)`r`n"
    $tpFiles = [ordered]@{ 'NOTICE.txt' = $noticeBody; 'LICENSE.md' = "Script engine licence`r`n3. You must have a visible line in your documentation.`r`n"
        'src/dec.c' = "/* dec.c */`r`n#include `"dec.h`"`r`n"; 'src/dec.h' = "/* Decoder terms`r`n   LGPL or CPL, with an exception.`r`n   end of terms */`r`n" }
    $tpBlobs = [ordered]@{}
    foreach ($name in $tpFiles.Keys) {
        $bytes = $utf8.GetBytes($tpFiles[$name])
        [System.IO.File]::WriteAllBytes((Join-Path $tpFolder $name.Replace('/', '\')), $bytes)
        $sha1 = [System.Security.Cryptography.SHA1]::Create()
        try { $header = [System.Text.Encoding]::ASCII.GetBytes("blob $($bytes.Length)`0"); [void]$sha1.TransformBlock($header, 0, $header.Length, $null, 0); [void]$sha1.TransformFinalBlock($bytes, 0, $bytes.Length)
              $tpBlobs[$name] = [System.BitConverter]::ToString($sha1.Hash).Replace('-', '').ToLowerInvariant() } finally { $sha1.Dispose() }
    }
    $tpCommit = 'ab' * 20
    $tpPolicy = [pscustomobject]@{ thirdPartySources = [pscustomobject]@{ 'upstream-abc' = [pscustomobject]@{ repository = 'https://github.com/example/upstream'; commit = $tpCommit; tag = 'v1'
        folder = 'installer/third-party/upstream-abc'; files = [pscustomobject]$tpBlobs } } }
    $kept = Get-ThirdPartySourceFile -Policy $tpPolicy -RepoRoot $tpRoot -Source 'upstream-abc' -File 'NOTICE.txt'
    Assert-True ($kept.Text -ceq $noticeBody -and $kept.Provenance -ceq "NOTICE.txt of https://github.com/example/upstream at commit $tpCommit, tag v1 (git blob $($tpBlobs['NOTICE.txt']))") "Round 2: a kept file is read as its blob, with its provenance ($($kept.Provenance))"
    [System.IO.File]::WriteAllText((Join-Path $tpFolder 'NOTICE.txt'), $noticeBody.Replace("`r`n", "`n"), $utf8)
    Assert-True ((Get-ThirdPartySourceFile -Policy $tpPolicy -RepoRoot $tpRoot -Source 'upstream-abc' -File 'NOTICE.txt').Text -ceq $noticeBody) 'Round 2: a checkout with other line endings gives the blob''s bytes'
    [System.IO.File]::WriteAllText((Join-Path $tpFolder 'NOTICE.txt'), $noticeBody.Replace('Other lines.', 'Changed lines.'), $utf8)
    Assert-Throws { Get-ThirdPartySourceFile -Policy $tpPolicy -RepoRoot $tpRoot -Source 'upstream-abc' -File 'NOTICE.txt' } 'Round 2: a kept file that is not the pinned blob is refused'
    [System.IO.File]::WriteAllBytes((Join-Path $tpFolder 'NOTICE.txt'), $utf8.GetBytes($noticeBody))
    Assert-Throws { Get-ThirdPartySourceFile -Policy $tpPolicy -RepoRoot $tpRoot -Source 'upstream-abc' -File 'missing.txt' } 'Round 2: a file the policy does not pin is refused'
    $realKept = @(foreach ($source in @($realPolicy.thirdPartySources.PSObject.Properties)) { foreach ($file in @($source.Value.files.PSObject.Properties)) {
        try { $null = Get-ThirdPartySourceFile -Policy $realPolicy -RepoRoot $repoRoot -Source $source.Name -File $file.Name } catch { "$($source.Name)/$($file.Name): $($_.Exception.Message)" } } })
    Assert-True ($realKept.Count -eq 0) "Round 2: every file kept in installer\third-party has the git blob id the policy pins ($($realKept -join ' | '))"

    # 13e. Qt's records reviewed: a licence the code carries instead (Khronos), its NOTICE lines (Tika), the Source
    # Code Form of copyleft code at the commit of its package URL (the Public Suffix List).
    $qtTree = Join-Path $work 'qt-archive'
    New-Item -ItemType Directory -Path (Join-Path $qtTree 'qtlike-1.0\src\gui\opengl') -Force | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $qtTree 'qtlike-1.0\src\gui\opengl\REUSE.toml'), "version = 1`n[[annotations]]`nSPDX-License-Identifier = `"MIT-Khronos-old`"`n", $utf8)
    $qtTarball = Join-Path $downloadsDir 'qtlike-1.0.tar.gz'
    & (Join-Path $env:SystemRoot 'System32\tar.exe') -czf $qtTarball -C $qtTree 'qtlike-1.0'
    $qtSha = (Get-FileHash -LiteralPath $qtTarball -Algorithm SHA512).Hash.ToLowerInvariant()
    $qtSources = @([pscustomobject]@{ Name = 'qtlike-1.0.tar.gz'; Location = 'https://example.org/qtlike-1.0.tar.gz'; Sha512 = $qtSha
        Resource = [pscustomobject]@{ name = 'qtlike-1.0.tar.gz'; downloadLocation = 'https://example.org/qtlike-1.0.tar.gz'; packageFileName = 'qtlike-1.0.tar.gz'
            checksums = @([pscustomobject]@{ algorithm = 'SHA512'; checksumValue = $qtSha }) } })
    $pslCommit = 'cd' * 20
    $newAttributions = {
        @([pscustomobject]@{ Id = 'Gui_Attribution_opengl-headers'; Name = 'OpenGL Headers'; Version = $null; License = 'MIT'; Files = @('Qt6Gui.dll'); Compiled = $true; Purl = $null; DownloadLocation = 'https://www.khronos.org/' },
          [pscustomobject]@{ Id = 'Core_Attribution_mimetypes'; Name = 'MIME types'; Version = '1'; License = 'Apache-2.0'; Files = @('Qt6Core.dll'); Compiled = $true; Purl = $null; DownloadLocation = 'NOASSERTION' },
          [pscustomobject]@{ Id = 'Network_Attribution_psl-data'; Name = 'The Public Suffix List'; Version = '2026'; License = 'MPL-2.0'; Files = @('Qt6Network.dll'); Compiled = $true
              Purl = "pkg:generic/psl-data@$($pslCommit)?download_url=https://example.org/list.dat"; DownloadLocation = 'https://example.org/list.dat' }) }
    $qtEntry = [pscustomobject]@{
        attributionLicences = [pscustomobject]@{
            'opengl-headers' = [pscustomobject]@{ qtSbom = 'MIT'; licence = 'MIT-Khronos-old'; comment = 'as REUSE.toml says'; evidence = @([pscustomobject]@{ member = 'src/gui/opengl/REUSE.toml'; line = 'SPDX-License-Identifier = "MIT-Khronos-old"' }) }
            'mimetypes' = [pscustomobject]@{ qtSbom = 'Apache-2.0'; licence = 'Apache-2.0 AND OGL-UK-3.0'; comment = 'the NOTICE names it'; texts = [pscustomobject]@{ 'OGL-UK-3.0' = 'a link in the NOTICE' }
                evidence = @([pscustomobject]@{ source = 'upstream-abc'; file = 'NOTICE.txt'; line = 'under the Open Licence 3.0' }) } }
        attributionNotices = [pscustomobject]@{ 'mimetypes' = [pscustomobject]@{ source = 'upstream-abc'; file = 'NOTICE.txt'; as = 'Upstream-NOTICE.txt'; comment = 'its NOTICE lines'
            excerpts = @([pscustomobject]@{ first = 'Upstream'; last = 'The Foundation (https://example.org/).' }, [pscustomobject]@{ first = 'Data.xml includes definitions'; last = 'under the Open Licence 3.0' }) } }
        attributionSources = [pscustomobject]@{ 'psl-data' = [pscustomobject]@{ repository = 'https://github.com/example/list'; commit = $pslCommit; file = 'list.dat'; comment = 'the file psl_data.cpp is made from' } } }
    $reviewed = & $newAttributions
    $review = Resolve-AttributionPolicy -Port 'qtlike' -Entry $qtEntry -Attributions $reviewed -Sources $qtSources -DownloadsDir $downloadsDir -Policy $tpPolicy -RepoRoot $tpRoot
    Assert-True (@($review.Errors).Count -eq 0) "Round 2: the policy's review of Qt's records is shown by the verified sources ($(@($review.Errors) -join ' | '))"
    Assert-True ($reviewed[0].License -ceq 'MIT-Khronos-old' -and $reviewed[0].QtLicense -ceq 'MIT' -and $reviewed[1].License -ceq 'Apache-2.0 AND OGL-UK-3.0' -and $reviewed[1].TextsElsewhere.ContainsKey('OGL-UK-3.0')) `
        'Round 2: the licence the code carries replaces Qt''s record, which is kept beside it (Khronos'' headers: MIT-Khronos-old)'
    $noticeFile = @($review.Notices)[0]
    Assert-True ($reviewed[1].NoticeFile -ceq 'qtlike\Upstream-NOTICE.txt' -and $noticeFile.Text.Contains("Upstream`nCopyright 2021 The Foundation") -and $noticeFile.Text.Contains('under the Open Licence 3.0') -and
                 -not $noticeFile.Text.Contains('Other lines.') -and ($noticeFile.Text -replace '\s+', ' ').Contains("git blob $($tpBlobs['NOTICE.txt'])")) 'Round 2: the NOTICE lines that pertain to the code are taken verbatim, with their provenance, and the others left out'
    Assert-True ($reviewed[2].SourceCodeForm.Url -ceq "https://github.com/example/list/blob/$pslCommit/list.dat") "Round 2: the Source Code Form of the MPL code is named at its commit ($($reviewed[2].SourceCodeForm.Url))"
    $stale = & $newAttributions
    $stale[0].License = 'BSD-3-Clause'
    Assert-Problem (Resolve-AttributionPolicy -Port 'qtlike' -Entry $qtEntry -Attributions $stale -Sources $qtSources -DownloadsDir $downloadsDir -Policy $tpPolicy -RepoRoot $tpRoot).Errors `
        "reviewed its record 'MIT'" 'Round 2: a correction of a record that Qt has changed is refused'
    $moved = & $newAttributions
    $moved[2].Purl = 'pkg:generic/psl-data@' + ('ef' * 20)
    Assert-Problem (Resolve-AttributionPolicy -Port 'qtlike' -Entry $qtEntry -Attributions $moved -Sources $qtSources -DownloadsDir $downloadsDir -Policy $tpPolicy -RepoRoot $tpRoot).Errors `
        'not as commit .* that installer\\license-policy\.json names as its Source Code Form' 'Round 2: a Source Code Form at another commit than Qt''s record is refused'
    $noSource = [pscustomobject]@{ attributionLicences = $qtEntry.attributionLicences; attributionNotices = $qtEntry.attributionNotices }
    Assert-Problem (Resolve-AttributionPolicy -Port 'qtlike' -Entry $noSource -Attributions (& $newAttributions) -Sources $qtSources -DownloadsDir $downloadsDir -Policy $tpPolicy -RepoRoot $tpRoot).Errors `
        'The Public Suffix List \(MPL-2\.0\) .* names no Source Code Form' 'Round 2: compiled MPL code without its Source Code Form named is refused (the Public Suffix List before this review)'
    $unknown = [pscustomobject]@{ attributionSources = [pscustomobject]@{ 'psl-data' = $qtEntry.attributionSources.'psl-data'; 'gone' = $qtEntry.attributionSources.'psl-data' } }
    Assert-Problem (Resolve-AttributionPolicy -Port 'qtlike' -Entry $unknown -Attributions (& $newAttributions) -Sources $qtSources -DownloadsDir $downloadsDir -Policy $tpPolicy -RepoRoot $tpRoot).Errors `
        'attributionSources names gone, which Qt''s SBOM does not record' 'Round 2: a review of a record Qt''s SBOM does not hold is refused'
    $qtPolicyEntry = $realPolicy.ports.qtbase
    Assert-True ($qtPolicyEntry.attributionLicences.'opengl-headers'.licence -ceq 'MIT-Khronos-old' -and $qtPolicyEntry.attributionLicences.'opengl-es2-headers'.licence -ceq 'MIT-Khronos-old' -and
                 $qtPolicyEntry.attributionLicences.'tika-mimetypes'.licence -ceq 'Apache-2.0 AND OGL-UK-3.0' -and $qtPolicyEntry.attributionNotices.'tika-mimetypes'.as -ceq 'Apache-Tika-NOTICE.txt' -and
                 $qtPolicyEntry.attributionSources.'psl-data'.commit -ceq '944fb53321606f08740f04ef7c386f64df34ff76' -and
                 (Test-LicenceInExpression -Licence 'MIT-Khronos-old' -Expression $qtPolicyEntry.concluded) -and (Test-LicenceInExpression -Licence 'OGL-UK-3.0' -Expression $qtPolicyEntry.concluded)) `
        'Round 2: the policy corrects Khronos'' licence, takes Tika''s NOTICE and names the Public Suffix List''s Source Code Form, and qtbase''s licence covers them'

    # 13f. Code of a Windows SDK library inside shipped binaries (C++/WinRT in Qt6Core.dll and qwindows.dll).
    $kits = Join-Path $work 'kits'
    New-Item -ItemType Directory -Path (Join-Path $kits 'Include\10.0.1.0\cppwinrt\winrt') -Force | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $kits 'Include\10.0.1.0\cppwinrt\LICENSE.txt'), "C++/WinRT test licence`r`nMIT License`r`n", $utf8)
    [System.IO.File]::WriteAllText((Join-Path $kits 'Include\10.0.1.0\cppwinrt\winrt\base.h'), "#define CPPWINRT_VERSION `"2.0.1`"`n", $utf8)
    $sdkStage = Join-Path $work 'sdk-stage'
    New-Item -ItemType Directory -Path (Join-Path $sdkStage 'plugins') -Force | Out-Null
    [System.IO.File]::WriteAllBytes((Join-Path $sdkStage 'Qt6Core.dll'), [byte[]](@(0x4D, 0x5A, 0) + @([System.Text.Encoding]::ASCII.GetBytes('C++/WinRT version:2.0.1')) + @(0, 0)))
    [System.IO.File]::WriteAllBytes((Join-Path $sdkStage 'plugins\qwin.dll'), [byte[]](@(0x4D, 0x5A, 0) + @([System.Text.Encoding]::ASCII.GetBytes('xC++/WinRT version:2.0.1')) + @(0)))
    [System.IO.File]::WriteAllBytes((Join-Path $sdkStage 'z.dll'), [byte[]]@(0x4D, 0x5A, 0, 0))
    $sdkReport = [pscustomobject]@{ Files = @(
        [pscustomobject]@{ Path = 'Qt6Core.dll'; Origin = 'vcpkg'; Port = 'qtbase' }, [pscustomobject]@{ Path = 'plugins\qwin.dll'; Origin = 'vcpkg'; Port = 'qtbase' },
        [pscustomobject]@{ Path = 'z.dll'; Origin = 'vcpkg'; Port = 'zlib' }) }
    $sdkEntry = [pscustomobject]@{ kit = 'windows-sdk'; name = 'C++/WinRT'; reviewedSdk = '10.0.1.0'; reviewedVersion = '2.0.1'; declared = 'MIT'; concluded = 'MIT'; copyright = 'Copyright (c) Microsoft Corporation'
        licenseFile = 'cppwinrt/LICENSE.txt'; licenseSha256 = (Get-Sha256 (Join-Path $kits 'Include\10.0.1.0\cppwinrt\LICENSE.txt')); versionFile = 'cppwinrt/winrt/base.h'
        versionLine = '#define CPPWINRT_VERSION "2.0.1"'; probe = 'C++/WinRT version:'; inside = @('qtbase') }
    $savedSdk = $env:WindowsSdkDir
    try {
        $env:WindowsSdkDir = $kits
        $sdk = Get-WindowsSdkLibrary -Id 'cppwinrt' -Entry $sdkEntry -Info $tcInfo -StageDir $sdkStage -Report $sdkReport
        Assert-True ((@($sdk.Binaries) -join '|') -ceq 'Qt6Core.dll|plugins\qwin.dll' -and (@($sdk.Inside) | ForEach-Object { $_.Port }) -join '|' -ceq 'qtbase' -and $sdk.LicenceFile.File -ceq 'cppwinrt\LICENSE.txt' -and $sdk.Kit -ceq 'Windows SDK 10.0.1.0') `
            "Round 2: C++/WinRT is found in every shipped binary that carries its version, inside the port the policy names ($(@($sdk.Binaries) -join ', '))"
        [System.IO.File]::WriteAllBytes((Join-Path $sdkStage 'z.dll'), [byte[]](@(0x4D, 0x5A) + @([System.Text.Encoding]::ASCII.GetBytes('C++/WinRT version:2.0.1')) + @(0)))
        Assert-Throws { Get-WindowsSdkLibrary -Id 'cppwinrt' -Entry $sdkEntry -Info $tcInfo -StageDir $sdkStage -Report $sdkReport } 'Round 2: the code in a binary of a port the policy does not name is refused'
        [System.IO.File]::WriteAllBytes((Join-Path $sdkStage 'z.dll'), [byte[]](@(0x4D, 0x5A) + @([System.Text.Encoding]::ASCII.GetBytes('C++/WinRT version:2.0.2')) + @(0)))
        Assert-Throws { Get-WindowsSdkLibrary -Id 'cppwinrt' -Entry $sdkEntry -Info $tcInfo -StageDir $sdkStage -Report $sdkReport } 'Round 2: another version of the code is refused until it is reviewed'
        [System.IO.File]::WriteAllBytes((Join-Path $sdkStage 'z.dll'), [byte[]]@(0x4D, 0x5A, 0, 0))
        $sdkEntry.licenseSha256 = '0' * 64
        Assert-Throws { Get-WindowsSdkLibrary -Id 'cppwinrt' -Entry $sdkEntry -Info $tcInfo -StageDir $sdkStage -Report $sdkReport } 'Round 2: another licence file is refused until it is reviewed'
    }
    finally { $env:WindowsSdkDir = $savedSdk }
    $cppwinrt = $realPolicy.toolchainLibraries.cppwinrt
    Assert-True ($cppwinrt.kit -ceq 'windows-sdk' -and $cppwinrt.reviewedVersion -ceq '2.0.250303.1' -and $cppwinrt.reviewedSdk -ceq '10.0.26100.0' -and $cppwinrt.licenseSha256 -match '^f6048594' -and
                 $cppwinrt.probe -ceq 'C++/WinRT version:' -and (@($cppwinrt.inside) -join '|') -ceq 'qtbase') 'Round 2: the policy records C++/WinRT 2.0.250303.1 of the Windows SDK 10.0.26100.0, its licence pinned and its probe'
    Assert-True ($realPolicy.ports.qtbase.comment.Contains('d3d12.h') -and $realPolicy.ports.qtbase.comment.Contains('dxcapi.h') -and $realPolicy.ports.qtbase.comment.Contains('declarations and GUID constants only')) `
        'Round 2: qtbase''s note records that the SDK''s D3D12 and DXC headers give Qt6Gui.dll declarations and GUID constants only'

    # 13g. The code of others inside Inno Setup's loader, engine and uninstaller, with their licences and, for code
    # under a copyleft licence, its source.
    $innoFake = Join-Path $work 'inno-fake'
    New-Item -ItemType Directory -Path $innoFake -Force | Out-Null
    [System.IO.File]::WriteAllBytes((Join-Path $innoFake 'Setup.e32'), [byte[]](@([System.Text.Encoding]::ASCII.GetBytes('MZ engine RemObjects Pascal Script. Copyright (c) test ')) +
        @([System.Text.Encoding]::Unicode.GetBytes('home page: https://example.org/ps')) + @([System.Text.Encoding]::ASCII.GetBytes(' 7z Decoder 1.0 : Someone : Public domain '))))
    [System.IO.File]::WriteAllBytes((Join-Path $innoFake 'SetupLdr.e32'), [byte[]](@(0x4D, 0x5A) + @([System.Text.Encoding]::Unicode.GetBytes('smalldecoder: corrupted'))))
    [System.IO.File]::WriteAllBytes((Join-Path $innoFake 'ISCC.exe'), [byte[]]@(0x4D, 0x5A))
    $isccFake = [pscustomobject]@{ Path = (Join-Path $innoFake 'ISCC.exe'); Version = 'Inno Setup 9.9.9' }
    $scriptPart = [pscustomobject]@{ name = 'Script engine'; copyright = 'c'; licence = 'LicenseRef-Script'; in = @('Setup.e32'); probe = 'RemObjects Pascal Script. Copyright (c) test'
        text = [pscustomobject]@{ source = 'upstream-abc'; file = 'LICENSE.md' }; as = 'Script-LICENSE.txt'; notice = 'Made using the script engine, at https://example.org/ps.'; noticeProbe = 'https://example.org/ps'; comment = 'Its licence asks for a line.' }
    $pdPart = [pscustomobject]@{ name = 'PD decoder'; version = '1.0'; copyright = 'Someone : Public domain'; licence = 'LicenseRef-PD'; in = @('Setup.e32'); probe = '7z Decoder 1.0 : Someone : Public domain'; statement = 'probe'; as = 'PD.txt'; comment = 'Public domain.' }
    $lgplPart = [pscustomobject]@{ name = 'Small decoder'; version = '4.40'; copyright = 'c'; licence = 'LGPL-2.1-only'; declared = 'LicenseRef-Dec'; in = @('SetupLdr.e32', 'SetupLdr.e64'); probe = 'smalldecoder'
        text = [pscustomobject]@{ source = 'upstream-abc'; file = 'src/dec.h'; first = 'Decoder terms'; last = 'end of terms'; about = 'Its terms' }
        textIn = @([pscustomobject]@{ file = 'gettext-libintl\copyright.txt'; marker = 'Version 2.1'; holds = 'the LGPL 2.1' })
        sourceFiles = [pscustomobject]@{ source = 'upstream-abc'; folder = 'Dec'; files = @('src/dec.c', 'src/dec.h') }; as = 'Dec.txt'; comment = 'Under the LGPL.' }
    $innoPartsPolicy = [pscustomobject]@{ thirdPartySources = $tpPolicy.thirdPartySources; distribution = [pscustomobject]@{ installerTool = [pscustomobject]@{ parts = @($scriptPart, $pdPart, $lgplPart) } } }
    $innoParts = @(Get-InnoSetupParts -Iscc $isccFake -Policy $innoPartsPolicy -RepoRoot $tpRoot)
    Assert-True ($innoParts.Count -eq 3 -and $innoParts[0].Text.Contains('Script engine licence') -and ($innoParts[0].Text -replace '\s+', ' ').Contains("RemObjects Pascal Script. Copyright (c) test") -and $innoParts[0].Notice -and
                 $innoParts[0].ExtractedText.StartsWith('Script engine licence') -and $innoParts[0].Inside -ceq 'the setup engine inside Lucent Setup and its uninstaller, unins000.exe') `
        'Round 2: a part of Inno Setup is found by its text in the compiler''s engine, with its licence from the published source and the notice it asks for'
    Assert-True ($innoParts[1].ExtractedText -ceq '7z Decoder 1.0 : Someone : Public domain' -and $innoParts[1].Text.Contains('states its terms in that text itself')) 'Round 2: public-domain code states its terms in its own text'
    Assert-True ((@($innoParts[2].SourceFiles | ForEach-Object { $_.File }) -join '|') -ceq 'inno-setup\Dec\dec.c|inno-setup\Dec\dec.h' -and (@($innoParts[2].Holding) -join '|') -ceq 'SetupLdr.e32' -and
                 $innoParts[2].Inside -ceq "Setup's loader" -and $innoParts[2].Declared -ceq 'LicenseRef-Dec' -and $innoParts[2].ExtractedText.Contains('LGPL or CPL')) `
        'Round 2: the LGPL decoder of Setup''s loader ships its source and its own terms'
    $noSourcePart = [pscustomobject]@{ name = 'Small decoder'; copyright = 'c'; licence = 'LGPL-2.1-only'; in = @('SetupLdr.e32'); probe = 'smalldecoder'; statement = 'probe'; as = 'Dec.txt'; comment = 'c' }
    Assert-Throws { Get-InnoSetupParts -Iscc $isccFake -Policy ([pscustomobject]@{ thirdPartySources = $tpPolicy.thirdPartySources; distribution = [pscustomobject]@{ installerTool = [pscustomobject]@{ parts = @($noSourcePart) } } }) -RepoRoot $tpRoot } `
        'Round 2: code under a copyleft licence inside Inno Setup without its source is refused'
    $absentPart = [pscustomobject]@{ name = 'Absent'; copyright = 'c'; licence = 'MIT'; in = @('Setup.e32'); probe = 'not in the engine'; statement = 'probe'; as = 'A.txt'; comment = 'c' }
    Assert-Throws { Get-InnoSetupParts -Iscc $isccFake -Policy ([pscustomobject]@{ distribution = [pscustomobject]@{ installerTool = [pscustomobject]@{ parts = @($absentPart) } } }) -RepoRoot $tpRoot } `
        'Round 2: a part the compiler''s files do not hold is refused'
    Assert-True (@(Get-InnoSetupParts -Iscc $null -Policy $innoPartsPolicy -RepoRoot $tpRoot).Count -eq 0) 'Round 2: without a compiler there are no parts'
    $tool = $realPolicy.distribution.installerTool
    $lzma = @($tool.parts | Where-Object { $_.licence -ceq 'LGPL-2.1-only' })[0]
    Assert-True ((@($tool.parts | ForEach-Object { $_.licence }) -join '|') -ceq 'LicenseRef-RemObjects-Pascal-Script|Zlib|LicenseRef-LZMA-SDK-Public-Domain|LGPL-2.1-only' -and
                 @($lzma.sourceFiles.files).Count -eq 5 -and (@($lzma.in) -join '|') -ceq 'SetupLdr.e32' -and $tool.licenseGitBlob -ceq 'ac4dfe9e6543a360c5422b6f1fb88075e8b6073d' -and $tool.sourceCommit -match '^4adf37ed') `
        'Round 2: the policy names Pascal Script, zlib and the LZMA SDK''s decoders inside Inno Setup, the 4.40 decoder of the loader Lucent Setup is made with (SetupLdr.e32, a 32-bit program like Setup) with its five source files, and license.txt''s blob in Inno Setup''s published source'

    # 13h. The programs' whole licences: Lucent's, the logo's and those of the code compiled or linked into each.
    $programComponents = @(
        [pscustomobject]@{ Port = 'jsonlib'; Version = '3'; Linkage = 'compiled-in'; LicenseConcluded = 'MIT AND CC0-1.0'; StaticLibraries = @() },
        [pscustomobject]@{ Port = 'expected'; Version = '1'; Linkage = 'compiled-in'; LicenseConcluded = 'CC0-1.0'; StaticLibraries = @() },
        [pscustomobject]@{ Port = 'qtlike'; Version = '6'; Linkage = 'dynamic'; LicenseConcluded = 'LGPL-3.0-only'; StaticLibraries = @([pscustomobject]@{ Name = 'Entry point'; File = 'Entry.lib'; LicenseConcluded = 'BSD-3-Clause'; Executables = @('Lucent.exe') }) })
    $programTools = @([pscustomobject]@{ Id = 'msvc-stl'; Version = '14'; LicenseConcluded = 'Apache-2.0 WITH LLVM-exception AND BSL-1.0'; Programs = @('Lucent.exe', 'lucent-worker.exe') })
    $programLicences = Get-ProgramLicences -Info $sbomInfo -Components $programComponents -ToolchainLibraries $programTools -OwnLicence 'MIT AND LicenseRef-Logo'
    Assert-True ($programLicences['lucent.exe'].Licence -ceq 'MIT AND LicenseRef-Logo AND CC0-1.0 AND BSD-3-Clause AND Apache-2.0 WITH LLVM-exception AND BSL-1.0' -and
                 $programLicences['lucent-worker.exe'].Licence -ceq 'MIT AND LicenseRef-Logo AND CC0-1.0 AND Apache-2.0 WITH LLVM-exception AND BSL-1.0') `
        "Round 2: each program's licence names what is compiled or linked into it ($($programLicences['lucent.exe'].Licence) / $($programLicences['lucent-worker.exe'].Licence))"
    $ownProgram = Get-OwnFileLicence -Relative 'lucent-worker.exe' -Info $sbomInfo -ProgramLicence 'MIT AND LicenseRef-Logo' -ProgramLicences $programLicences
    Assert-True ($ownProgram.Program -and $ownProgram.Licence -ceq $programLicences['lucent-worker.exe'].Licence) 'Round 2: the SBOM gives a program''s file its whole licence'

    # 13i. The wording of the notices and the archive's README, and the programs' and Setup's copyright lines.
    $otherFiles = @(@(1..174 | ForEach-Object { "qml\QtQuick\Controls\Basic\F$_.qml" }) + @(1..19 | ForEach-Object { "qml\M$_\qmldir" }) + @(1..19 | ForEach-Object { "qml\M$_\plugins.qmltypes" }))
    $otherText = Format-OtherFiles -Files $otherFiles
    Assert-True ($otherText -ceq '174 QML source files (.qml), 19 module definitions (qmldir) and 19 type information files (.qmltypes)') "Round 2: Qt Declarative's other files are counted by kind ($otherText)"
    $qmlStage = Join-Path $work 'qml-notice'
    Write-LgplNotice -StageDir $qmlStage -Info ([pscustomobject]@{ version = '9.8.7'; homepage = 'https://example.org/lucent'; executable = 'Lucent.exe'; helperExecutables = @('lucent-worker.exe') }) `
        -Components @([pscustomobject]@{ Port = 'qtdecl'; Version = '6'; LicenseConcluded = 'LGPL-3.0-only'; Lgpl = $true; Files = @(@('Qt6Qml.dll') + $otherFiles); LicenceTexts = @('qtdecl\copyright.txt')
            Sources = @(); RecipeRef = [pscustomobject]@{ Overlay = $null; Tree = $null; Url = $null }; PortVersion = '6'; StaticLibraries = @() }) -NoticesDir (Join-Path $repoRoot 'installer\notices') `
        -SourcePackage ([pscustomobject]@{ File = 'Lucent-9.8.7-corresponding-source.zip'; Size = [int64]1; Sha256 = ('ab' * 32); Components = @([pscustomobject]@{ Port = 'qtdecl'; Archives = @(); Modifications = @() }) }) -Baseline ('e143402b' + '0' * 32)
    $qmlText = [System.IO.File]::ReadAllText((Join-Path $qmlStage 'licenses\LGPL-NOTICE.txt')) -replace '\s+', ' '
    Assert-True ($qmlText.Contains('and 212 other files: 174 QML source files (.qml), 19 module definitions (qmldir) and 19 type information files (.qmltypes)') -and -not $qmlText.Contains('QML module files (qmldir, type information)')) `
        'Round 2: LGPL-NOTICE.txt counts the QML source files too'
    $flatTemplate = $noticesTemplate -replace '\s+', ' '
    Assert-True ($flatTemplate.Contains('The full copyright and licence text of each vcpkg component is in the folder named after it') -and $flatTemplate.Contains('(its note says why its compiled code needs no licence text)') -and
                 -not $flatTemplate.Contains('standard words') -and $noticesTemplate.Contains('@VC_REDIST_TERMS@')) `
        'Round 2: the notices limit the sentence on copyright.txt to vcpkg components, name no words Lucent supplies, and name the redistributable''s terms'
    $withTerms = Complete-ThirdPartyNotices @noticeArgs -InnoSetup 'Inno Setup 6.7.3' -Recipes '' -Programs 'Lucent.exe and lucent-worker.exe' -InnoSetupNote '' -RedistributableTerms 'MICROSOFT SOFTWARE LICENSE TERMS - TEST (EULA ID T.1033)'
    Assert-True (($withTerms -replace '\s+', ' ').Contains("Microsoft's licence terms for it are the MICROSOFT SOFTWARE LICENSE TERMS - TEST (EULA ID T.1033), which that installer shows when it is run on its own.") -and
                 ($withTerms -replace '\s+', ' ').Contains('Microsoft Visual C++ Redistributable (x64) 14.44.35211.0 (vc_redist.x64.exe)')) 'Round 2: the notices name the terms of the redistributable Setup runs without its window'
    $longTerms = 'MICROSOFT SOFTWARE LICENSE TERMS - MICROSOFT VISUAL C++ 2015 - 2022 RUNTIME (EULA ID Cpp_2015-2022_ENU.1033)'
    $innoList = "  Its text is inno-setup\license.txt in this folder.`n  Code of others inside Inno Setup, each with the file of its licence in inno-setup\ in this folder:`n" +
        "    A part whose name and place are long enough to take more than one hundred columns on its line: LicenseRef-Test (inno-setup\a.txt)"
    $wrappedNotices = Complete-ThirdPartyNotices @noticeArgs -InnoSetup 'Inno Setup 6.7.3' -Recipes '' -Programs 'Lucent.exe and lucent-worker.exe' -InnoSetupNote $innoList -RedistributableTerms $longTerms
    $introText = $wrappedNotices.Substring(0, $wrappedNotices.IndexOf("`nRequired notices`n", [System.StringComparison]::Ordinal))
    Assert-True (@(($introText -split "`n") | Where-Object { $_.Length -gt 100 -and -not $_.StartsWith('    ') }).Count -eq 0 -and $wrappedNotices.Contains("`n$innoList`n") -and
                 ($wrappedNotices -replace '\s+', ' ').Contains("Microsoft's licence terms for it are the $longTerms, which that installer shows when it is run on its own.")) `
        'Round 2: the redistributable''s bullet is wrapped once its long terms are filled in, and a bullet with a list of its own keeps its lines'
    $readmeTemplate =[System.IO.File]::ReadAllText((Join-Path $repoRoot 'installer\notices\CORRESPONDING-SOURCE-README.txt.in'))
    $flatReadme = $readmeTemplate -replace '\s+', ' '
    Assert-True (-not $flatReadme.Contains("its texts are in its source archive, and in licenses\<port>\") -and $flatReadme.Contains('Its texts are in its source archive where that archive holds them.') -and
                 $readmeTemplate.Contains('@LICENCE_TEXTS@') -and $flatReadme.Contains('the lines it quotes and changes stay under the licences of the files they come from')) `
        'Round 2: the README says where each licence''s text is when the library''s archive lacks it, and what a patch''s quoted lines are under'
    $branding = [System.IO.File]::ReadAllText((Join-Path $repoRoot 'cmake\LucentBranding.cmake'))
    $iss = [System.IO.File]::ReadAllText((Join-Path $repoRoot 'installer\lucent.iss'))
    Assert-True ($branding.Contains('set(LUCENT_RC_COPYRIGHT "Copyright (c) 2026 Fan Zhu. MIT License.")') -and $iss.Contains('AppCopyright=Copyright (c) 2026 Fan Zhu. MIT License.')) `
        'The programs'' and Setup''s copyright lines stay as the owner decided on 2026-10-15 (the notices state the exceptions)'

    # 13j. The rest of the policy's corrections.
    $vcPolicy = $realPolicy.vcRedistributable
    Assert-True (-not $vcPolicy.licenseText.Contains('The terms are shown by the redistributable installer') -and $vcPolicy.licenseText.Contains('/install /quiet /norestart') -and
                 $vcPolicy.reviewed.eulaId -ceq 'Cpp_2015-2022_ENU.1033' -and $vcPolicy.reviewed.version -ceq '14.44.35211.0' -and $vcPolicy.termsReview.status -in 'pending', 'reviewed') `
        'Round 2: the redistributable''s terms are named truthfully (Setup runs it silently), pinned to the reviewed redistributable, and the owner''s review is recorded'
    $ports = $realPolicy.ports
    $simd = @($ports.'libjpeg-turbo'.parts)[0]
    Assert-True ($simd.copyright.Contains('Intel Corporation') -and $simd.copyright.Contains('Institute of Software, Chinese Academy of Sciences') -and $simd.copyright.Contains('Samsung Electronics')) 'Round 2: the SIMD part names the holders of every compiled SIMD file'
    Assert-True ($ports.'gettext-libintl'.comment.Contains('alloca.in.h') -and $ports.libiconv.comment.Contains('unicode.org') -and $ports.lensfun.comment.Contains('libs/getopt') -and
                 $ports.libraw.comment.Contains('DNGLab') -and $ports.libraw.comment.Contains('LibRaw-cmake') -and $realPolicy.toolchainLibraries.'msvc-stl'.comment.Contains('4(a), 4(b) and 4(d)')) `
        'Round 2: the notes record gnulib''s alloca.h, libiconv''s Unicode-derived tables, Lensfun''s getopt, DNGLab and LibRaw-cmake, and why msvc-stl ships no licence text'
}
finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "$($script:checks) checks, $($script:failures) failed"
if ($script:failures -gt 0) { exit 1 }
exit 0
