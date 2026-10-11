<#
.SYNOPSIS
    Self-test of the packaging audit's builder-path scan (installer\scripts\LucentPackaging.psm1,
    Invoke-BuilderPathAudit; docs/adr/0009-windows-packaging.md, decision 12).

.DESCRIPTION
    Builds the scanner for a made-up build machine (user profile C:\Users\builder, repository
    D:\src\Lucent, vcpkg in D:\vcpkg with buildtrees in C:\vb) and checks that it finds those
    folders in 8-bit and UTF-16LE text, with either separator, doubled separators (JSON and C
    strings), any letter case, after \\?\ and in the MSYS form, and any other user's profile
    folder; that it ignores text that only resembles them (Public and Default profiles, URLs,
    registry paths, other folders that share a prefix's letters); that a match inside a PE file's
    debug directory is reported as the PDB path, with the offset of the drive letter; and that the
    folder audit names the file and offset of every hit. Needs no build and no developer tools.

    Compressed content (W12 review SEC-07): paths planted in a zlib-compressed rcc resource, a
    gzip member, a zlib stream inside a zlib stream, and in the LZMA-compressed setup data and
    LZMA2-compressed installed files of a synthetic Inno Setup 6.7 executable are found and named by
    their compressed part; a zstd frame and an Inno Setup layout the reader cannot decode are errors.
    With -InnoSetupCompiler a real Setup is compiled with planted paths and scanned too.
    Exit code 0 when every check passes.

.PARAMETER InnoSetupCompiler
    Optional ISCC.exe (Inno Setup 6.4 or later): also compile a probe Setup with planted paths in its
    compiled script and an installed file, and check the scan finds both.

.EXAMPLE
    powershell -NoProfile -File installer\tests\Test-BuilderPathScan.ps1
.EXAMPLE
    powershell -NoProfile -File installer\tests\Test-BuilderPathScan.ps1 -InnoSetupCompiler 'C:\Program Files (x86)\Inno Setup 6\ISCC.exe'

.NOTES
    Windows PowerShell 5.1 compatible. Keep this file ASCII.
#>
[CmdletBinding()]
param([string]$InnoSetupCompiler)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0
Import-Module (Join-Path (Split-Path -Parent $PSScriptRoot) 'scripts\LucentPackaging.psm1') -Force

$failures = 0
$checks = 0
function Assert-That([string]$Name, [bool]$Condition, [string]$Detail = '') {
    $script:checks++
    if ($Condition) { Write-Host "[PASS] $Name" -ForegroundColor Green }
    else { $script:failures++; Write-Host "[FAIL] $Name$(if ($Detail) { " - $Detail" })" -ForegroundColor Red }
}

# A build machine unlike this one, so the result does not depend on where the test runs. The
# environment's folders are cleared for the rules (Get-BuilderPathRules also reads them).
$info = [pscustomobject]@{
    vcpkgInstalledDir = 'D:/src/Lucent/build/release/vcpkg_installed'; binaryDir = 'D:/src/Lucent/build/release'
    sourceDir = 'D:/src/Lucent'; userProfile = 'C:/Users/builder'; vcpkgRoot = 'D:/vcpkg'
    vcpkgBuildtreesDir = 'C:/vb'; vcpkgPackagesDir = 'D:/vcpkg/packages'; vcpkgDownloadsDir = 'D:/vcpkg/downloads' }
$saved = @{}
foreach ($name in 'USERPROFILE', 'VCPKG_ROOT', 'LUCENT_VCPKG_BUILDTREES', 'VCPKG_DOWNLOADS') {
    $saved[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
    [Environment]::SetEnvironmentVariable($name, $null, 'Process')
}
try { $rules = Get-BuilderPathRules -Info $info -RepoRoot 'D:\src\Lucent' }
finally { foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name], 'Process') } }
$scanner = New-BuilderPathScanner -Rules $rules
Assert-That 'Rules: most specific folder first' ($rules[0].Path -eq 'D:\src\Lucent\build\release\vcpkg_installed') ($rules[0].Path)
Assert-That 'Rules: only the user profile matches without its drive letter' (@($rules | Where-Object DriveOptional).Count -eq 1 -and @($rules | Where-Object DriveOptional)[0].Path -eq 'C:\Users\builder')

function Find-In([string]$Text, [System.Text.Encoding]$Encoding) {
    $bytes = $Encoding.GetBytes($Text)
    $file = Join-Path ([System.IO.Path]::GetTempPath()) ('lucent-path-scan-' + [guid]::NewGuid().ToString('N') + '.bin')
    [System.IO.File]::WriteAllBytes($file, $bytes)
    try { return Invoke-BuilderPathAudit -Scanner $scanner -Path $file -Name 'sample' }
    finally { Remove-Item -LiteralPath $file -Force }
}

$found = @(
    @('C:\Users\builder\AppData\Local\Temp\x.obj', 'user profile'),
    @('c:/users/BUILDER/x', 'user profile'),
    @('"C:\\Users\\builder\\x"', 'user profile'),
    @('/c/Users/builder/x', 'user profile'),
    @('\\?\C:\Users\builder\x', 'user profile'),
    @('E:\Users\builder\x', 'user profile'),
    @('D:\src\Lucent\src\core\a.cpp', 'source tree'),
    @('d:/src/lucent/build/release/apps/x.cpp', 'build tree'),
    @('D:\src\Lucent\build\release\vcpkg_installed\x64-windows-lucent\include\x.h', 'vcpkg installed tree'),
    @('C:\vb\tiff\src\v4.7.2.clean\libtiff\tif_dir.c', 'vcpkg buildtrees folder'),
    @('D:\vcpkg\packages\zlib_x64\bin\z.pdb', 'vcpkg packages folder'),
    @('D:\vcpkg\ports\zlib\portfile.cmake', 'vcpkg root'),
    @('C:\Users\runneradmin\work\qt\qtbase.pdb', 'user profile folder (any user)'))
foreach ($case in $found) {
    foreach ($encoding in [System.Text.Encoding]::UTF8, [System.Text.Encoding]::Unicode) {
        $report = Find-In ("prefix $($case[0]) suffix") $encoding
        $rule = @($report.Hits | ForEach-Object Rule) -join ', '
        Assert-That ("Found in {0}: {1}" -f $encoding.WebName, $case[0]) (@($report.Hits).Count -eq 1 -and $rule -eq $case[1]) "hits: $(@($report.Hits).Count), rule: $rule"
    }
}

$ignored = @('C:\Users\Public\Documents\x', 'C:\Users\Default\x', 'C:\Users\All Users\x', 'https://github.com/microsoft/vcpkg',
             'Software\Lucent\Settings', 'C:\temp\qt-user', 'D:\src\Lucid\x', 'X:\vbox\x', 'C:\vcpkg\x', 'Users\builder without a separator before')
foreach ($text in $ignored) {
    foreach ($encoding in [System.Text.Encoding]::UTF8, [System.Text.Encoding]::Unicode) {
        $report = Find-In ("prefix $text suffix") $encoding
        Assert-That ("Ignored in {0}: {1}" -f $encoding.WebName, $text) (@($report.Hits).Count -eq 0) (@($report.Errors) -join '; ')
    }
}

# A minimal PE image whose debug directory holds a CodeView record with a PDB path.
function New-PeImage([string]$PdbPath, [string]$DataText) {
    $image = New-Object byte[] 0x600
    $image[0] = 0x4D; $image[1] = 0x5A                                  # MZ
    [BitConverter]::GetBytes([int]0x80).CopyTo($image, 0x3C)              # e_lfanew
    [BitConverter]::GetBytes([uint32]0x4550).CopyTo($image, 0x80)         # PE\0\0
    [BitConverter]::GetBytes([uint16]0x8664).CopyTo($image, 0x84)         # x64
    [BitConverter]::GetBytes([uint16]1).CopyTo($image, 0x86)              # one section
    [BitConverter]::GetBytes([uint16]0xF0).CopyTo($image, 0x94)           # SizeOfOptionalHeader
    $optional = 0x98
    [BitConverter]::GetBytes([uint16]0x20B).CopyTo($image, $optional)     # PE32+
    [BitConverter]::GetBytes([uint32]0x1000).CopyTo($image, $optional + 112 + 6 * 8)   # debug directory RVA
    [BitConverter]::GetBytes([uint32]28).CopyTo($image, $optional + 112 + 6 * 8 + 4)   # one entry
    $section = $optional + 0xF0
    [BitConverter]::GetBytes([uint32]0x400).CopyTo($image, $section + 8)  # VirtualSize
    [BitConverter]::GetBytes([uint32]0x1000).CopyTo($image, $section + 12) # VirtualAddress
    [BitConverter]::GetBytes([uint32]0x400).CopyTo($image, $section + 16) # SizeOfRawData
    [BitConverter]::GetBytes([uint32]0x200).CopyTo($image, $section + 20) # PointerToRawData
    $entry = 0x200
    $codeView = 0x240
    # RSDS, GUID and age (20 bytes), the path and its terminating NUL.
    $bytes = New-Object System.Collections.Generic.List[byte]
    $bytes.AddRange([System.Text.Encoding]::ASCII.GetBytes('RSDS'))
    $bytes.AddRange([byte[]]::new(20))
    $bytes.AddRange([System.Text.Encoding]::UTF8.GetBytes($PdbPath))
    $bytes.Add([byte]0)
    $record = $bytes.ToArray()
    [BitConverter]::GetBytes([uint32]2).CopyTo($image, $entry + 12)        # IMAGE_DEBUG_TYPE_CODEVIEW
    [BitConverter]::GetBytes([uint32]$record.Length).CopyTo($image, $entry + 16)
    [BitConverter]::GetBytes([uint32]($codeView - 0x200 + 0x1000)).CopyTo($image, $entry + 20)
    [BitConverter]::GetBytes([uint32]$codeView).CopyTo($image, $entry + 24)
    $record.CopyTo($image, $codeView)
    $data = [System.Text.Encoding]::ASCII.GetBytes($DataText)
    $data.CopyTo($image, 0x400)
    return $image
}
$folder = Join-Path ([System.IO.Path]::GetTempPath()) ('lucent-path-scan-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path (Join-Path $folder 'plugins') | Out-Null
try {
    [System.IO.File]::WriteAllBytes((Join-Path $folder 'leaky.dll'), (New-PeImage 'C:\vb\zlib\x64-windows-lucent-rel\z.pdb' 'D:\src\Lucent\src\a.cpp'))
    [System.IO.File]::WriteAllBytes((Join-Path $folder 'plugins\clean.dll'), (New-PeImage 'clean.pdb' 'src\a.cpp'))
    [System.IO.File]::WriteAllText((Join-Path $folder 'notes.txt'), 'nothing to see')
    $report = Invoke-BuilderPathAudit -Scanner $scanner -Root $folder -Label 'Synthetic folder'
    Assert-That 'Folder audit: every file scanned' ($report.Files -eq 3) "files: $($report.Files)"
    Assert-That 'Folder audit: two hits, both in leaky.dll' (@($report.Hits).Count -eq 2 -and @($report.HitFiles).Count -eq 1 -and $report.HitFiles[0] -eq 'leaky.dll') (@($report.Errors) -join '; ')
    $pdbHit = @($report.Hits | Where-Object { $_.Location -like 'PE debug directory (CodeView*' })
    Assert-That 'PE debug directory: the PDB path is reported as such, at the drive letter' ($pdbHit.Count -eq 1 -and $pdbHit[0].Offset -eq (0x240 + 24) -and $pdbHit[0].Rule -eq 'vcpkg buildtrees folder') `
        $(if ($pdbHit.Count) { "offset $($pdbHit[0].Offset), $($pdbHit[0].Rule)" } else { 'no hit' })
    $dataHit = @($report.Hits | Where-Object { $_.Location -eq 'file data' })
    Assert-That 'File data: a __FILE__-like string, at its offset' ($dataHit.Count -eq 1 -and $dataHit[0].Offset -eq 0x400 -and $dataHit[0].Rule -eq 'source tree') `
        $(if ($dataHit.Count) { "offset $($dataHit[0].Offset), $($dataHit[0].Rule)" } else { 'no hit' })
    Assert-That 'Errors name the file, the offset and the folder' (@($report.Errors | Where-Object { $_ -match '^leaky\.dll at offset 0x00000258 ' -and $_ -match 'C:\\vb' }).Count -eq 1) (@($report.Errors) -join '; ')
}
finally {
    Remove-Item -LiteralPath $folder -Recurse -Force
}

$text = Invoke-BuilderPathAudit -Scanner $scanner -Text '{ "innoSetup": "%USERPROFILE%\\Tools\\ISCC.exe", "gitCommit": "abc" }' -Name 'manifest'
Assert-That 'Text: %USERPROFILE% stands for the profile without naming it' (@($text.Hits).Count -eq 0) (@($text.Errors) -join '; ')

# --- Compressed content (W12 review SEC-07) ----------------------------------------------------------
# Paths hidden by compression, made by encoders independent of the scan's decoders: .NET's
# DeflateStream and GZipStream (with an Adler-32 computed here), and for Inno Setup's LZMA a layout
# as ISCC 6.7.3 writes it, built with Python's lzma module (liblzma) by inno_setup_vector.py.

function ConvertTo-ZlibBytes([byte[]]$Data) {
    $memory = New-Object System.IO.MemoryStream
    $memory.WriteByte(0x78); $memory.WriteByte(0x9C)
    $deflate = New-Object System.IO.Compression.DeflateStream($memory, [System.IO.Compression.CompressionMode]::Compress, $true)
    $deflate.Write($Data, 0, $Data.Length)
    $deflate.Dispose()
    [int64]$a = 1; [int64]$b = 0
    foreach ($value in $Data) { $a = ($a + $value) % 65521; $b = ($b + $a) % 65521 }
    $adler = [uint32]($b * 65536 + $a)
    foreach ($shift in 24, 16, 8, 0) { $memory.WriteByte([byte](($adler -shr $shift) -band 0xFF)) }
    return $memory.ToArray()
}
function ConvertTo-GzipBytes([byte[]]$Data) {
    $memory = New-Object System.IO.MemoryStream
    $gzip = New-Object System.IO.Compression.GZipStream($memory, [System.IO.Compression.CompressionMode]::Compress, $true)
    $gzip.Write($Data, 0, $Data.Length)
    $gzip.Dispose()
    return $memory.ToArray()
}
# Qt's rcc stores a compressed resource as qCompress does: the size, big-endian, then a zlib stream.
function ConvertTo-QCompressBytes([byte[]]$Data) {
    $zlib = ConvertTo-ZlibBytes $Data
    $size = [byte[]]@((($Data.Length -shr 24) -band 0xFF), (($Data.Length -shr 16) -band 0xFF), (($Data.Length -shr 8) -band 0xFF), ($Data.Length -band 0xFF))
    return [byte[]]($size + $zlib)
}
function Join-Bytes([object[]]$Pieces) {
    $memory = New-Object System.IO.MemoryStream
    foreach ($piece in $Pieces) { $bytes = [byte[]]$piece; $memory.Write($bytes, 0, $bytes.Length) }
    return $memory.ToArray()
}
$latin1 = [System.Text.Encoding]::GetEncoding(28591)
$filler = [System.Text.Encoding]::ASCII.GetBytes(('qml resource text ' * 64))
$qmlWithPath = [System.Text.Encoding]::UTF8.GetBytes("import QtQuick`n// generated from C:\vb\qtdeclarative\src\quick\Window.qml`nItem {}`n" + ('padding ' * 200))
$mimeWithPath = [System.Text.Encoding]::Unicode.GetBytes('<mime-info>' + ('x' * 300) + 'D:\src\Lucent\build\release\vcpkg_installed\x64-windows-lucent\share\mime.xml</mime-info>')
$innerWithPath = [System.Text.Encoding]::ASCII.GetBytes(('inner ' * 50) + 'D:\vcpkg\packages\qtbase_x64\debug.txt' + (' inner' * 50))

$folder = Join-Path ([System.IO.Path]::GetTempPath()) ('lucent-path-scan-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $folder | Out-Null
try {
    $resource = Join-Bytes @($filler, (ConvertTo-QCompressBytes $qmlWithPath), $filler)
    $mime = Join-Bytes @($filler, (ConvertTo-GzipBytes $mimeWithPath), $filler)
    $nested = Join-Bytes @($filler, (ConvertTo-ZlibBytes (Join-Bytes @($filler, (ConvertTo-ZlibBytes $innerWithPath), $filler))), $filler)
    $clean = Join-Bytes @($filler, (ConvertTo-QCompressBytes ([System.Text.Encoding]::ASCII.GetBytes('Item { width: 10 }' * 40))), $filler)
    [System.IO.File]::WriteAllBytes((Join-Path $folder 'Qt6Quick.dll'), $resource)
    [System.IO.File]::WriteAllBytes((Join-Path $folder 'Qt6Core.dll'), $mime)
    [System.IO.File]::WriteAllBytes((Join-Path $folder 'nested.bin'), $nested)
    [System.IO.File]::WriteAllBytes((Join-Path $folder 'clean.dll'), $clean)
    $hidden = @('C:\vb\qtdeclarative', 'D:\src\Lucent\build\release\vcpkg_installed', 'D:\vcpkg\packages') | Where-Object {
        $needle = $_; @($resource, $mime, $nested) | Where-Object { $latin1.GetString($_).Contains($needle) -or [System.Text.Encoding]::Unicode.GetString($_).Contains($needle) } }
    Assert-That 'Compressed: the planted paths are not in the files'' raw bytes' (@($hidden).Count -eq 0) (@($hidden) -join ', ')
    $report = Invoke-BuilderPathAudit -Scanner $scanner -Root $folder -Label 'Compressed'
    $rcc = @($report.Hits | Where-Object { $_.File -match '^Qt6Quick\.dll \[zlib at 0x[0-9A-F]{8}\]$' })
    Assert-That 'Compressed: a path in a zlib-compressed rcc resource (qCompress) is found, named by the stream' `
        ($rcc.Count -eq 1 -and $rcc[0].Rule -eq 'vcpkg buildtrees folder' -and $rcc[0].Location -eq 'decompressed data' -and $rcc[0].File -eq ('Qt6Quick.dll [zlib at 0x{0:X8}]' -f ($filler.Length + 4))) `
        (@($report.Errors) -join '; ')
    $gz = @($report.Hits | Where-Object { $_.File -match '^Qt6Core\.dll \[gzip at ' })
    Assert-That 'Compressed: a UTF-16 path in a gzip member (as Qt''s MIME database) is found' ($gz.Count -eq 1 -and $gz[0].Encoding -eq 'UTF-16LE' -and $gz[0].Rule -eq 'vcpkg installed tree') `
        (@($report.Errors) -join '; ')
    $deep = @($report.Hits | Where-Object { $_.File -match '^nested\.bin \[zlib at 0x[0-9A-F]{8}\] \[zlib at 0x[0-9A-F]{8}\]$' })
    Assert-That 'Compressed: a path in a zlib stream inside a zlib stream is found' ($deep.Count -eq 1 -and $deep[0].Rule -eq 'vcpkg packages folder') (@($report.Errors) -join '; ')
    Assert-That 'Compressed: four hit-free and hit files, three hits, nothing in clean.dll' (@($report.Hits).Count -eq 3 -and @($report.HitFiles | Where-Object { $_ -like 'clean.dll*' }).Count -eq 0) `
        "hits: $(@($report.Hits).Count)"
    Assert-That 'Compressed: every stream counted (zlib 4: rcc, outer, inner, clean; gzip 1)' ($report.Compressed.Kinds['zlib'] -eq 4 -and $report.Compressed.Kinds['gzip'] -eq 1) `
        (Format-CompressedContentTally $report.Compressed)

    # A zstd frame cannot be decompressed by the scan: the audit fails instead of skipping it.
    $zstd = Join-Bytes @($filler, [byte[]](0x28, 0xB5, 0x2F, 0xFD, 0x20, 0x40, 0x41, 0x00, 0x00), $filler)
    $zstdReport = Invoke-BuilderPathAudit -Scanner $scanner -Blobs @([pscustomobject]@{ Name = 'zstd.bin'; Bytes = $zstd }) -Label 'zstd'
    Assert-That 'Compressed: a zstd frame is an error (it cannot be decompressed for the scan)' (@($zstdReport.Errors | Where-Object { $_ -match '^zstd\.bin at offset 0x[0-9A-F]{8}: zstd-compressed' }).Count -eq 1) `
        (@($zstdReport.Errors) -join '; ')

    # Inno Setup: the setup data, the setup engine and the installed files are LZMA-compressed. The
    # synthetic executable (installer\tests\inno_setup_vector.py's output) holds a UTF-16 path in its
    # setup data and an ASCII path in its installed files; its setup engine is call-transformed and
    # must match the CRC-32 of the loader's offset table once restored.
    $inno = [Convert]::FromBase64String(
    'TVoAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAHJEbFB0U83m13sL' +
    'KgIAAADIAgAAAAAAAIECAAAAAAAA9AEAAL79tKFlAQAAAAAAAOAAAAAAAAAAAAAAALlH8Z9sb2FkZXIgY29kZSBsb2FkZXIgY29k' +
    'ZSBsb2FkZXIgY29kZSBsb2FkZXIgY29kZSBsb2FkZXIgY29kZSBsb2FkZXIgY29kZSBsb2FkZXIgY29kZSBsb2FkZXIgY29kZSB6' +
    'bGIaEOAH4wB4XQAmloAFYYlIbZxu6IwgsYQ0wRpCvojAHjfdV6Y13G0I98PQ8tSg2+8VztMYc9TpwKFrp8QSeSEJQOi/yZEoMmH5' +
    'mrEy19KwM4ipaQNLtvYfKYPmgzYQEgngbTDyPY08CwWvE/z2uAiF9KwOynIeAhSBGJjki3ZnUgAASW5ubyBTZXR1cCBTZXR1cCBE' +
    'YXRhICg2LjcuMCkAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAALKJktwAAAAAAAAAAAAAAAAAAAAAAGBbAwAAAAAA' +
    'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAoqI6K3UAAAAAAAAAAYZNUC5dAAABAAAmAAsVW2v2tk1XqTqb1cnq8BGTA5AK0X43RppD' +
    'UnmeF8f3a07pg7SZBquDXOTwfKeNo/0SsZ6Li11Nmfqf18Hp4+YvFkm82MiMoAAx97PPrFvrtwQs6k9iYpXHaBnvvoCAHmA2Unf/' +
    '/rnAAFHEFoYYAAAAAAAAAAG9TOaLXQAAAQAAAIA8rukSEfv///tYQAB0bhEqOgAAAAAAAAABXDS7Al0AAAEAACaWgAVR+FlyRSG3' +
    'k7xsCi8AAvB6fHWcexsEbkcSBhoDqcCKNOO6F5Ce5f///PhcYA==')
    $setup = Join-Path $folder 'Lucent-Setup-0.0.0-x64.exe'
    [System.IO.File]::WriteAllBytes($setup, $inno)
    $plain = Invoke-BuilderPathAudit -Scanner $scanner -Path $setup -Label 'Setup without Inno Setup decoding'
    Assert-That 'Inno Setup: without decoding its LZMA parts the paths are invisible' (@($plain.Hits).Count -eq 0) (@($plain.Errors) -join '; ')
    $decoded = Invoke-BuilderPathAudit -Scanner $scanner -Path $setup -Label 'Setup' -InnoSetup
    $setupData = @($decoded.Hits | Where-Object { $_.File -match '^Lucent-Setup-0\.0\.0-x64\.exe \[Inno Setup setup data, block 0 at 0x[0-9A-F]{8}\]$' })
    Assert-That 'Inno Setup: a UTF-16 path in the LZMA-compressed setup data (the compiled script) is found' `
        ($setupData.Count -eq 1 -and $setupData[0].Encoding -eq 'UTF-16LE' -and $setupData[0].Rule -eq 'user profile') (@($decoded.Errors) -join '; ')
    $installed = @($decoded.Hits | Where-Object { $_.File -match '^Lucent-Setup-0\.0\.0-x64\.exe \[Inno Setup installed files, chunk 0 at 0x[0-9A-F]{8}\]$' })
    Assert-That 'Inno Setup: a path in the LZMA2-compressed installed files is found' ($installed.Count -eq 1 -and $installed[0].Rule -eq 'vcpkg installed tree') `
        (@($decoded.Errors) -join '; ')
    Assert-That 'Inno Setup: the setup engine was restored (its size and CRC-32 checked) and scanned' `
        ($decoded.Compressed.Kinds['Inno Setup setup engine'] -eq 1 -and $decoded.Compressed.Kinds['Inno Setup setup data'] -eq 2 -and
         $decoded.Compressed.Kinds['Inno Setup installed files'] -eq 1 -and @($decoded.CompressedProblems).Count -eq 0) (Format-CompressedContentTally $decoded.Compressed)
    # A damaged offset table: Setup's compressed content cannot be read, which fails the audit.
    $damaged = [byte[]]$inno.Clone()
    $damaged[0x40 + 60] = $damaged[0x40 + 60] -bxor 0xFF
    [System.IO.File]::WriteAllBytes($setup, $damaged)
    $refused = Invoke-BuilderPathAudit -Scanner $scanner -Path $setup -Label 'Damaged Setup' -InnoSetup
    Assert-That 'Inno Setup: a layout the reader cannot decode is an error' (@($refused.Errors | Where-Object { $_ -match 'Inno Setup''s compressed content cannot be decompressed' }).Count -eq 1) `
        (@($refused.Errors) -join '; ')
}
finally {
    Remove-Item -LiteralPath $folder -Recurse -Force
}

# With -InnoSetupCompiler: a real Setup compiled by ISCC with planted paths in its compiled script
# (UTF-16 in the LZMA setup data) and in an installed file (LZMA2): both are found.
if ($InnoSetupCompiler) {
    $probe = Join-Path ([System.IO.Path]::GetTempPath()) ('lucent-inno-probe-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path (Join-Path $probe 'files') | Out-Null
    try {
        [System.IO.File]::WriteAllText((Join-Path $probe 'files\readme.txt'), "built in D:\src\Lucent\src\planted.cpp`r`n" + ('installed file text ' * 100))
        $script = @(
            '[Setup]', 'AppId={{0F3A6C2E-7D1B-4C5A-9E8F-1A2B3C4D5E6F}', 'AppName=Lucent scan probe', 'AppVersion=0.0.1',
            'PrivilegesRequired=lowest', 'DefaultDirName={autopf}\LucentScanProbe', 'OutputDir=out', 'OutputBaseFilename=probe',
            'Compression=lzma2/max', 'SolidCompression=yes', 'ArchitecturesAllowed=x64os', 'ArchitecturesInstallIn64BitMode=x64os',
            '[Files]', 'Source: "files\*"; DestDir: "{app}"',
            '[Code]', 'function InitializeSetup: Boolean;', 'begin', "  Log('C:\Users\builder\probe\from-the-script.txt');", '  Result := True;', 'end;')
        [System.IO.File]::WriteAllText((Join-Path $probe 'probe.iss'), ($script -join "`r`n") + "`r`n")
        $compile = & $InnoSetupCompiler /Q (Join-Path $probe 'probe.iss') 2>&1
        $exe = Join-Path $probe 'out\probe.exe'
        Assert-That "ISCC compiled the probe Setup ($InnoSetupCompiler)" (Test-Path -LiteralPath $exe) (@($compile) -join ' ')
        if (Test-Path -LiteralPath $exe) {
            $real = Invoke-BuilderPathAudit -Scanner $scanner -Path $exe -Name 'probe.exe' -Label 'ISCC probe' -InnoSetup
            $inScript = @($real.Hits | Where-Object { $_.File -like 'probe.exe `[Inno Setup setup data, block 0 at *' -and $_.Rule -eq 'user profile' })
            $inFiles = @($real.Hits | Where-Object { $_.File -like 'probe.exe `[Inno Setup installed files, chunk 0 at *' -and $_.Rule -eq 'source tree' })
            Assert-That 'A real ISCC Setup: the path in the compiled script and the path in the installed file are found' `
                ($inScript.Count -eq 1 -and $inFiles.Count -eq 1 -and @($real.CompressedProblems).Count -eq 0) ("{0}; {1}" -f (Format-CompressedContentTally $real.Compressed), (@($real.Errors) -join '; '))
        }
    }
    finally {
        Remove-Item -LiteralPath $probe -Recurse -Force -ErrorAction SilentlyContinue
    }
}
else {
    Write-Host '[SKIP] A real ISCC Setup (pass -InnoSetupCompiler <ISCC.exe> to compile one with planted paths)'
}

Write-Host ("{0} of {1} checks passed." -f ($checks - $failures), $checks)
if ($failures -gt 0) { exit 1 }
exit 0
