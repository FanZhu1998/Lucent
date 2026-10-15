<#
.SYNOPSIS
    Generates Lucent's logo derivatives in assets\branding from the master, or checks them (ADR 0025).

.DESCRIPTION
    The branding asset is one master, master\lucent-logo.png (the project owner's image, kept byte for
    byte with its provenance record), and the derivatives made from it: the Windows icon of Lucent.exe,
    lucent-worker.exe and Setup, the PNGs of the window icon and the About dialog, Setup's wizard images
    and the documentation logo. branding.json ("lucent.branding" v1) names each derivative with its role,
    pixel size and how it was made; manifest.json ("lucent.asset-manifest" v1) lists every file with its
    size and SHA-256. Derivatives are never edited by hand.

    Without -Check, the master is validated and analysed (noise floor, content box, interior alpha),
    every derivative is rendered (area averages of premultiplied linear light, the content box centred,
    per-size padding) and written without metadata, then branding.json and manifest.json. The master's
    provenance record, provenance.json (written by hand), must describe the master. Files whose bytes do
    not change are left alone (a missing or damaged derivative is written again, and the version stays
    when nothing else changed); files of the previous manifest that the recipe no longer makes are
    deleted, inside the folder only (a manifest that names another path is refused). When anything
    changes, -Version must name the new branding version (Semantic Versioning: MAJOR when a role or file
    is removed or renamed, MINOR for a new master or new derivatives, PATCH for the same master rendered
    differently, or for a change of provenance.json or of the generated record's wording only).

    With -Check, nothing is written: the folder must hold exactly the files manifest.json lists (and
    README.md; hidden files count), each with its size and SHA-256, provenance.json must describe the
    master, and a fresh rendering of the committed master must reproduce every derivative, branding.json
    and manifest.json byte for byte. Exit code 1 when anything is stale, missing, edited by hand, or its
    manifest entry is wrong, with what to do for each kind of problem (CTest lucent_branding_tests runs
    it, with more).

    Replacing the logo: record the new master's provenance in provenance.json, pass the master with
    -MasterFile and a new -Version, review the contact sheet (-ContactSheet), rebuild and test; no
    consumer changes (assets\branding\README.md, "Replacing the logo").

.PARAMETER Root
    The branding asset folder (default: assets\branding of this repository).
.PARAMETER Check
    Verify instead of writing.
.PARAMETER Version
    The new branding version, required when the derivatives change.
.PARAMETER MasterFile
    A new master PNG to import: validated, then copied byte for byte to master\lucent-logo.png.
.PARAMETER ContactSheet
    Also write a contact sheet PNG of the master and every derivative (light, grey and dark backgrounds,
    1:1 and magnified) to this path, for review.

.EXAMPLE
    powershell -NoProfile -File tools\branding\Update-Branding.ps1 -Check
.EXAMPLE
    powershell -NoProfile -File tools\branding\Update-Branding.ps1 -MasterFile C:\Temp\new-logo.png -Version 2.0.0 -ContactSheet build\branding-sheet.png

.NOTES
    Windows PowerShell 5.1 compatible. Keep this file ASCII.
#>
[CmdletBinding()]
param(
    [string]$Root,
    [switch]$Check,
    [string]$Version,
    [string]$MasterFile,
    [string]$ContactSheet
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Import-Module (Join-Path $PSScriptRoot 'LucentBranding.psm1') -Force
if (-not $Root) { $Root = Join-Path $repoRoot 'assets\branding' }

try {
    $result = Invoke-BrandingGenerator -Root $Root -Check:$Check -Version $Version -MasterFile $MasterFile
}
catch {
    Write-Host "ERROR: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}
if ($result.Master) {
    $analysis = $result.Master.Analysis
    Write-Host ("Master: {0}x{1} px ({2}), artwork {3}x{4} px at ({5}, {6}), interior alpha {7} made opaque; cleared as noise: {8} pixels of alpha 1 to {9}, {10} faint groups of {11} pixels" -f
        $analysis.Width, $analysis.Height, $result.Master.Png.ChunkList(), $analysis.ContentWidth, $analysis.ContentHeight,
        $analysis.ContentLeft, $analysis.ContentTop, $analysis.SolidAlpha, $analysis.ClearedPixels, $analysis.NoiseAlpha,
        $analysis.FaintComponents, $analysis.FaintPixels)
}
if ($Check) {
    if ($result.Errors.Count -gt 0) {
        foreach ($problem in $result.Errors) { Write-Host "ERROR: $problem" -ForegroundColor Red }
        Write-Host "The branding asset in $($result.Root) is not what the generator makes ($($result.Errors.Count) problem(s)). What to do:" -ForegroundColor Red
        foreach ($advice in (Get-BrandingRemedy -Problems @($result.Errors))) { Write-Host "  - $advice" -ForegroundColor Red }
        exit 1
    }
    Write-Host "Branding $($result.Version): every derivative is current and every manifest entry is right ($($result.Root))."
}
else {
    if ($result.Unchanged -and @($result.Written).Count -eq 0) { Write-Host "Branding $($result.Version): nothing changed." }
    elseif ($result.Unchanged) {
        # A missing or damaged derivative written again: the version stays (BRD-05).
        Write-Host "Branding $($result.Version): the version is unchanged; restored in $($result.Root):"
        foreach ($path in $result.Written) { Write-Host "    restored $path" }
    }
    else {
        Write-Host "Branding $($result.Version) written to $($result.Root):"
        foreach ($path in $result.Written) { Write-Host "    wrote   $path" }
        foreach ($path in $result.Removed) { Write-Host "    removed $path" }
    }
}
if ($ContactSheet) {
    $sheetPath = [System.IO.Path]::GetFullPath($ContactSheet)
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $sheetPath) | Out-Null
    New-BrandingContactSheet -Root $Root -Path $sheetPath
    Write-Host "Contact sheet: $sheetPath"
}
exit 0
