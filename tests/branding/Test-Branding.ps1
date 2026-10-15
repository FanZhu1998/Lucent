<#
.SYNOPSIS
    Checks Lucent's branding asset and its generator (ADR 0025): the committed derivatives are what the
    generator makes from the master, the icon and the wizard images have the forms Windows and Inno
    Setup take, and the generator refuses what it must refuse.

.DESCRIPTION
    1. The committed asset (assets\branding, or -Root): Invoke-BrandingGenerator -Check passes (every
       file has the size and SHA-256 manifest.json lists, nothing unlisted is in the folder,
       provenance.json describes the master, and a fresh rendering of the master reproduces every
       derivative, branding.json and manifest.json byte for byte: Update-Branding.ps1 -Check).
    2. The Windows icon (role windows-icon): exactly the sizes 16, 20, 24, 32, 40, 48, 64, 96, 128 and
       256, square, one plane of 32 bits without a palette, 32-bit BMPs (BITMAPINFOHEADER, an AND mask
       that agrees with the alpha) below 256 px and a PNG at 256, transparent corners and an opaque
       middle, every image read by Windows (CreateIconFromResourceEx; Test-IconFile), and every image
       the app-icon PNG of its size, pixel for pixel.
    3. The PNG derivatives: IHDR, IDAT and IEND only (8-bit RGBA, no metadata, no content credentials,
       no time), the size branding.json records, transparent corners. The app icons cover every size
       the window's small and large icons (16 and 32 logical pixels) and the About dialog's logo (64)
       need at the display scales Windows offers up to 300 %. There is a wizard image for every image
       area Inno Setup 6.7.3 documents for 100 to 250 % (WizardImageFile 202x386 to 534x1022, in its
       aspect ratio of 164:314; WizardSmallImageFile 58 to 159 px, square).
    4. Generating from scratch, in a folder that holds only the master and its provenance record, gives
       the committed files byte for byte (the generator is deterministic); generating again changes
       nothing. The review evidence draws: the contact sheet of the generated asset is written
       (Update-Branding.ps1 -ContactSheet).
    5. Tampering with a copy of the asset is found: a changed byte (SHA-256), a derivative re-encoded
       with other pixels and its manifest entry updated (the pixels differ), a missing derivative, a
       file added by hand, a provenance record of another master, and a manifest or branding content of
       schema version 2 (refused, never guessed).
    6. The rendering rules on synthetic masters: an off-centre artwork comes out centred; faint specks
       and a faint ring outside the edge are cleared (the rendering equals that of the clean master); a
       nearly opaque interior becomes opaque; a white artwork's anti-aliased edge stays white (no dark
       fringe: premultiplied averaging); a black and white edge averages in linear light (sRGB 188, not
       128). A translucent artwork, an artwork touching the edge, a colour profile (iCCP), a master that
       is not 8-bit RGBA, one too small and one whose artwork is not roughly square are refused. A new
       master needs its provenance record first and a higher branding version.

    Registered with CTest as lucent_branding_tests (label "branding"). Exit code 0 when every check
    passed. Windows PowerShell 5.1 compatible. Keep this file ASCII.

.PARAMETER Root
    The branding asset folder to check (default: assets\branding of this repository).

.EXAMPLE
    powershell -NoProfile -File tests\branding\Test-Branding.ps1
#>
[CmdletBinding()]
param([string]$Root)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Import-Module (Join-Path $repoRoot 'tools\branding\LucentBranding.psm1') -Force
if (-not $Root) { $Root = Join-Path $repoRoot 'assets\branding' }
$Root = [System.IO.Path]::GetFullPath($Root)
Initialize-BrandingType

$script:checks = 0
$script:failures = 0
function Assert-True([bool]$Condition, [string]$What) {
    $script:checks++
    if (-not $Condition) { $script:failures++; Write-Host "FAILED: $What" }
}
function Assert-Problem([object[]]$Errors, [string]$Pattern, [string]$What) {
    $script:checks++
    if (-not (@($Errors) | Where-Object { $_ -match $Pattern })) {
        $script:failures++
        Write-Host "FAILED: $What (no error matching '$Pattern'; errors: $(@($Errors) -join ' | '))"
    }
}
function Assert-Refused([scriptblock]$Block, [string]$Pattern, [string]$What) {
    # The block must throw, with a message (or an inner exception's message) matching Pattern.
    $script:checks++
    try { & $Block | Out-Null }
    catch {
        $messages = @()
        $exception = $_.Exception
        while ($exception) { $messages += $exception.Message; $exception = $exception.InnerException }
        if (-not ($messages | Where-Object { $_ -match $Pattern })) {
            $script:failures++
            Write-Host "FAILED: $What (refused, but not with '$Pattern': $($messages -join ' | '))"
        }
        return
    }
    $script:failures++
    Write-Host "FAILED (not refused): $What"
}
function Read-PngImage([string]$Path) { return ([LucentBranding.Png]::Read([System.IO.File]::ReadAllBytes($Path))).Image }

# Synthetic masters: RGBA pixels drawn with 4 x 4 supersampling, made fast in C# (an Add-Type assembly of
# its own, so it hands back plain byte arrays).
if (-not ('LucentBrandingTest.Synthetic' -as [type])) {
    Add-Type -Language CSharp -TypeDefinition @'
namespace LucentBrandingTest {
    public static class Synthetic {
        // A size x size master, transparent but for a rectangle [left, left + width) x [top, top + height)
        // with corners of the given radius (0: square corners), its interior alpha `alpha`, coloured
        // `leftColour` left of x = split and `rightColour` from it (RGB packed as 0xRRGGBB).
        public static byte[] Plate(int size, double left, double top, double width, double height, double radius,
                                   int alpha, int leftColour, int rightColour, double split) {
            byte[] pixels = new byte[size * size * 4];
            for (int y = 0; y < size; y++) {
                for (int x = 0; x < size; x++) {
                    int inside = 0;
                    for (int sy = 0; sy < 4; sy++) {
                        for (int sx = 0; sx < 4; sx++) {
                            double px = x + (sx + 0.5) / 4.0, py = y + (sy + 0.5) / 4.0;
                            if (px < left || px >= left + width || py < top || py >= top + height) { continue; }
                            double cx = px < left + radius ? left + radius : (px > left + width - radius ? left + width - radius : px);
                            double cy = py < top + radius ? top + radius : (py > top + height - radius ? top + height - radius : py);
                            double dx = px - cx, dy = py - cy;
                            if (dx * dx + dy * dy <= radius * radius) { inside++; }
                        }
                    }
                    if (inside == 0) { continue; }
                    int colour = x < split ? leftColour : rightColour;
                    int i = (y * size + x) * 4;
                    pixels[i] = (byte)(colour >> 16);
                    pixels[i + 1] = (byte)(colour >> 8);
                    pixels[i + 2] = (byte)colour;
                    pixels[i + 3] = (byte)((alpha * inside + 8) / 16);
                }
            }
            return pixels;
        }

        // Sets pixel (x, y) to the given colour and alpha.
        public static void Set(byte[] pixels, int size, int x, int y, int colour, int alpha) {
            int i = (y * size + x) * 4;
            pixels[i] = (byte)(colour >> 16);
            pixels[i + 1] = (byte)(colour >> 8);
            pixels[i + 2] = (byte)colour;
            pixels[i + 3] = (byte)alpha;
        }
    }
}
'@
}
function New-SyntheticMaster([int]$Size = 600, [double]$Left = 90, [double]$Top = 130, [double]$Width = 380, [double]$Height = 380,
                             [double]$Radius = 60, [int]$Alpha = 253, [int]$LeftColour = 0xFFFFFF, [int]$RightColour = 0xFFFFFF,
                             [double]$Split = 0, [scriptblock]$Edit) {
    $pixels = [LucentBrandingTest.Synthetic]::Plate($Size, $Left, $Top, $Width, $Height, $Radius, $Alpha, $LeftColour, $RightColour, $Split)
    if ($Edit) { & $Edit $pixels }
    return [LucentBranding.Png]::Write((New-Object LucentBranding.RgbaImage $Size, $Size, $pixels))
}
function Write-Provenance([string]$Folder, [byte[]]$MasterBytes) {
    # A provenance record for a synthetic master (the form Test-BrandingProvenance requires).
    $record = [ordered]@{
        schema = 'lucent.branding-provenance'; schema_version = 1
        master = [ordered]@{ path = 'master/lucent-logo.png'; size = [long]$MasterBytes.Length; sha256 = (Get-BytesSha256 $MasterBytes) }
        description = 'A synthetic test master.'; supplied_by = 'tests\branding\Test-Branding.ps1'; supplied = '2026-10-14'
        origin = [ordered]@{ tool = 'Test-Branding.ps1' }; modifications = 'None.'; licence = $null; licence_status = 'Test data.' }
    [System.IO.File]::WriteAllText((Join-Path $Folder 'provenance.json'), (ConvertTo-CanonicalJson $record), (New-Object System.Text.UTF8Encoding $false))
}

# --- 1. The committed asset ----------------------------------------------------------------------------

$committed = Invoke-BrandingGenerator -Root $Root -Check
Assert-True ($committed.Errors.Count -eq 0) "the committed asset is what the generator makes from its master ($($committed.Errors -join ' | '))"
$asset = Read-BrandingAsset -Root $Root
Write-Host "Branding $($asset.Version): $(@($asset.Derivatives).Count) derivatives of $($asset.Master.Path) (SHA-256 $($asset.Master.Sha256))"

# --- 2. The Windows icon -------------------------------------------------------------------------------

$icons = @(Get-BrandingFiles -Asset $asset -Role 'windows-icon')
Assert-True ($icons.Count -eq 1) "one windows-icon derivative ($($icons.Count))"
# The sizes the asset's own branding.json records for the icon (a size added to the recipe needs no
# change here; W12 review BRD-02), which must include those Windows and Inno Setup recommend.
function Get-IconSizes($Asset) {
    $icon = @($Asset.Derivatives | Where-Object { $_.Role -ceq 'windows-icon' })[0]
    return @(@($icon.Images) | ForEach-Object { [int]$_.width })
}
$iconSizes = @(Get-IconSizes $asset)
foreach ($required in 16, 32, 48, 256) { Assert-True ($iconSizes -contains $required) "the Windows icon has a $required px image ($($iconSizes -join ', '))" }
$problems = @(Test-IconFile -Path $icons[0] -ExpectedSizes $iconSizes)
Assert-True ($problems.Count -eq 0) "the Windows icon's structure ($($problems -join ' | '))"
$appIcons = @{}
foreach ($derivative in @($asset.Derivatives | Where-Object { $_.Role -ceq 'app-icon' })) { $appIcons[[int]$derivative.Width] = $derivative.FullPath }
foreach ($image in [LucentBranding.Ico]::Read([System.IO.File]::ReadAllBytes($icons[0]))) {
    if (-not $appIcons.ContainsKey($image.Width)) { Assert-True $false "an app-icon PNG of $($image.Width) px, as in the Windows icon"; continue }
    $largest = 0
    $count = $image.Image.CountDifferences((Read-PngImage $appIcons[$image.Width]), [ref]$largest)
    Assert-True ($count -eq 0) "the Windows icon's $($image.Width) px image has the pixels of the app icon of that size ($count differ)"
}

# --- 3. The PNG derivatives ----------------------------------------------------------------------------

foreach ($derivative in @($asset.Derivatives | Where-Object { $_.Format -eq 'png' })) {
    $file = [LucentBranding.Png]::Read([System.IO.File]::ReadAllBytes($derivative.FullPath))
    Assert-True ($file.ChunkList() -ceq 'IHDR, IDAT, IEND') "$($derivative.Path) has the chunks IHDR, IDAT and IEND only ($($file.ChunkList()))"
    Assert-True ($file.Image.Width -eq $derivative.Width -and $file.Image.Height -eq $derivative.Height) "$($derivative.Path) is $($derivative.Width)x$($derivative.Height) px, as branding.json says ($($file.Image.Width)x$($file.Image.Height))"
    $w = $file.Image.Width - 1; $h = $file.Image.Height - 1
    Assert-True (($file.Image.Alpha(0, 0) + $file.Image.Alpha($w, 0) + $file.Image.Alpha(0, $h) + $file.Image.Alpha($w, $h)) -eq 0) "$($derivative.Path) has transparent corners"
}
$appSizes = @($appIcons.Keys)
foreach ($logical in @(16, 32, 64)) {
    foreach ($percent in @(100, 125, 150, 175, 200, 225, 250, 300)) {
        $pixels = [int]($logical * $percent / 100)
        Assert-True ($appSizes -contains $pixels) "an app icon of $pixels px ($logical logical px at $percent %)"
    }
}
foreach ($derivative in @($asset.Derivatives | Where-Object { $_.Role -ceq 'app-icon' })) {
    Assert-True ($derivative.Width -eq $derivative.Height) "$($derivative.Path) is square"
}
# Setup's wizard images: one for every image area Inno Setup 6.7.3 documents for 100 to 250 % (more may
# be added), the large ones in its aspect ratio of 164:314 (to a pixel), the small ones square.
$wizard = @($asset.Derivatives | Where-Object { $_.Role -ceq 'setup-wizard-image' })
$wizardSizes = @($wizard | ForEach-Object { "$($_.Width)x$($_.Height)" })
foreach ($area in @('202x386', '269x515', '336x643', '403x772', '430x824', '498x953', '534x1022')) {
    Assert-True ($wizardSizes -contains $area) "a wizard image for Inno Setup's $area image area ($($wizardSizes -join ', '))"
}
foreach ($derivative in $wizard) {
    Assert-True ([Math]::Abs($derivative.Height * 164 / 314 - $derivative.Width) -le 1) "$($derivative.Path) has the wizard image's aspect ratio of 164:314"
}
$small = @($asset.Derivatives | Where-Object { $_.Role -ceq 'setup-wizard-small-image' })
$smallSizes = @($small | ForEach-Object { "$($_.Width)x$($_.Height)" })
foreach ($area in @('58x58', '77x77', '97x97', '116x116', '124x124', '143x143', '159x159')) {
    Assert-True ($smallSizes -contains $area) "a small wizard image for Inno Setup's $area image area ($($smallSizes -join ', '))"
}
foreach ($derivative in $small) { Assert-True ($derivative.Width -eq $derivative.Height) "$($derivative.Path) is square" }
Assert-True (@($asset.Derivatives | Where-Object { $_.Role -ceq 'docs-logo' }).Count -eq 1) 'one docs-logo derivative'

# The branding version's order (Semantic Versioning 2.0.0 precedence) that the generator enforces.
foreach ($pair in @(@('1.0.0', '1.0.0', 0), @('1.0.0', '1.1.0', -1), @('1.10.0', '1.9.0', 1), @('2.0.0', '1.99.99', 1),
                    @('1.0.0-alpha', '1.0.0', -1), @('1.0.0-alpha.1', '1.0.0-alpha.beta', -1), @('1.0.0-2', '1.0.0-10', -1),
                    @('1.0.0+build.5', '1.0.0', 0))) {
    $order = Compare-SemanticVersion $pair[0] $pair[1]
    Assert-True ($order -eq $pair[2]) "$($pair[0]) against $($pair[1]) is $($pair[2]) ($order)"
}
Assert-True (-not (Test-SemanticVersion '1.0') -and -not (Test-SemanticVersion '01.0.0') -and (Test-SemanticVersion '1.0.0-rc.1')) 'only Semantic Versions are accepted'

$work = Join-Path ([System.IO.Path]::GetTempPath()) ('lucent-branding-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
try {
    # --- 4. Generating from scratch reproduces the committed files ---------------------------------------

    $fresh = Join-Path $work 'fresh'
    New-Item -ItemType Directory -Path (Join-Path $fresh 'master') -Force | Out-Null
    Copy-Item -LiteralPath $asset.Master.FullPath -Destination (Join-Path $fresh 'master\lucent-logo.png')
    Copy-Item -LiteralPath (Join-Path $Root 'provenance.json') -Destination (Join-Path $fresh 'provenance.json')
    $generated = Invoke-BrandingGenerator -Root $fresh -Version $asset.Version
    $different = @()
    foreach ($path in @($asset.Files.Keys) + @('manifest.json')) {
        $mine = Join-Path $fresh $path.Replace('/', '\')
        $theirs = Join-Path $Root $path.Replace('/', '\')
        if (-not (Test-Path -LiteralPath $mine -PathType Leaf) -or
            -not [System.Linq.Enumerable]::SequenceEqual([byte[]][System.IO.File]::ReadAllBytes($mine), [byte[]][System.IO.File]::ReadAllBytes($theirs))) {
            $different += $path
        }
    }
    Assert-True ($different.Count -eq 0) "generating from the master alone gives the committed files byte for byte (different: $($different -join ', '))"
    Assert-True (@($generated.Written).Count -eq @($asset.Files.Keys).Count - 1) "the generation wrote every derivative, branding.json and manifest.json ($(@($generated.Written).Count) files)"
    $again = Invoke-BrandingGenerator -Root $fresh
    Assert-True ($again.Unchanged -and @($again.Written).Count -eq 0) 'generating again changes nothing'

    # The review evidence (LucentBrandingEvidence.cs, compiled only when it is drawn) compiles and draws.
    $sheet = Join-Path $work 'contact-sheet.png'
    try {
        New-BrandingContactSheet -Root $fresh -Path $sheet
        $sheetBytes = [System.IO.File]::ReadAllBytes($sheet)
        Assert-True ($sheetBytes.Length -gt 8 -and $sheetBytes[0] -eq 137 -and $sheetBytes[1] -eq 80 -and $sheetBytes[2] -eq 78 -and $sheetBytes[3] -eq 71) "the contact sheet of the generated asset is written as a PNG ($($sheetBytes.Length) bytes)"
    }
    catch { Assert-True $false "the contact sheet of the generated asset is drawn ($($_.Exception.Message))" }

    # --- 5. Tampering is found ----------------------------------------------------------------------------

    $copyIndex = 0
    function New-AssetCopy {
        $script:copyIndex++
        $copy = Join-Path $work "copy$($script:copyIndex)"
        Copy-Item -LiteralPath $Root -Destination $copy -Recurse
        return $copy
    }
    function Get-CheckErrors([string]$Folder) { return @((Invoke-BrandingGenerator -Root $Folder -Check).Errors) }
    function Set-ManifestEntry([string]$Folder, [string]$Path) {
        # Lists the file's present size and SHA-256 in the copy's manifest (as someone who edits the file
        # and its manifest entry by hand would).
        $manifestPath = Join-Path $Folder 'manifest.json'
        $manifest = [System.IO.File]::ReadAllText($manifestPath) | ConvertFrom-Json
        $entry = @($manifest.files | Where-Object { $_.path -ceq $Path })[0]
        $bytes = [System.IO.File]::ReadAllBytes((Join-Path $Folder $Path.Replace('/', '\')))
        $text = [System.IO.File]::ReadAllText($manifestPath)
        $text = $text.Replace("""size"": $($entry.size),`n   ""sha256"": ""$($entry.sha256)""", """size"": $($bytes.Length),`n   ""sha256"": ""$(Get-BytesSha256 $bytes)""")
        [System.IO.File]::WriteAllText($manifestPath, $text, (New-Object System.Text.ASCIIEncoding))
    }

    $copy = New-AssetCopy
    $target = Join-Path $copy 'app\lucent-32.png'
    $bytes = [System.IO.File]::ReadAllBytes($target); $bytes[$bytes.Length - 20] = $bytes[$bytes.Length - 20] -bxor 1
    [System.IO.File]::WriteAllBytes($target, $bytes)
    Assert-Problem (Get-CheckErrors $copy) 'app/lucent-32\.png does not have the SHA-256 manifest\.json lists' 'a changed byte is found by its SHA-256'

    $copy = New-AssetCopy
    $target = Join-Path $copy 'app\lucent-48.png'
    $image = Read-PngImage $target
    $image.Pixels[(24 * 48 + 24) * 4] = [byte](255 - $image.Pixels[(24 * 48 + 24) * 4])
    [System.IO.File]::WriteAllBytes($target, [LucentBranding.Png]::Write($image))
    Set-ManifestEntry $copy 'app/lucent-48.png'
    $errors = Get-CheckErrors $copy
    Assert-Problem $errors 'app/lucent-48\.png is not what the generator makes from the master \(stale or edited by hand\): 1 pixel\(s\) differ' 'a re-encoded derivative with its manifest entry updated is found by its pixels'
    Assert-Problem $errors 'manifest\.json is not the canonical manifest' 'and the manifest is not the generated one'

    $copy = New-AssetCopy
    Remove-Item -LiteralPath (Join-Path $copy 'installer\wizard-small-image-58.png')
    Assert-Problem (Get-CheckErrors $copy) 'installer/wizard-small-image-58\.png is listed but missing' 'a missing derivative is found'

    $copy = New-AssetCopy
    Copy-Item -LiteralPath (Join-Path $copy 'app\lucent-16.png') -Destination (Join-Path $copy 'app\lucent-17.png')
    Assert-Problem (Get-CheckErrors $copy) 'app/lucent-17\.png is in the folder but not in manifest\.json' 'a file added by hand is found'

    $copy = New-AssetCopy
    $provenancePath = Join-Path $copy 'provenance.json'
    [System.IO.File]::WriteAllText($provenancePath, ([System.IO.File]::ReadAllText($provenancePath).Replace($asset.Master.Sha256, ('0' * 64))), (New-Object System.Text.ASCIIEncoding))
    Set-ManifestEntry $copy 'provenance.json'
    # The master's size from the asset itself, so a new logo needs no change here (BRD-02).
    $masterSize = (Get-Item -LiteralPath $asset.Master.FullPath).Length
    Assert-Problem (Get-CheckErrors $copy) "provenance\.json describes a master of $masterSize bytes with SHA-256 0{64}, but the master is $masterSize bytes" 'a provenance record of another master is found'

    # W12 review BRD-04: an unlisted file is found when it is hidden too (git keeps it, without the attribute).
    $copy = New-AssetCopy
    $hiddenFile = Join-Path $copy 'app\lucent-15.png'
    Copy-Item -LiteralPath (Join-Path $copy 'app\lucent-16.png') -Destination $hiddenFile
    (Get-Item -LiteralPath $hiddenFile -Force).Attributes = [System.IO.FileAttributes]::Hidden
    Assert-Problem (Get-CheckErrors $copy) 'app/lucent-15\.png is in the folder but not in manifest\.json \(left over, or added by hand, a hidden or system file\)' 'BRD-04: a hidden file the manifest does not list is found'
    $iniFile = Join-Path $copy 'desktop.ini'
    [System.IO.File]::WriteAllText($iniFile, "[.ShellClassInfo]`r`n")
    (Get-Item -LiteralPath $iniFile -Force).Attributes = [System.IO.FileAttributes]::Hidden -bor [System.IO.FileAttributes]::System
    Assert-Problem (Get-CheckErrors $copy) '^desktop\.ini is in the folder but not in manifest\.json' 'BRD-04: a hidden system desktop.ini is found'
    Assert-Refused { Read-BrandingAsset -Root $copy } 'desktop\.ini is in the folder' 'BRD-04: packaging''s reading of the asset (Read-BrandingAsset) refuses it too'

    # W12 review BRD-05: a missing derivative is one problem, with the advice to write it again without
    # -Version, which then restores it and keeps the version.
    $copy = New-AssetCopy
    Remove-Item -LiteralPath (Join-Path $copy 'app\lucent-40.png')
    $errors = @(Get-CheckErrors $copy)
    Assert-True ($errors.Count -eq 1 -and $errors[0] -match '^app/lucent-40\.png is listed but missing$') "BRD-05: a missing derivative is one problem, not also 'not listed' ($($errors -join ' | '))"
    $remedy = @(Get-BrandingRemedy -Problems $errors)
    Assert-True ($remedy.Count -eq 1 -and $remedy[0] -match 'without -Version') "BRD-05: the advice is to run the generator without -Version ($($remedy -join ' | '))"
    $restored = Invoke-BrandingGenerator -Root $copy
    Assert-True ($restored.Unchanged -and $restored.Version -ceq $asset.Version -and @($restored.Written) -contains 'app/lucent-40.png' -and @($restored.Written).Count -eq 1) "BRD-05: the generator without -Version restores it and keeps the version ($(@($restored.Written) -join ', '))"
    Assert-True (@(Get-CheckErrors $copy).Count -eq 0) 'BRD-05: the restored copy passes the check'
    $target = Join-Path $copy 'app\lucent-32.png'
    $bytes = [System.IO.File]::ReadAllBytes($target); $bytes[$bytes.Length - 20] = $bytes[$bytes.Length - 20] -bxor 1
    [System.IO.File]::WriteAllBytes($target, $bytes)
    Assert-True (@(Get-BrandingRemedy -Problems @(Get-CheckErrors $copy))[0] -match 'without -Version') 'BRD-05: a damaged derivative gets the same advice'
    $restored = Invoke-BrandingGenerator -Root $copy
    Assert-True ($restored.Unchanged -and @($restored.Written) -contains 'app/lucent-32.png' -and @(Get-CheckErrors $copy).Count -eq 0) 'BRD-05: and is restored the same way'
    Assert-True (@(Get-BrandingRemedy -Problems @('app/lucent-17.png is in the folder but not in manifest.json (left over, or added by hand)'))[0] -match 'delete it') 'BRD-05: an unlisted file is to be deleted'

    # W12 review BRD-03: the licence recorded in provenance.json by hand: the check fails, the advice is a
    # PATCH version, and generating with it passes the check.
    $copy = New-AssetCopy
    $provenancePath = Join-Path $copy 'provenance.json'
    $record = [System.IO.File]::ReadAllText($provenancePath).Replace('"licence": null', '"licence": "All rights reserved (a test)."')
    [System.IO.File]::WriteAllText($provenancePath, $record, (New-Object System.Text.ASCIIEncoding))
    $errors = @(Get-CheckErrors $copy)
    Assert-Problem $errors '^provenance\.json is \d+ bytes, manifest\.json says \d+' 'BRD-03: a licence recorded by hand is a provenance.json the manifest does not list'
    Assert-True ((@(Get-BrandingRemedy -Problems $errors) -match 'PATCH when only provenance\.json changed').Count -eq 1) "BRD-03: the advice is a PATCH version ($(@(Get-BrandingRemedy -Problems $errors) -join ' | '))"
    Assert-Refused { Invoke-BrandingGenerator -Root $copy } 'PATCH for the same master rendered differently, or for a change of provenance\.json' 'BRD-03: without -Version the generator names a provenance change as PATCH'
    $patch = ($asset.Version -split '\.')
    $patched = Invoke-BrandingGenerator -Root $copy -Version ('{0}.{1}.{2}' -f $patch[0], $patch[1], ([int]($patch[2] -replace '[^0-9].*$', '') + 1))
    Assert-True (@($patched.Written) -ccontains 'manifest.json' -and @(Get-CheckErrors $copy).Count -eq 0) "BRD-03: generating with the next PATCH version records it, and the check passes ($(@($patched.Written) -join ', '))"

    # W12 review BRD-01: a manifest that names a file outside the folder is refused, and the file survives.
    $copy = New-AssetCopy
    $victim = Join-Path (Split-Path -Parent $copy) 'victim.txt'
    [System.IO.File]::WriteAllText($victim, 'must survive')
    $manifestPath = Join-Path $copy 'manifest.json'
    $tampered = [System.IO.File]::ReadAllText($manifestPath).Replace(" ""files"": [`n", " ""files"": [`n  {`n   ""path"": ""../victim.txt"",`n   ""size"": 12,`n   ""sha256"": ""$('0' * 64)""`n  },`n")
    [System.IO.File]::WriteAllText($manifestPath, $tampered, (New-Object System.Text.ASCIIEncoding))
    Assert-Problem (Get-CheckErrors $copy) "manifest\.json lists the invalid path '\.\./victim\.txt'" 'BRD-01: check mode finds the invalid path'
    Assert-True ((@(Get-BrandingRemedy -Problems @(Get-CheckErrors $copy)) -match 'restore the folder from git').Count -ge 1) 'BRD-01: and advises restoring the folder from git, not generating'
    Assert-Refused { Invoke-BrandingGenerator -Root $copy -Version 9.0.0 } 'lists the invalid path.*nothing was written or deleted' 'BRD-01: the generator refuses a manifest that names a file outside the folder'
    Assert-True ((Test-Path -LiteralPath $victim) -and [System.IO.File]::ReadAllText($victim) -ceq 'must survive') 'BRD-01: the file outside the folder survives'
    Remove-Item -LiteralPath $victim -Force
    Assert-True (-not (Test-BrandingPath '../x') -and -not (Test-BrandingPath 'a/../b') -and -not (Test-BrandingPath 'C:/x') -and -not (Test-BrandingPath 'a\b') -and
                 -not (Test-BrandingPath '/x') -and -not (Test-BrandingPath './x') -and (Test-BrandingPath 'app/lucent-16.png')) 'BRD-01: the path rule refuses traversal, drives, backslashes and absolute paths'

    $copy = New-AssetCopy
    $manifestPath = Join-Path $copy 'manifest.json'
    [System.IO.File]::WriteAllText($manifestPath, ([System.IO.File]::ReadAllText($manifestPath).Replace('"schema_version": 1,', '"schema_version": 2,')), (New-Object System.Text.ASCIIEncoding))
    Assert-Problem (Get-CheckErrors $copy) 'has schema_version 2; this tool reads version 1 only' 'a manifest of schema version 2 is refused'
    Assert-Refused { Invoke-BrandingGenerator -Root $copy -Version 9.0.0 } 'does not overwrite another schema' 'the generator does not overwrite a manifest of another schema version'

    $copy = New-AssetCopy
    $manifestPath = Join-Path $copy 'manifest.json'
    [System.IO.File]::WriteAllText($manifestPath, ([System.IO.File]::ReadAllText($manifestPath).Replace("""schema"": ""lucent.branding"",`n  ""schema_version"": 1", """schema"": ""lucent.branding"",`n  ""schema_version"": 2")), (New-Object System.Text.ASCIIEncoding))
    Assert-Problem (Get-CheckErrors $copy) "content is 'lucent\.branding' version 2; this tool reads 'lucent\.branding' version 1 only" 'branding content of schema version 2 is refused'

    # --- 6. The rendering rules on synthetic masters -------------------------------------------------------

    # An off-centre, nearly opaque (alpha 253) white artwork with specks of alpha 1 and 2 and a faint ring
    # two pixels outside its left edge, separated from it by transparent pixels.
    $clean = New-SyntheticMaster
    $noisy = New-SyntheticMaster -Edit {
        param($p)
        foreach ($xy in @(@(5, 5), @(590, 20), @(300, 580), @(20, 400))) { [LucentBrandingTest.Synthetic]::Set($p, 600, $xy[0], $xy[1], 0xFFFFFF, 2) }
        [LucentBrandingTest.Synthetic]::Set($p, 600, 550, 560, 0x000000, 1)
        for ($y = 200; $y -lt 440; $y++) { [LucentBrandingTest.Synthetic]::Set($p, 600, 87, $y, 0x808080, 12) }
    }
    $cleanMaster = Read-BrandingMaster -Bytes $clean
    $noisyMaster = Read-BrandingMaster -Bytes $noisy
    $analysis = $noisyMaster.Analysis
    Assert-True ($analysis.ContentLeft -eq 90 -and $analysis.ContentTop -eq 130 -and $analysis.ContentWidth -eq 380 -and $analysis.ContentHeight -eq 380) "the content box is the artwork's ($($analysis.ContentLeft), $($analysis.ContentTop), $($analysis.ContentWidth)x$($analysis.ContentHeight))"
    Assert-True ($analysis.SolidAlpha -eq 253) "the interior's alpha is found ($($analysis.SolidAlpha))"
    Assert-True ($analysis.ClearedPixels -eq 5 -and $analysis.FaintComponents -ge 1 -and $analysis.FaintPixels -ge 240) "the specks and the ring are cleared ($($analysis.ClearedPixels) pixels of alpha 1 to 2, $($analysis.FaintComponents) faint groups of $($analysis.FaintPixels) pixels)"
    $cleanImage = $cleanMaster.Prepared.Render(64, 64, 4, 4, 56)
    $noisyImage = $noisyMaster.Prepared.Render(64, 64, 4, 4, 56)
    $largest = 0
    Assert-True ($noisyImage.CountDifferences($cleanImage, [ref]$largest) -eq 0) 'the specks and the ring leave no trace: the rendering equals that of the clean master'
    $sum = 0.0; $sumX = 0.0; $sumY = 0.0; $fringe = 0
    for ($y = 0; $y -lt 64; $y++) {
        for ($x = 0; $x -lt 64; $x++) {
            $i = ($y * 64 + $x) * 4
            $a = $noisyImage.Pixels[$i + 3]
            if ($a -eq 0) { continue }
            $sum += $a; $sumX += $a * ($x + 0.5); $sumY += $a * ($y + 0.5)
            if ($noisyImage.Pixels[$i] -ne 255 -or $noisyImage.Pixels[$i + 1] -ne 255 -or $noisyImage.Pixels[$i + 2] -ne 255) { $fringe++ }
        }
    }
    $centreX = $sumX / $sum; $centreY = $sumY / $sum
    Assert-True ([Math]::Abs($centreX - 32) -lt 0.02 -and [Math]::Abs($centreY - 32) -lt 0.02) ("the off-centre artwork comes out centred (centroid {0:N3}, {1:N3})" -f $centreX, $centreY)
    Assert-True ($noisyImage.Alpha(32, 32) -eq 255 -and $noisyImage.Alpha(0, 0) -eq 0) "the nearly opaque interior is opaque, the padding transparent ($($noisyImage.Alpha(32, 32)), $($noisyImage.Alpha(0, 0)))"
    Assert-True ($fringe -eq 0) "a white artwork's anti-aliased edge stays white: no dark fringe ($fringe pixels are not white)"

    # Linear light: a square artwork of 448 px rendered at 56 px (8 x 8 master pixels per output pixel),
    # black left of a line through the middle of output column 32, white right of it.
    $split = New-SyntheticMaster -Left 70 -Top 90 -Width 448 -Height 448 -Radius 0 -LeftColour 0x000000 -RightColour 0xFFFFFF -Split (70 + 8 * 28 + 4)
    $splitImage = (Read-BrandingMaster -Bytes $split).Prepared.Render(64, 64, 4, 4, 56)
    $mid = (40 * 64 + 32) * 4
    $values = "$($splitImage.Pixels[$mid]), $($splitImage.Pixels[$mid + 1]), $($splitImage.Pixels[$mid + 2]), $($splitImage.Pixels[$mid + 3])"
    Assert-True ($values -ceq '188, 188, 188, 255') "half black, half white averages in linear light to sRGB 188 ($values)"
    Assert-True ($splitImage.Pixels[(40 * 64 + 31) * 4] -eq 0 -and $splitImage.Pixels[(40 * 64 + 33) * 4] -eq 255) 'the neighbouring columns stay black and white'

    # Refused masters.
    Assert-Refused { Read-BrandingMaster -Bytes (New-SyntheticMaster -Alpha 200) } 'translucent' 'a translucent artwork is refused'
    Assert-Refused { Read-BrandingMaster -Bytes (New-SyntheticMaster -Left 0 -Radius 0) } 'touches the edge' 'an artwork touching the edge is refused'
    Assert-Refused { Read-BrandingMaster -Bytes ([LucentBranding.Png]::InsertChunk($clean, 'iCCP', [System.Text.Encoding]::ASCII.GetBytes("profile`0`0"))) } 'iCCP chunk' 'a master with a colour profile is refused'
    # W12 review BRD-06: PNG 3's coding-independent code points (Display P3, BT.2020 with PQ) and its HDR
    # chunks, which a viewer applies before iCCP and sRGB, are refused; so is any chunk the generator does
    # not know (an allowlist), while metadata passes.
    Assert-Refused { Read-BrandingMaster -Bytes ([LucentBranding.Png]::InsertChunk($clean, 'cICP', [byte[]](12, 13, 0, 1))) } 'cICP chunk' 'BRD-06: a Display P3 (cICP) master is refused'
    Assert-Refused { Read-BrandingMaster -Bytes ([LucentBranding.Png]::InsertChunk($clean, 'cICP', [byte[]](9, 16, 0, 1))) } 'cICP chunk' 'BRD-06: a BT.2020 PQ (cICP) master is refused'
    Assert-Refused { Read-BrandingMaster -Bytes ([LucentBranding.Png]::InsertChunk($clean, 'mDCV', (New-Object byte[] 24))) } 'mDCV chunk' 'BRD-06: an HDR mastering-display (mDCV) master is refused'
    Assert-Refused { Read-BrandingMaster -Bytes ([LucentBranding.Png]::InsertChunk($clean, 'cLLI', (New-Object byte[] 8))) } 'cLLI chunk' 'BRD-06: an HDR light-level (cLLI) master is refused'
    Assert-Refused { Read-BrandingMaster -Bytes ([LucentBranding.Png]::InsertChunk($clean, 'zzZz', [byte[]](1, 2))) } 'zzZz chunk' 'BRD-06: a chunk the generator does not know is refused'
    $withText = [LucentBranding.Png]::InsertChunk($clean, 'tEXt', [System.Text.Encoding]::ASCII.GetBytes("Comment`0a test"))
    Assert-True ((Read-BrandingMaster -Bytes $withText).Png.ChunkList() -match 'tEXt') 'BRD-06: a text chunk (metadata) is accepted'
    $rgb = [byte[]]$clean.Clone()
    $rgb[25] = 2   # IHDR colour type: RGB without alpha
    $crc = [LucentBranding.Checksums]::Crc32($rgb, 12, 17)
    $rgb[29] = [byte](($crc -shr 24) -band 0xFF); $rgb[30] = [byte](($crc -shr 16) -band 0xFF); $rgb[31] = [byte](($crc -shr 8) -band 0xFF); $rgb[32] = [byte]($crc -band 0xFF)
    Assert-Refused { Read-BrandingMaster -Bytes $rgb } 'not 8-bit RGBA' 'a master that is not 8-bit RGBA is refused'
    Assert-Refused { Read-BrandingMaster -Bytes (New-SyntheticMaster -Size 400 -Left 40 -Top 40 -Width 300 -Height 300) } 'must be 512 to 8192 px' 'a master that is too small is refused'
    Assert-Refused { Read-BrandingMaster -Bytes (New-SyntheticMaster -Width 460 -Height 300 -Left 60 -Top 150 -Radius 30) } 'not roughly square' 'an artwork that is not roughly square is refused'

    # A new master: its provenance first, then a higher version.
    $copy = New-AssetCopy
    $newMaster = Join-Path $work 'new-master.png'
    [System.IO.File]::WriteAllBytes($newMaster, $clean)
    Assert-Refused { Invoke-BrandingGenerator -Root $copy -MasterFile $newMaster -Version 2.0.0 } 'record this master''s provenance' 'a new master without its provenance record is refused'
    Write-Provenance $copy $clean
    Assert-Refused { Invoke-BrandingGenerator -Root $copy -MasterFile $newMaster } 'pass -Version' 'a new master needs a new branding version'
    Assert-Refused { Invoke-BrandingGenerator -Root $copy -MasterFile $newMaster -Version $asset.Version } 'is not higher than' 'the new version must be higher'
    $replaced = Invoke-BrandingGenerator -Root $copy -MasterFile $newMaster -Version 2.0.0
    Assert-True ($replaced.Version -ceq '2.0.0' -and @($replaced.Written) -contains 'master/lucent-logo.png' -and @($replaced.Written) -contains 'windows/lucent.ico') "the new master and its derivatives are written ($(@($replaced.Written).Count) files)"
    $recheck = Invoke-BrandingGenerator -Root $copy -Check
    Assert-True ($recheck.Errors.Count -eq 0) "the regenerated copy passes the check ($($recheck.Errors -join ' | '))"
    $replacedIcon = @(Test-IconFile -Path (Join-Path $copy 'windows\lucent.ico') -ExpectedSizes @(Get-IconSizes (Read-BrandingAsset -Root $copy)))
    Assert-True ($replacedIcon.Count -eq 0) "the new master's Windows icon is valid ($($replacedIcon -join ' | '))"
}
finally {
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host "$($script:checks) checks, $($script:failures) failed"
if ($script:failures -gt 0) { exit 1 }
exit 0
