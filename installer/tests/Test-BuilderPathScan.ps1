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
    Exit code 0 when every check passes.

.EXAMPLE
    powershell -NoProfile -File installer\tests\Test-BuilderPathScan.ps1

.NOTES
    Windows PowerShell 5.1 compatible. Keep this file ASCII.
#>
[CmdletBinding()]
param()

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

Write-Host ("{0} of {1} checks passed." -f ($checks - $failures), $checks)
if ($failures -gt 0) { exit 1 }
exit 0
