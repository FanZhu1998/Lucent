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

    With -PackageManifest <Lucent-Setup-<version>-x64.manifest.json> (schema 5): the archive beside it
    (or -SourcePackage) must have the size and SHA-256 the manifest records and hold exactly the files
    it lists; every component the manifest marks "correspondingSource" must be in the archive with its
    source archives (by SHA-512) and recipe files (by SHA-256), and no component whose licence leaves
    no choice but a copyleft licence may lack the mark (Test-CorrespondingSourcePackage).

    Registered with CTest as lucent_source_package_tests (label "installer"). Exit code 0 when every
    check passed. Windows PowerShell 5.1 and PowerShell 7. Keep this file ASCII.

.EXAMPLE
    powershell -NoProfile -File installer\tests\Test-SourcePackage.ps1
.EXAMPLE
    powershell -NoProfile -File installer\tests\Test-SourcePackage.ps1 -PackageManifest build\release\package\Lucent-Setup-0.1.0-x64.manifest.json
#>
[CmdletBinding()]
param(
    [string]$PackageManifest,
    [string]$SourcePackage
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
    Assert-True (@($result.Errors).Count -eq 0) 'the archive holds every recorded archive and recipe file, in its deterministic form'
    $listed = @{}
    foreach ($file in @($record.files)) { $listed[[string]$file.path] = $file }
    Assert-True (@($result.Entries).Count -eq $listed.Count) "the archive holds exactly the $($listed.Count) files the manifest lists ($(@($result.Entries).Count))"
    foreach ($entry in $result.Entries) {
        $file = $listed[$entry.Name]
        Assert-True ($null -ne $file -and [int64]$file.size -eq $entry.Length -and $file.sha256 -eq $entry.Sha256) "$($entry.Name) is listed with its size and SHA-256"
    }
    Write-Host ("{0}: {1:N1} MiB, {2} files, {3} components: {4}" -f $item.Name, ($item.Length / 1MB), @($result.Entries).Count, $provided.Count, ($provided -join ', '))
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
            'build/liba/vcpkg_abi_info.txt' = $abiInfo }
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
    Assert-True (@($complete.Entries).Count -eq 7) "it lists its seven files ($(@($complete.Entries).Count))"
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
}
finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "$($script:checks) checks, $($script:failures) failed"
if ($script:failures -gt 0) { exit 1 }
exit 0
