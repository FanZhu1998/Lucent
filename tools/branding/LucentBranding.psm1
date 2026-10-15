<#
Lucent's branding module (ADR 0025): the logo's master and its generated derivatives in
assets\branding, the generator behind tools\branding\Update-Branding.ps1, and the checks behind
tools\package.ps1 and the CTest suites lucent_branding_tests and lucent_program_icon_tests
(tests\branding).

    Get-BrandingRecipe        the derivatives: path, role, size and composition of each (the one table to
                              extend; consumers find files by role, never by size)
    Invoke-BrandingGenerator  renders every derivative from the master and writes the asset (or, with
                              -Check, compares the committed files with a fresh rendering)
    Read-BrandingAsset        reads and verifies manifest.json and branding.json (schemas, sizes, SHA-256)
                              and returns the derivatives by role: what tools\package.ps1 and the tests use
    Test-BrandingProvenance   provenance.json, the hand-written record of where the master comes from and
                              on which terms it is used, against the master's bytes
    Test-IconFile             the structure of an .ico (sizes, depth, alpha, PNG and BMP images, Windows'
                              own reading of each image)
    Test-ProgramIcon          an executable carries the branding icon as an icon group, image for image
    New-BrandingContactSheet  evidence: every derivative at 1:1 and magnified on light, grey and dark
    New-ProgramIconSheet      evidence: the icons of executables as Windows draws them

The image code is LucentBranding.cs (deterministic, no GDI+); the evidence drawing is
LucentBrandingEvidence.cs (System.Drawing). Windows PowerShell 5.1 compatible. Keep this file ASCII.
#>

Set-StrictMode -Version 3.0

# --- Contract constants ------------------------------------------------------------------------------

$script:ManifestSchema = 'lucent.asset-manifest'
$script:ManifestSchemaVersion = 1
$script:BrandingSchema = 'lucent.branding'
$script:BrandingSchemaVersion = 1
$script:AssetId = 'branding'
$script:MinimumAppVersion = '0.1.0'
$script:MasterPath = 'master/lucent-logo.png'
$script:BrandingDocument = 'branding.json'
$script:ManifestName = 'manifest.json'
# The hand-written provenance record of the master: listed in the manifest, never written by the
# generator, checked against the master's bytes.
$script:ProvenanceDocument = 'provenance.json'
$script:ProvenanceSchema = 'lucent.branding-provenance'
$script:ProvenanceSchemaVersion = 1
# Files of the folder that the manifest does not list: itself and the README (documentation).
$script:UnlistedFiles = @('manifest.json', 'README.md')

# The alpha rule (ADR 0025): alpha 1..NoiseAlpha is the image generator's noise and is cleared; the
# interior's 1st-percentile alpha must be at least MinimumSolidAlpha and becomes opaque.
$script:NoiseAlpha = 2
$script:MinimumSolidAlpha = 240
# The master: an 8-bit RGBA PNG between these sizes whose content box is roughly square.
$script:MasterMinimumSize = 512
$script:MasterMaximumSize = 8192
$script:ContentAspectLimit = 1.25
# The ancillary chunks a master may have: none of them changes how its pixels are read (sRGB tags the
# colours as the generator reads them; caBX holds content credentials; text, physical size, time,
# Exif, a background colour hint and a suggested palette are metadata). Every other ancillary chunk is
# refused, a colour space (iCCP, cHRM, gAMA, sBIT, PNG 3's cICP and its HDR mDCV and cLLI, which a viewer
# applies before iCCP and sRGB), a transparency key (tRNS), an animation (acTL, fcTL, fdAT) or one that
# a later PNG edition adds alike: the master must be sRGB (untagged, or tagged sRGB) and a single image
# (W12 review BRD-06).
$script:AllowedMasterChunks = @('IHDR', 'PLTE', 'IDAT', 'IEND', 'sRGB', 'caBX', 'tEXt', 'zTXt', 'iTXt', 'pHYs', 'tIME', 'eXIf', 'bKGD', 'sPLT')

function Initialize-BrandingType {
    if ('LucentBranding.PreparedMaster' -as [type]) { return }
    Add-Type -TypeDefinition ([System.IO.File]::ReadAllText((Join-Path $PSScriptRoot 'LucentBranding.cs'))) -Language CSharp
}

function Initialize-BrandingEvidenceType {
    # Compiled on its own (Add-Type assemblies live in memory and cannot reference each other), so it
    # takes pixels and icon resources as arrays.
    Initialize-BrandingType
    if ('LucentBrandingEvidence.ContactSheet' -as [type]) { return }
    Add-Type -AssemblyName System.Drawing
    Add-Type -TypeDefinition ([System.IO.File]::ReadAllText((Join-Path $PSScriptRoot 'LucentBrandingEvidence.cs'))) -Language CSharp `
        -ReferencedAssemblies @('System.Drawing')
}

# --- The recipe --------------------------------------------------------------------------------------

function Get-IconPadding {
    # The margin of an icon-style rendering on each side: round(size / 16) pixels (1 px at 16, 2 px at 24
    # to 36, 3 px at 40 to 48, 4 px at 64, 16 px at 256), so the artwork takes 87.5 % of the icon, as
    # Windows' own plated app icons do, and starts on a whole pixel. The content box (alpha >= 128) fills
    # the rest; the master's anti-aliased rim around it reaches about one pixel into the margin (alpha up
    # to 26 at 256 px), so the margin is nearly, not exactly, transparent.
    param([Parameter(Mandatory)][int]$Size)
    return [int][Math]::Floor($Size / 16.0 + 0.5)
}

function New-Artwork([int]$Left, [int]$Top, [int]$Size) {
    # Where the content box's longer side goes: the square [left, left + size) x [top, top + size).
    return [ordered]@{ left = $Left; top = $Top; size = $Size }
}

function New-IconArtwork([int]$Size) {
    $padding = Get-IconPadding -Size $Size
    return (New-Artwork $padding $padding ($Size - 2 * $padding))
}

function Get-BrandingRecipe {
    <#
    .SYNOPSIS
        Every derivative of the master, in the order branding.json lists them. Adding a size or a target
        adds an entry (and is a new MINOR branding version); consumers find files by role.
    .DESCRIPTION
        Roles (the stable contract with the consumers, ADR 0025):
          windows-icon              windows\lucent.ico: Lucent.exe's and lucent-worker.exe's IDI_ICON1
                                    (cmake\LucentBranding.cmake) and Setup's icon (SetupIconFile)
          app-icon                  app\lucent-<N>.png: Lucent.exe's window icon (a QIcon with every size)
                                    and the About dialog's logo, from a Qt resource; every size the
                                    window's small and large icons and the 64 px logo need at 100 to 300 %
          setup-wizard-image        installer\wizard-image-<W>x<H>.png: WizardImageFile, one per
                                    image-area size Inno Setup 6.7.3 documents for 100 to 250 %
          setup-wizard-small-image  installer\wizard-small-image-<N>.png: WizardSmallImageFile, likewise
          docs-logo                 docs\lucent-logo.png: README.md and other documentation
    #>
    $derivatives = New-Object System.Collections.Generic.List[object]
    $iconSizes = @(16, 20, 24, 32, 40, 48, 64, 96, 128, 256)
    $derivatives.Add([ordered]@{
        path = 'windows/lucent.ico'; role = 'windows-icon'; format = 'ico'
        images = @($iconSizes | ForEach-Object {
            [ordered]@{ width = $_; height = $_; encoding = $(if ($_ -ge 256) { 'png' } else { 'bmp32' }); artwork = (New-IconArtwork $_) } }) })
    # Small icon (SM_CXSMICON) at 100..300 %: 16 20 24 28 32 36 40 48; large icon (SM_CXICON): 32 40 48
    # 56 64 72 80 96; the About dialog's 64 px logo: 64 80 96 112 128 144 160 192; and 256.
    foreach ($size in @(16, 20, 24, 28, 32, 36, 40, 48, 56, 64, 72, 80, 96, 112, 128, 144, 160, 192, 256)) {
        $derivatives.Add([ordered]@{ path = "app/lucent-$size.png"; role = 'app-icon'; format = 'png'; width = $size; height = $size; artwork = (New-IconArtwork $size) })
    }
    # Inno Setup 6.7.3's image areas at 100, 125, 150, 175, 200, 225 and 250 % (WizardImageFile and
    # WizardSmallImageFile in its help); Setup picks the file that fits best. The large image shows the
    # logo at half the panel's width, centred, its middle at 38 % of the panel's height.
    foreach ($area in @(@(202, 386), @(269, 515), @(336, 643), @(403, 772), @(430, 824), @(498, 953), @(534, 1022))) {
        $width = $area[0]; $height = $area[1]
        $size = [int][Math]::Floor($width * 0.5 + 0.5)
        $left = [int][Math]::Floor(($width - $size) / 2.0 + 0.5)
        $top = [int][Math]::Floor($height * 0.38 - $size / 2.0 + 0.5)
        $derivatives.Add([ordered]@{ path = "installer/wizard-image-$($width)x$($height).png"; role = 'setup-wizard-image'; format = 'png'
            width = $width; height = $height; artwork = (New-Artwork $left $top $size) })
    }
    foreach ($size in @(58, 77, 97, 116, 124, 143, 159)) {
        $derivatives.Add([ordered]@{ path = "installer/wizard-small-image-$size.png"; role = 'setup-wizard-small-image'; format = 'png'
            width = $size; height = $size; artwork = (New-IconArtwork $size) })
    }
    # README.md shows it at 96 px; 256 px stays sharp on high-density screens. A 4 px margin.
    $derivatives.Add([ordered]@{ path = 'docs/lucent-logo.png'; role = 'docs-logo'; format = 'png'; width = 256; height = 256; artwork = (New-Artwork 4 4 248) })
    return $derivatives.ToArray()
}

function Get-BrandingMethod {
    # How every derivative is made, recorded in branding.json.
    return [ordered]@{
        alpha = "Alpha 1 to $($script:NoiseAlpha) is cleared as the image generator's noise, then every group of touching (4-connected) non-transparent pixels in which no pixel reaches alpha 128 (specks, and ringing separated from an edge by transparent pixels). Coverage is min(1, alpha / solid_alpha), where solid_alpha is the 1st-percentile alpha of the artwork's solid interior (at least $($script:MinimumSolidAlpha)): the nearly opaque artwork becomes opaque and its anti-aliased edges keep their ramp."
        placement = 'The content box (the bounding box of alpha >= 128) is cropped, and its longer side spans the derivative''s artwork square, centred in it: the master''s off-centre placement is corrected and its proportions are kept.'
        resampling = 'Each pixel is the area average (box filter, exact footprint coverage) of the master under it, in premultiplied linear light: sRGB (IEC 61966-2-1) decoded and encoded with deterministic arithmetic; alpha is the covered fraction, colour the average colour of what covers it. Fully transparent pixels are 0, 0, 0, 0.'
        icon_padding = 'Icon-style renderings (the Windows icon, the app icons, the small wizard images) place the content box (alpha >= 128) round(size / 16) pixels from each side; the artwork''s anti-aliased rim, below alpha 128, reaches about one pixel into that margin.'
        wizard_image = 'The large wizard images show the logo at half the image''s width, centred horizontally, its middle at 38 % of the image''s height, on a transparent background.'
        docs_logo = 'The documentation logo keeps a 4-pixel margin.'
        png = '8-bit RGBA; IHDR, IDAT and IEND only (no metadata, no timestamps); each row filtered by the smallest sum of absolute differences; one zlib stream with fixed Huffman codes (greedy LZ77): the same pixels always give the same bytes.'
        ico = 'Images below 256 px are 32-bit BMPs (BITMAPINFOHEADER, straight alpha, bottom-up, AND mask set where alpha is 0); the 256 px image is PNG.'
    }
}

# --- Canonical JSON (the form of src\assets' write_asset_manifest: one-space indentation, LF) ---------

function ConvertTo-JsonString([string]$Text) {
    $builder = New-Object System.Text.StringBuilder
    [void]$builder.Append('"')
    foreach ($char in $Text.ToCharArray()) {
        $code = [int]$char
        if ($code -eq 0x22) { [void]$builder.Append('\"') }
        elseif ($code -eq 0x5C) { [void]$builder.Append('\\') }
        elseif ($code -eq 0x08) { [void]$builder.Append('\b') }
        elseif ($code -eq 0x0C) { [void]$builder.Append('\f') }
        elseif ($code -eq 0x0A) { [void]$builder.Append('\n') }
        elseif ($code -eq 0x0D) { [void]$builder.Append('\r') }
        elseif ($code -eq 0x09) { [void]$builder.Append('\t') }
        elseif ($code -lt 0x20) { [void]$builder.Append(('\u{0:x4}' -f $code)) }
        elseif ($code -gt 0x7E) { throw "JSON text must be ASCII: '$Text'" }
        else { [void]$builder.Append($char) }
    }
    [void]$builder.Append('"')
    return $builder.ToString()
}

function ConvertTo-CanonicalJson {
    <#
    .SYNOPSIS
        Ordered dictionaries, arrays, strings, integers, booleans and null as JSON with one-space
        indentation, LF line endings and a final newline (nlohmann::ordered_json::dump(1)).
    #>
    param([AllowNull()]$Value)
    $lines = New-Object System.Text.StringBuilder
    Write-CanonicalJsonValue -Builder $lines -Value $Value -Depth 0
    [void]$lines.Append("`n")
    return $lines.ToString()
}

function Write-CanonicalJsonValue($Builder, $Value, [int]$Depth) {
    $inner = ' ' * ($Depth + 1)
    if ($null -eq $Value) { [void]$Builder.Append('null'); return }
    if ($Value -is [System.Collections.IDictionary]) {
        if ($Value.Count -eq 0) { [void]$Builder.Append('{}'); return }
        [void]$Builder.Append("{`n")
        $first = $true
        foreach ($key in $Value.Keys) {
            if (-not $first) { [void]$Builder.Append(",`n") }
            $first = $false
            [void]$Builder.Append($inner + (ConvertTo-JsonString ([string]$key)) + ': ')
            Write-CanonicalJsonValue -Builder $Builder -Value $Value[$key] -Depth ($Depth + 1)
        }
        [void]$Builder.Append("`n" + (' ' * $Depth) + '}')
        return
    }
    if ($Value -is [string]) { [void]$Builder.Append((ConvertTo-JsonString $Value)); return }
    if ($Value -is [bool]) { [void]$Builder.Append($(if ($Value) { 'true' } else { 'false' })); return }
    if ($Value -is [int] -or $Value -is [long] -or $Value -is [uint32] -or $Value -is [uint64]) {
        [void]$Builder.Append($Value.ToString([System.Globalization.CultureInfo]::InvariantCulture)); return
    }
    if ($Value -is [System.Collections.IEnumerable]) {
        $items = New-Object System.Collections.ArrayList
        foreach ($item in $Value) { [void]$items.Add($item) }
        if ($items.Count -eq 0) { [void]$Builder.Append('[]'); return }
        [void]$Builder.Append("[`n")
        for ($i = 0; $i -lt $items.Count; $i++) {
            if ($i -gt 0) { [void]$Builder.Append(",`n") }
            [void]$Builder.Append($inner)
            Write-CanonicalJsonValue -Builder $Builder -Value $items[$i] -Depth ($Depth + 1)
        }
        [void]$Builder.Append("`n" + (' ' * $Depth) + ']')
        return
    }
    throw "ConvertTo-CanonicalJson: unsupported value of type $($Value.GetType().FullName)"
}

# --- Small helpers -----------------------------------------------------------------------------------

function Get-BytesSha256([byte[]]$Bytes) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try { return (($sha.ComputeHash($Bytes) | ForEach-Object { $_.ToString('x2') }) -join '') } finally { $sha.Dispose() }
}

function Get-AsciiBytes([string]$Text) { return [System.Text.Encoding]::ASCII.GetBytes($Text) }

function Get-ErrorText($ErrorRecord) {
    # The message of the .NET exception behind an error (a BrandingException of the C# code reaches
    # PowerShell wrapped in a MethodInvocationException).
    $exception = $ErrorRecord.Exception
    while ($exception.InnerException) { $exception = $exception.InnerException }
    return $exception.Message
}

function Test-SemanticVersion([string]$Text) {
    return [bool]($Text -cmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-((0|[1-9][0-9]*|[0-9]*[A-Za-z-][0-9A-Za-z-]*)(\.(0|[1-9][0-9]*|[0-9]*[A-Za-z-][0-9A-Za-z-]*))*))?(\+[0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*)?$')
}

function Compare-SemanticVersion([string]$A, [string]$B) {
    # Semantic Versioning 2.0.0 precedence: -1, 0 or 1 (build metadata ignored).
    $partsA = ($A -split '\+')[0] -split '-', 2
    $partsB = ($B -split '\+')[0] -split '-', 2
    $coreA = @($partsA[0] -split '\.' | ForEach-Object { [System.Numerics.BigInteger]::Parse($_) })
    $coreB = @($partsB[0] -split '\.' | ForEach-Object { [System.Numerics.BigInteger]::Parse($_) })
    for ($i = 0; $i -lt 3; $i++) { if ($coreA[$i] -ne $coreB[$i]) { return $(if ($coreA[$i] -lt $coreB[$i]) { -1 } else { 1 }) } }
    # @() around the if: an empty array an if statement outputs would otherwise become $null.
    $preA = @(if ($partsA.Count -gt 1) { $partsA[1] -split '\.' })
    $preB = @(if ($partsB.Count -gt 1) { $partsB[1] -split '\.' })
    if ($preA.Count -eq 0 -or $preB.Count -eq 0) { return [Math]::Sign($preB.Count - $preA.Count) }
    for ($i = 0; $i -lt [Math]::Min($preA.Count, $preB.Count); $i++) {
        $numericA = $preA[$i] -match '^[0-9]+$'; $numericB = $preB[$i] -match '^[0-9]+$'
        if ($numericA -and $numericB) {
            $x = [System.Numerics.BigInteger]::Parse($preA[$i]); $y = [System.Numerics.BigInteger]::Parse($preB[$i])
            if ($x -ne $y) { return $(if ($x -lt $y) { -1 } else { 1 }) }
        }
        elseif ($numericA) { return -1 }
        elseif ($numericB) { return 1 }
        else {
            $c = [string]::CompareOrdinal($preA[$i], $preB[$i])
            if ($c -ne 0) { return [Math]::Sign($c) }
        }
    }
    return [Math]::Sign($preA.Count - $preB.Count)
}

function Get-Property($Object, [string]$Name) {
    if ($null -eq $Object) { return $null }
    if ($Object -is [System.Collections.IDictionary]) { if ($Object.Contains($Name)) { return $Object[$Name] } else { return $null } }
    if ($Object.PSObject.Properties.Name -contains $Name) { return $Object.$Name }
    return $null
}

function Test-ExactMembers($Object, [string[]]$Names, [string]$What) {
    # Strictness of the asset manifest: a member that is missing or unknown is an error.
    $problems = @()
    $present = @($Object.PSObject.Properties | ForEach-Object Name)
    foreach ($name in $Names) { if ($present -cnotcontains $name) { $problems += "$What has no member '$name'" } }
    foreach ($name in $present) { if ($Names -cnotcontains $name) { $problems += "$What has an unknown member '$name'" } }
    return $problems
}

# --- Rendering -----------------------------------------------------------------------------------------

function Read-BrandingMaster {
    <#
    .SYNOPSIS
        Reads and validates a master: an 8-bit RGBA PNG of 512 to 8192 px per side, sRGB (no colour
        profile, gamma or chromaticity chunk), one image, a roughly square content box away from the edges,
        an opaque or nearly opaque interior. Returns the bytes, the decoded file and the prepared master.
    #>
    param([Parameter(Mandatory)][byte[]]$Bytes)
    Initialize-BrandingType
    try { $png = [LucentBranding.Png]::Read($Bytes) }
    catch { throw "The master is not a PNG the branding generator reads: $(Get-ErrorText $_)." }
    $image = $png.Image
    foreach ($chunk in $png.Chunks) {
        if ($script:AllowedMasterChunks -cnotcontains $chunk.Type) {
            throw "The master has a $($chunk.Type) chunk, which can change how its pixels are read: a master must be an untagged (or sRGB-tagged) single image (chunks $($script:AllowedMasterChunks -join ', ') only); convert it to plain sRGB first."
        }
    }
    foreach ($dimension in @($image.Width, $image.Height)) {
        if ($dimension -lt $script:MasterMinimumSize -or $dimension -gt $script:MasterMaximumSize) {
            throw "The master is $($image.Width)x$($image.Height) px; it must be $($script:MasterMinimumSize) to $($script:MasterMaximumSize) px on each side."
        }
    }
    try { $prepared = [LucentBranding.PreparedMaster]::Prepare($image, $script:NoiseAlpha, $script:MinimumSolidAlpha) }
    catch { throw "The master cannot be used: $(Get-ErrorText $_)." }
    $analysis = $prepared.Analysis
    $aspect = [Math]::Max($analysis.ContentWidth, $analysis.ContentHeight) / [double][Math]::Min($analysis.ContentWidth, $analysis.ContentHeight)
    if ($aspect -gt $script:ContentAspectLimit) {
        throw "The master's artwork is $($analysis.ContentWidth)x$($analysis.ContentHeight) px, not roughly square (aspect $([Math]::Round($aspect, 3)) > $($script:ContentAspectLimit)): an app icon needs a square composition (ADR 0025)."
    }
    return [pscustomobject]@{ Bytes = $Bytes; Png = $png; Prepared = $prepared; Analysis = $analysis }
}

function Test-BrandingProvenance {
    <#
    .SYNOPSIS
        Checks provenance.json ("lucent.branding-provenance" version 1) against the master's bytes.
        Returns the problems (empty when the record is valid and describes this master).
    .DESCRIPTION
        The record is hand-written (assets\branding\README.md) and listed in the manifest. Exactly these
        members, as for every Lucent manifest (an unknown member or another schema version is refused,
        never guessed):
          schema, schema_version   "lucent.branding-provenance", 1
          master                   path (master/lucent-logo.png), size and sha256 of the master it
                                   describes: a new master needs a new record before the generator
                                   accepts it
          description              what the logo shows
          supplied_by, supplied    who supplied the master for Lucent, and when (YYYY-MM-DD)
          origin                   how it was made: an object of text members (tool, date, content
                                   credentials, ...), as the master's own records state them
          modifications            what was changed in the master before it was committed
          licence                  the logo's licence terms as the project owner decided them, or null
                                   while they are undecided (a release needs them: docs\release\
                                   checklist.md, gate 8)
          licence_status           the state of that decision, in words
    #>
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][byte[]]$MasterBytes)
    $problems = New-Object System.Collections.Generic.List[string]
    $name = $script:ProvenanceDocument
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        $problems.Add("$name is missing: record where the master comes from before generating (assets\branding\README.md, ""Replacing the logo"")")
        return $problems.ToArray()
    }
    try { $record = [System.IO.File]::ReadAllText($Path) | ConvertFrom-Json }
    catch { $problems.Add("$name is not JSON: $($_.Exception.Message)"); return $problems.ToArray() }
    if ((Get-Property $record 'schema') -cne $script:ProvenanceSchema -or (Get-Property $record 'schema_version') -ne $script:ProvenanceSchemaVersion) {
        $problems.Add("$name is '$(Get-Property $record 'schema')' version $(Get-Property $record 'schema_version'); this tool reads '$($script:ProvenanceSchema)' version $($script:ProvenanceSchemaVersion) only")
        return $problems.ToArray()
    }
    foreach ($problem in (Test-ExactMembers $record @('schema', 'schema_version', 'master', 'description', 'supplied_by', 'supplied', 'origin',
            'modifications', 'licence', 'licence_status') $name)) { $problems.Add($problem) }
    foreach ($member in @('description', 'supplied_by', 'modifications', 'licence_status')) {
        $value = Get-Property $record $member
        if ($value -isnot [string] -or -not $value.Trim()) { $problems.Add("$name`: $member must be text") }
    }
    if ([string](Get-Property $record 'supplied') -cnotmatch '^\d{4}-\d{2}-\d{2}$') { $problems.Add("$name`: supplied must be a date (YYYY-MM-DD)") }
    $licence = Get-Property $record 'licence'
    if ($null -ne $licence -and ($licence -isnot [string] -or -not $licence.Trim())) { $problems.Add("$name`: licence must be text, or null while it is undecided") }
    $origin = Get-Property $record 'origin'
    $originMembers = @(if ($origin -is [System.Management.Automation.PSCustomObject]) { $origin.PSObject.Properties })
    if ($originMembers.Count -eq 0) { $problems.Add("$name`: origin must be an object with at least one member") }
    foreach ($member in $originMembers) {
        if ($member.Value -isnot [string] -or -not $member.Value.Trim()) { $problems.Add("$name`: origin.$($member.Name) must be text") }
    }
    $master = Get-Property $record 'master'
    if ($master -isnot [System.Management.Automation.PSCustomObject]) { $problems.Add("$name`: master must be an object"); return $problems.ToArray() }
    foreach ($problem in (Test-ExactMembers $master @('path', 'size', 'sha256') "$name's master")) { $problems.Add($problem) }
    $sha256 = Get-BytesSha256 $MasterBytes
    if ((Get-Property $master 'path') -cne $script:MasterPath) { $problems.Add("$name describes the master '$(Get-Property $master 'path')', not $($script:MasterPath)") }
    if ([string](Get-Property $master 'sha256') -cne $sha256 -or [long](Get-Property $master 'size') -ne $MasterBytes.Length) {
        $problems.Add(("{0} describes a master of {1} bytes with SHA-256 {2}, but the master is {3} bytes with SHA-256 {4}: record this master's provenance in {0} first (assets\branding\README.md, ""Replacing the logo"")" -f
            $name, (Get-Property $master 'size'), (Get-Property $master 'sha256'), $MasterBytes.Length, $sha256))
    }
    return $problems.ToArray()
}

function Invoke-BrandingRendering {
    <#
    .SYNOPSIS
        Renders every derivative of the recipe from a prepared master: returns an ordered dictionary of
        relative path -> bytes (identical renderings are shared, so an ICO image and the PNG of the same
        size hold the same pixels).
    #>
    param([Parameter(Mandatory)]$Master, [Parameter(Mandatory)][object[]]$Recipe)
    $cache = @{}
    $render = {
        param([int]$Width, [int]$Height, $Artwork)
        $key = "$Width,$Height,$($Artwork.left),$($Artwork.top),$($Artwork.size)"
        if (-not $cache.ContainsKey($key)) {
            $cache[$key] = $Master.Prepared.Render($Width, $Height, [double]$Artwork.left, [double]$Artwork.top, [double]$Artwork.size)
        }
        return $cache[$key]
    }
    $files = [ordered]@{}
    foreach ($derivative in $Recipe) {
        switch ($derivative.format) {
            'png' {
                $image = & $render $derivative.width $derivative.height $derivative.artwork
                $files[$derivative.path] = [LucentBranding.Png]::Write($image)
            }
            'ico' {
                $images = New-Object 'System.Collections.Generic.List[LucentBranding.RgbaImage]'
                foreach ($entry in $derivative.images) { $images.Add((& $render $entry.width $entry.height $entry.artwork)) }
                $files[$derivative.path] = [LucentBranding.Ico]::Write($images, 256)
            }
            default { throw "Unknown derivative format '$($derivative.format)'." }
        }
    }
    return $files
}

function New-BrandingDocument {
    # branding.json ("lucent.branding" v1): the master, what the analysis found, the method, every
    # derivative with its role, pixel size and composition.
    param([Parameter(Mandatory)]$Master, [Parameter(Mandatory)][object[]]$Recipe)
    $analysis = $Master.Analysis
    return [ordered]@{
        schema = $script:BrandingSchema
        schema_version = $script:BrandingSchemaVersion
        master = [ordered]@{
            path = $script:MasterPath
            format = 'png'
            width = $analysis.Width
            height = $analysis.Height
            size = $Master.Bytes.Length
            sha256 = (Get-BytesSha256 $Master.Bytes)
            chunks = @($Master.Png.Chunks | ForEach-Object { $_.Type })
            provenance = $script:ProvenanceDocument }
        analysis = [ordered]@{
            noise_alpha = $analysis.NoiseAlpha
            cleared_pixels = $analysis.ClearedPixels
            faint_components = $analysis.FaintComponents
            faint_pixels = $analysis.FaintPixels
            content_box = [ordered]@{ left = $analysis.ContentLeft; top = $analysis.ContentTop; right = $analysis.ContentRight; bottom = $analysis.ContentBottom }
            solid_alpha = $analysis.SolidAlpha
            interior_pixels = $analysis.InteriorPixels }
        method = (Get-BrandingMethod)
        derivatives = $Recipe
    }
}

function New-BrandingManifest {
    # manifest.json ("lucent.asset-manifest" v1, content "lucent.branding" v1) of these files.
    param([Parameter(Mandatory)][string]$Version, [Parameter(Mandatory)][System.Collections.IDictionary]$Files)
    $paths = [string[]]@($Files.Keys)
    [Array]::Sort($paths, [System.StringComparer]::Ordinal)
    return (ConvertTo-CanonicalJson ([ordered]@{
        schema = $script:ManifestSchema
        schema_version = $script:ManifestSchemaVersion
        id = $script:AssetId
        version = $Version
        minimum_app_version = $script:MinimumAppVersion
        content = [ordered]@{ schema = $script:BrandingSchema; schema_version = $script:BrandingSchemaVersion }
        files = @($paths | ForEach-Object { [ordered]@{ path = $_; size = [long]$Files[$_].Length; sha256 = (Get-BytesSha256 $Files[$_]) } })
    }))
}

function ConvertTo-NativeRelativePath([string]$Path) { return $Path.Replace('/', '\') }

function Test-BrandingPath([string]$Path) {
    # A manifest path of the asset: relative, lower-case segments of letters, digits, '.', '_' and '-'
    # that start and end with a letter or digit (so never '.' or '..'), separated by '/': no drive, no
    # backslash, nothing that leaves the folder (W12 review BRD-01).
    return [bool]($Path -cmatch '^[a-z0-9]([a-z0-9._-]*[a-z0-9])?(/[a-z0-9]([a-z0-9._-]*[a-z0-9])?)*$')
}

function New-BrandingProblemsException([string]$Message, [string[]]$Problems) {
    # One exception for every problem found, which also carries them one by one (Data['Problems']), so
    # check mode reports and counts each (W12 review BRD-05).
    $exception = New-Object System.InvalidOperationException ($Message + "`n  " + ($Problems -join "`n  "))
    $exception.Data['Problems'] = [string[]]$Problems
    return $exception
}

# --- Reading and checking the asset ------------------------------------------------------------------

function Read-BrandingAsset {
    <#
    .SYNOPSIS
        Reads assets\branding: manifest.json must be a "lucent.asset-manifest" version 1 for the asset
        "branding" with content "lucent.branding" version 1 (another schema or version is refused, never
        guessed), every file it lists must have its size and SHA-256, and branding.json must be
        "lucent.branding" version 1. Returns the version, the master and the derivatives with their
        absolute paths, by role. Throws with every problem found.
    #>
    param([Parameter(Mandatory)][string]$Root)
    $root = [System.IO.Path]::GetFullPath($Root)
    $problems = New-Object System.Collections.Generic.List[string]
    $manifestPath = Join-Path $root $script:ManifestName
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) { throw "$manifestPath is missing." }
    $manifestText = [System.IO.File]::ReadAllText($manifestPath)
    try { $manifest = $manifestText | ConvertFrom-Json } catch { throw "$manifestPath is not JSON: $($_.Exception.Message)" }
    if ((Get-Property $manifest 'schema') -cne $script:ManifestSchema) { throw "$manifestPath is not a '$($script:ManifestSchema)' (schema '$(Get-Property $manifest 'schema')')." }
    if ((Get-Property $manifest 'schema_version') -ne $script:ManifestSchemaVersion) {
        throw "$manifestPath has schema_version $(Get-Property $manifest 'schema_version'); this tool reads version $($script:ManifestSchemaVersion) only."
    }
    foreach ($problem in (Test-ExactMembers $manifest @('schema', 'schema_version', 'id', 'version', 'minimum_app_version', 'content', 'files') 'manifest.json')) { $problems.Add($problem) }
    $content = Get-Property $manifest 'content'
    if ((Get-Property $manifest 'id') -cne $script:AssetId) { $problems.Add("manifest.json names the asset '$(Get-Property $manifest 'id')', not '$($script:AssetId)'") }
    if (-not (Test-SemanticVersion ([string](Get-Property $manifest 'version')))) { $problems.Add("manifest.json's version '$(Get-Property $manifest 'version')' is not a Semantic Version") }
    if (-not (Test-SemanticVersion ([string](Get-Property $manifest 'minimum_app_version')))) { $problems.Add("manifest.json's minimum_app_version is not a Semantic Version") }
    if ((Get-Property $content 'schema') -cne $script:BrandingSchema -or (Get-Property $content 'schema_version') -ne $script:BrandingSchemaVersion) {
        throw "manifest.json's content is '$(Get-Property $content 'schema')' version $(Get-Property $content 'schema_version'); this tool reads '$($script:BrandingSchema)' version $($script:BrandingSchemaVersion) only."
    }
    $listed = [ordered]@{}
    $missing = @{}
    foreach ($entry in @(Get-Property $manifest 'files')) {
        foreach ($problem in (Test-ExactMembers $entry @('path', 'size', 'sha256') 'a manifest file entry')) { $problems.Add($problem) }
        $path = [string](Get-Property $entry 'path')
        if (-not (Test-BrandingPath $path)) { $problems.Add("manifest.json lists the invalid path '$path'"); continue }
        if ($listed.Contains($path) -or $missing.ContainsKey($path)) { $problems.Add("manifest.json lists '$path' twice"); continue }
        if ([string](Get-Property $entry 'sha256') -cnotmatch '^[0-9a-f]{64}$') { $problems.Add("manifest.json's sha256 of '$path' is not 64 lower-case hex digits") }
        $full = Join-Path $root (ConvertTo-NativeRelativePath $path)
        # Listed but missing: recorded, so the cross-check with branding.json below does not call it unlisted.
        if (-not (Test-Path -LiteralPath $full -PathType Leaf)) { $problems.Add("$path is listed but missing"); $missing[$path] = $true; continue }
        $bytes = [System.IO.File]::ReadAllBytes($full)
        if ($bytes.Length -ne [long](Get-Property $entry 'size')) { $problems.Add("$path is $($bytes.Length) bytes, manifest.json says $(Get-Property $entry 'size')") }
        elseif ((Get-BytesSha256 $bytes) -cne [string](Get-Property $entry 'sha256')) { $problems.Add("$path does not have the SHA-256 manifest.json lists (changed by hand?)") }
        $listed[$path] = $full
    }
    if ($listed.Count -eq 0 -and $missing.Count -eq 0) { $problems.Add('manifest.json lists no files') }
    # -Force: hidden and system files too, as the C++ check (std::filesystem) sees them; git ignores the
    # attribute, so a hidden file would otherwise pass here and fail on a fresh clone (W12 review BRD-04).
    foreach ($file in (Get-ChildItem -LiteralPath $root -Recurse -File -Force)) {
        $relative = $file.FullName.Substring($root.TrimEnd('\').Length + 1).Replace('\', '/')
        if ($script:UnlistedFiles -ccontains $relative) { continue }
        if (-not $listed.Contains($relative)) {
            $hidden = if ($file.Attributes -band ([System.IO.FileAttributes]::Hidden -bor [System.IO.FileAttributes]::System)) { ', a hidden or system file' } else { '' }
            $problems.Add("$relative is in the folder but not in manifest.json (left over, or added by hand$hidden)")
        }
    }
    if (-not $listed.Contains($script:BrandingDocument)) {
        if ($missing.ContainsKey($script:BrandingDocument) -and $problems.Count -gt 0) { throw (New-BrandingProblemsException "The branding asset in $root is not valid:" $problems.ToArray()) }
        throw "manifest.json does not list $($script:BrandingDocument)$(if ($problems.Count) { ': ' + ($problems -join '; ') })."
    }
    $documentPath = $listed[$script:BrandingDocument]
    try { $document = [System.IO.File]::ReadAllText($documentPath) | ConvertFrom-Json } catch { throw "$documentPath is not JSON: $($_.Exception.Message)" }
    if ((Get-Property $document 'schema') -cne $script:BrandingSchema -or (Get-Property $document 'schema_version') -ne $script:BrandingSchemaVersion) {
        throw "$documentPath is '$(Get-Property $document 'schema')' version $(Get-Property $document 'schema_version'); this tool reads '$($script:BrandingSchema)' version $($script:BrandingSchemaVersion) only."
    }
    $master = Get-Property $document 'master'
    if (-not $master -or -not $listed.Contains([string](Get-Property $master 'path'))) { $problems.Add('branding.json names a master that manifest.json does not list') }
    $derivatives = New-Object System.Collections.Generic.List[object]
    foreach ($derivative in @(Get-Property $document 'derivatives')) {
        $path = [string](Get-Property $derivative 'path')
        if (-not $listed.Contains($path)) {
            if (-not $missing.ContainsKey($path)) { $problems.Add("branding.json names $path, which manifest.json does not list") }
            continue
        }
        $derivatives.Add([pscustomobject]@{
            Path = $path; FullPath = $listed[$path]; Role = [string](Get-Property $derivative 'role'); Format = [string](Get-Property $derivative 'format')
            Width = Get-Property $derivative 'width'; Height = Get-Property $derivative 'height'; Images = Get-Property $derivative 'images' })
    }
    if ($problems.Count -gt 0) { throw (New-BrandingProblemsException "The branding asset in $root is not valid:" $problems.ToArray()) }
    return [pscustomobject]@{
        Root = $root; Version = [string]$manifest.version; ManifestText = $manifestText
        ManifestSha256 = (Get-BytesSha256 ([System.IO.File]::ReadAllBytes($manifestPath)))
        Master = [pscustomobject]@{ Path = [string]$master.path; FullPath = $listed[[string]$master.path]; Sha256 = [string]$master.sha256 }
        Document = $document; Derivatives = $derivatives.ToArray(); Files = $listed }
}

function Get-BrandingFiles {
    # The absolute paths of the derivatives of one role, in branding.json's order.
    param([Parameter(Mandatory)]$Asset, [Parameter(Mandatory)][string]$Role)
    $files = @($Asset.Derivatives | Where-Object { $_.Role -ceq $Role } | ForEach-Object { $_.FullPath })
    if ($files.Count -eq 0) { throw "The branding asset has no derivative with the role '$Role'." }
    return $files
}

function Invoke-BrandingGenerator {
    <#
    .SYNOPSIS
        Renders every derivative from the master and writes the asset, or with -Check compares the
        committed asset with a fresh rendering of its own master.
    .DESCRIPTION
        Writing: -MasterFile (optional) is validated and copied byte for byte to master\lucent-logo.png;
        provenance.json must already describe that master (Test-BrandingProvenance); every derivative,
        branding.json and manifest.json are written (files whose bytes do not change are left alone),
        and files that the previous manifest listed and the recipe no longer makes are deleted. When
        anything changes, -Version must give the new branding version, higher than the current one by
        Semantic Versioning precedence; when nothing changes, the version stays.
        Checking: the folder must hold exactly the files the manifest lists (and README.md), each with
        its size and SHA-256 (Read-BrandingAsset), provenance.json must describe the master, and a
        rendering of the committed master must reproduce every derivative, branding.json and
        manifest.json byte for byte. A difference names the file and how many pixels differ (a stale or
        hand-edited derivative).
        Returns a result: Errors (empty when the check passed), Written, Removed, Version.
    #>
    param(
        [Parameter(Mandatory)][string]$Root,
        [switch]$Check,
        [string]$Version,
        [string]$MasterFile)
    Initialize-BrandingType
    $root = [System.IO.Path]::GetFullPath($Root)
    $errors = New-Object System.Collections.Generic.List[string]
    $result = [pscustomobject]@{ Root = $root; Errors = $errors; Written = @(); Removed = @(); Version = $null; Unchanged = $false; Master = $null }
    if ($Check -and ($Version -or $MasterFile)) { throw '-Check takes neither -Version nor -MasterFile.' }
    if ($Version -and -not (Test-SemanticVersion $Version)) { throw "-Version '$Version' is not a Semantic Version (MAJOR.MINOR.PATCH)." }
    $masterPath = Join-Path $root (ConvertTo-NativeRelativePath $script:MasterPath)
    $manifestPath = Join-Path $root $script:ManifestName

    if ($Check) {
        try { $asset = Read-BrandingAsset -Root $root }
        catch {
            # Each problem on its own, so the count is the number of problems printed (BRD-05).
            $found = $_.Exception.Data['Problems']
            if ($found) { foreach ($problem in $found) { $errors.Add([string]$problem) } } else { $errors.Add($_.Exception.Message) }
            return $result
        }
        $result.Version = $asset.Version
        $master = Read-BrandingMaster -Bytes ([System.IO.File]::ReadAllBytes($masterPath))
        $result.Master = $master
        $recipe = Get-BrandingRecipe
        $files = Invoke-BrandingRendering -Master $master -Recipe $recipe
        $files[$script:BrandingDocument] = Get-AsciiBytes (ConvertTo-CanonicalJson (New-BrandingDocument -Master $master -Recipe $recipe))
        $files[$script:MasterPath] = $master.Bytes
        $provenancePath = Join-Path $root $script:ProvenanceDocument
        foreach ($problem in (Test-BrandingProvenance -Path $provenancePath -MasterBytes $master.Bytes)) { $errors.Add($problem) }
        if (Test-Path -LiteralPath $provenancePath -PathType Leaf) { $files[$script:ProvenanceDocument] = [System.IO.File]::ReadAllBytes($provenancePath) }
        $expectedManifest = New-BrandingManifest -Version $asset.Version -Files $files
        foreach ($path in $files.Keys) {
            if (-not $asset.Files.Contains($path)) { $errors.Add("$path is missing: the recipe makes it, manifest.json does not list it (run the generator)"); continue }
            $committed = [System.IO.File]::ReadAllBytes($asset.Files[$path])
            if ([System.Linq.Enumerable]::SequenceEqual([byte[]]$committed, [byte[]]$files[$path])) { continue }
            $detail = ''
            try {
                if ($path -like '*.png') {
                    $largest = 0
                    $count = ([LucentBranding.Png]::Read($committed)).Image.CountDifferences(([LucentBranding.Png]::Read($files[$path])).Image, [ref]$largest)
                    $detail = if ($count -eq 0) { ': the same pixels, other bytes' } else { ": $count pixel(s) differ, by up to $largest" }
                }
            }
            catch { $detail = ": $(Get-ErrorText $_)" }
            $errors.Add("$path is not what the generator makes from the master (stale or edited by hand)$detail")
        }
        foreach ($path in $asset.Files.Keys) {
            if (-not $files.Contains($path)) { $errors.Add("$path is listed in manifest.json, but the recipe does not make it (stale)") }
        }
        if ($asset.ManifestText -cne $expectedManifest) { $errors.Add('manifest.json is not the canonical manifest of the generated files (stale, or edited by hand)') }
        return $result
    }

    # Writing.
    $existing = $null
    if (Test-Path -LiteralPath $manifestPath -PathType Leaf) {
        $existing = [System.IO.File]::ReadAllText($manifestPath) | ConvertFrom-Json
        if ($existing.schema -cne $script:ManifestSchema -or $existing.schema_version -ne $script:ManifestSchemaVersion -or
            $existing.content.schema -cne $script:BrandingSchema -or $existing.content.schema_version -ne $script:BrandingSchemaVersion) {
            throw "$manifestPath is '$($existing.schema)' v$($existing.schema_version) with content '$($existing.content.schema)' v$($existing.content.schema_version); this generator writes '$($script:ManifestSchema)' v$($script:ManifestSchemaVersion) with '$($script:BrandingSchema)' v$($script:BrandingSchemaVersion) and does not overwrite another schema."
        }
        # The previous manifest decides what is deleted: every path must be one of the asset's own, or
        # nothing is written (a tampered manifest could name a file outside the folder; W12 review BRD-01).
        $invalid = @(@($existing.files) | ForEach-Object { [string](Get-Property $_ 'path') } | Where-Object { -not (Test-BrandingPath $_) })
        if ($invalid.Count -gt 0) {
            throw "$manifestPath lists the invalid path(s) $(($invalid | ForEach-Object { "'$_'" }) -join ', '): nothing was written or deleted. Restore manifest.json from git (git checkout -- assets/branding/manifest.json), then generate again."
        }
    }
    $masterBytes = if ($MasterFile) { [System.IO.File]::ReadAllBytes([System.IO.Path]::GetFullPath($MasterFile)) }
                   elseif (Test-Path -LiteralPath $masterPath -PathType Leaf) { [System.IO.File]::ReadAllBytes($masterPath) }
                   else { throw "There is no master: $masterPath is missing (pass -MasterFile)." }
    $master = Read-BrandingMaster -Bytes $masterBytes
    $result.Master = $master
    # The master's provenance is recorded by hand before anything is generated from it.
    $provenancePath = Join-Path $root $script:ProvenanceDocument
    $provenanceProblems = @(Test-BrandingProvenance -Path $provenancePath -MasterBytes $masterBytes)
    if ($provenanceProblems.Count -gt 0) { throw ($provenanceProblems -join "`n") }
    $recipe = Get-BrandingRecipe
    $files = Invoke-BrandingRendering -Master $master -Recipe $recipe
    $files[$script:BrandingDocument] = Get-AsciiBytes (ConvertTo-CanonicalJson (New-BrandingDocument -Master $master -Recipe $recipe))
    $files[$script:MasterPath] = $masterBytes
    $files[$script:ProvenanceDocument] = [System.IO.File]::ReadAllBytes($provenancePath)

    $unchanged = $false
    if ($existing) {
        $before = @{}
        foreach ($entry in @($existing.files)) { $before[$entry.path] = $entry.sha256 }
        $unchanged = $before.Count -eq $files.Count
        foreach ($path in $files.Keys) {
            if (-not $before.ContainsKey($path) -or $before[$path] -cne (Get-BytesSha256 $files[$path])) { $unchanged = $false; break }
        }
    }
    if ($unchanged) {
        if ($Version -and $Version -cne $existing.version) { throw "Nothing changed, so the branding version stays $($existing.version); -Version $Version is refused." }
        $Version = $existing.version
        $result.Unchanged = $true
    }
    elseif (-not $Version) {
        throw "The derivatives change$(if ($existing) { " (branding $($existing.version))" }): pass -Version with the new branding version (MAJOR for a removed or renamed role or file, MINOR for a new master or new derivatives, PATCH for the same master rendered differently, or for a change of provenance.json or of the generated record's wording only; ADR 0025)."
    }
    elseif ($existing -and (Compare-SemanticVersion $Version $existing.version) -le 0) {
        throw "-Version $Version is not higher than the current branding version $($existing.version)."
    }
    $result.Version = $Version
    $manifestText = New-BrandingManifest -Version $Version -Files $files

    $written = New-Object System.Collections.Generic.List[string]
    foreach ($path in $files.Keys) {
        $full = Join-Path $root (ConvertTo-NativeRelativePath $path)
        if ((Test-Path -LiteralPath $full -PathType Leaf) -and [System.Linq.Enumerable]::SequenceEqual([byte[]][System.IO.File]::ReadAllBytes($full), [byte[]]$files[$path])) { continue }
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $full) | Out-Null
        [System.IO.File]::WriteAllBytes($full, [byte[]]$files[$path])
        $written.Add($path)
    }
    $removed = New-Object System.Collections.Generic.List[string]
    if ($existing) {
        $inside = $root.TrimEnd('\') + '\'
        foreach ($entry in @($existing.files)) {
            if ($files.Contains($entry.path)) { continue }
            # Only a file inside the asset folder, and never its manifest or README (BRD-01).
            $full = [System.IO.Path]::GetFullPath((Join-Path $root (ConvertTo-NativeRelativePath $entry.path)))
            $leaf = $full.Substring([Math]::Min($full.Length, $inside.Length))
            if (-not $full.StartsWith($inside, [System.StringComparison]::OrdinalIgnoreCase) -or $script:UnlistedFiles -ccontains $leaf.Replace('\', '/')) {
                throw "manifest.json lists $($entry.path), which is not a file of the asset in $($root): it is not deleted. Restore manifest.json from git."
            }
            if (Test-Path -LiteralPath $full -PathType Leaf) { Remove-Item -LiteralPath $full -Force; $removed.Add($entry.path) }
        }
    }
    if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf) -or [System.IO.File]::ReadAllText($manifestPath) -cne $manifestText) {
        [System.IO.File]::WriteAllBytes($manifestPath, (Get-AsciiBytes $manifestText))
        $written.Add($script:ManifestName)
    }
    $result.Written = $written.ToArray()
    $result.Removed = $removed.ToArray()
    return $result
}

function Get-BrandingRemedy {
    <#
    .SYNOPSIS
        What to do about the problems check mode found, one line for each kind (W12 review BRD-05: one
        piece of advice for all of them sent a user with a deleted derivative to a -Version the generator
        then refused).
    #>
    param([AllowEmptyCollection()][string[]]$Problems = @())
    $advice = New-Object System.Collections.Generic.List[string]
    foreach ($problem in $Problems) {
        $text = $null
        if ($problem -match 'provenance\.json') {
            $text = 'provenance.json does not describe the master, or was edited: record the master''s provenance (assets\branding\README.md, "Replacing the logo"; the licence under "Licence"), then run tools\branding\Update-Branding.ps1 -Version <a higher version> (PATCH when only provenance.json changed).'
        }
        elseif ($problem -match 'is in the folder but not in manifest\.json') {
            $text = 'A file that manifest.json does not list is in the folder: delete it (a hidden one too: dir /a). A new derivative belongs in the recipe (Get-BrandingRecipe) with a new MINOR version.'
        }
        elseif ($problem -match 'is listed but missing|does not have the SHA-256 manifest\.json lists|bytes, manifest\.json says|is not what the generator makes from the master|is missing: the recipe makes it') {
            $text = 'A generated file is missing or was changed: run tools\branding\Update-Branding.ps1 without -Version to write it again (the version stays when the master, provenance.json and the recipe did not change).'
        }
        elseif ($problem -match 'but the recipe does not make it|is not the canonical manifest') {
            $text = 'manifest.json is not the generated one: run tools\branding\Update-Branding.ps1, with -Version <a higher version> when the master, provenance.json or the recipe changed (ADR 0025).'
        }
        elseif ($problem -match 'invalid path|twice|schema_version|this tool reads|unknown member|has no member|not JSON|names a master') {
            $text = 'manifest.json or branding.json is damaged or of another version: restore the folder from git (git checkout -- assets/branding), then check again.'
        }
        if ($text -and -not $advice.Contains($text)) { $advice.Add($text) }
    }
    if ($advice.Count -eq 0 -and @($Problems).Count -gt 0) {
        $advice.Add('Run tools\branding\Update-Branding.ps1; add -Version only when the master, provenance.json or the recipe changed (ADR 0025).')
    }
    return $advice.ToArray()
}

# --- Icon checks ---------------------------------------------------------------------------------------

function Initialize-IconApiType {
    Initialize-BrandingType
    if ('LucentBranding.IconApi' -as [type]) { return }
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
namespace LucentBranding {
    // Windows' own reading of one icon image (CreateIconFromResourceEx, as LoadImage does).
    public static class IconApi {
        [DllImport("user32.dll", SetLastError = true)]
        private static extern IntPtr CreateIconFromResourceEx(byte[] bits, uint size, bool icon, uint version, int width, int height, uint flags);
        [DllImport("user32.dll", SetLastError = true)]
        private static extern bool DestroyIcon(IntPtr icon);
        public static bool WindowsReads(byte[] data, int width, int height) {
            IntPtr icon = CreateIconFromResourceEx(data, (uint)data.Length, true, 0x00030000, width, height, 0);
            if (icon == IntPtr.Zero) { return false; }
            DestroyIcon(icon);
            return true;
        }
    }
}
'@ -Language CSharp
}

function Test-IconFile {
    <#
    .SYNOPSIS
        The structure of a branding .ico: exactly the expected sizes, every image square, one plane of
        32 bits with a colour count of 0, PNG from 256 px and BMP below (BITMAPINFOHEADER, AND mask that
        agrees with the alpha), alpha that is used (transparent corners, opaque artwork), and every
        image read by Windows (CreateIconFromResourceEx). Returns the problems (empty when valid).
    #>
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][int[]]$ExpectedSizes)
    Initialize-IconApiType
    $problems = New-Object System.Collections.Generic.List[string]
    try { $images = [LucentBranding.Ico]::Read([System.IO.File]::ReadAllBytes($Path)) }
    catch { $problems.Add("$Path is not a valid icon file: $(Get-ErrorText $_)"); return $problems.ToArray() }
    $sizes = @($images | ForEach-Object { $_.Width })
    if (($sizes -join ',') -ne ($ExpectedSizes -join ',')) { $problems.Add("$Path holds the sizes $($sizes -join ', '), expected $($ExpectedSizes -join ', ')") }
    foreach ($image in $images) {
        $name = "the $($image.Width) px image of $(Split-Path -Leaf $Path)"
        if ($image.Width -ne $image.Height) { $problems.Add("$name is not square") }
        if ($image.BitCount -ne 32 -or $image.Planes -ne 1 -or $image.ColorCount -ne 0) { $problems.Add("$name is not one plane of 32 bits without a palette (planes $($image.Planes), bits $($image.BitCount), colours $($image.ColorCount))") }
        if ($image.IsPng -ne ($image.Width -ge 256)) { $problems.Add("$name is $(if ($image.IsPng) { 'PNG' } else { 'BMP' }); images below 256 px are 32-bit BMPs and the 256 px image is PNG") }
        # Alpha in use: the corners are transparent (every icon keeps a transparent margin), and some
        # pixel is opaque (the artwork; not necessarily the middle one, which a future logo may leave open).
        $pixels = $image.Image
        $last = $pixels.Width - 1
        $corners = $pixels.Alpha(0, 0) + $pixels.Alpha($last, 0) + $pixels.Alpha(0, $last) + $pixels.Alpha($last, $last)
        $opaque = 0
        for ($i = 3; $i -lt $pixels.Pixels.Length; $i += 4) { if ($pixels.Pixels[$i] -eq 255) { $opaque++ } }
        if ($corners -ne 0 -or $opaque -eq 0) { $problems.Add("$name does not use its alpha channel (corner alpha sum $corners, $opaque opaque pixels)") }
        if (-not [LucentBranding.IconApi]::WindowsReads($image.Data, $image.Width, $image.Height)) { $problems.Add("Windows does not read $name (CreateIconFromResourceEx)") }
    }
    return $problems.ToArray()
}

function Get-ProgramIconGroups {
    param([Parameter(Mandatory)][string]$Path)
    Initialize-BrandingType
    return @([LucentBranding.ProgramIcons]::Read([System.IO.Path]::GetFullPath($Path)))
}

function Test-ProgramIcon {
    <#
    .SYNOPSIS
        Whether an executable carries the branding icon: an icon group with exactly the images of the
        .ico (same count, sizes and depth; each image byte for byte, or with the same pixels when the
        program's builder re-encoded it). Returns a result with the group found, how it matched, and
        the problems (empty when it carries the icon).
    .PARAMETER GroupName
        The group must have this name (IDI_ICON1 for Lucent's programs, which Qt's Windows platform
        plugin loads by name).
    .PARAMETER First
        The group must be the program's first icon group: the one Explorer, shortcuts, the taskbar and
        "<program>,0" references (UninstallDisplayIcon, DefaultIcon) show.
    #>
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][string]$Icon, [string]$GroupName, [switch]$First)
    Initialize-BrandingType
    $problems = New-Object System.Collections.Generic.List[string]
    $leaf = Split-Path -Leaf $Path
    $expected = [LucentBranding.Ico]::Read([System.IO.File]::ReadAllBytes($Icon))
    $groups = @()
    try { $groups = @(Get-ProgramIconGroups -Path $Path) }
    catch {
        $problems.Add("The icon groups of $leaf cannot be read: $(Get-ErrorText $_)")
        return [pscustomobject]@{ Path = $Path; Group = $null; Match = $null; Groups = @(); Problems = $problems.ToArray() }
    }
    $candidates = @($groups | Where-Object { -not $GroupName -or $_.Name -ceq $GroupName })
    if ($First) { $candidates = @($groups | Select-Object -First 1) }
    $match = $null
    $how = $null
    $notes = New-Object System.Collections.Generic.List[string]
    foreach ($group in $candidates) {
        $differences = New-Object System.Collections.Generic.List[string]
        if ($GroupName -and $group.Name -cne $GroupName) { $differences.Add("it is named $($group.Name), not $GroupName") }
        if ($group.Images.Count -ne $expected.Count) { $differences.Add("$($group.Images.Count) images instead of $($expected.Count)") }
        $reencoded = 0
        foreach ($image in $expected) {
            $same = @($group.Images | Where-Object { $_.Width -eq $image.Width -and $_.Height -eq $image.Height -and $_.BitCount -eq $image.BitCount })
            if ($same.Count -ne 1) { $differences.Add("$(if ($same.Count) { 'more than one' } else { 'no' }) $($image.Width) px 32-bit image"); continue }
            if ([System.Linq.Enumerable]::SequenceEqual([byte[]]$same[0].Data, [byte[]]$image.Data)) { continue }
            $largest = 0
            if (-not $same[0].Image -or $same[0].Image.CountDifferences($image.Image, [ref]$largest) -ne 0) {
                $differences.Add("its $($image.Width) px image differs from the branding icon's$(if ($same[0].DecodeError) { " ($($same[0].DecodeError))" })")
            }
            else { $reencoded++ }
        }
        if ($differences.Count -eq 0) {
            $match = $group
            $how = if ($reencoded) { "every image with the branding icon's pixels ($reencoded re-encoded)" } else { "every image byte for byte" }
            break
        }
        $notes.Add("group $($group.Name): $($differences -join '; ')")
    }
    if (-not $match) {
        $groupList = if ($groups.Count) { ($groups | ForEach-Object Name) -join ', ' } else { 'none' }
        if ($candidates.Count -eq 0) { $problems.Add("$leaf has no icon group$(if ($GroupName) { " named $GroupName" }) (groups: $groupList)") }
        elseif ($First) { $problems.Add("The first icon group of $leaf (the one Windows shows for it) is not the branding icon: $($notes -join ' | ') (groups: $groupList)") }
        else { $problems.Add("$leaf does not carry the branding icon: $($notes -join ' | ')") }
    }
    return [pscustomobject]@{ Path = $Path; Group = $match; Match = $how; Groups = $groups; Problems = $problems.ToArray() }
}

# --- Evidence ------------------------------------------------------------------------------------------

function New-BrandingContactSheet {
    <#
    .SYNOPSIS
        Writes a contact sheet PNG: the master (scaled down) and every derivative of the asset (every ICO
        image too) at 1:1 and magnified with nearest-neighbour sampling, on a light, a mid-grey and a dark
        background. Evidence for review; GDI+ draws it, so it is not a derivative.
    #>
    param([Parameter(Mandatory)][string]$Root, [Parameter(Mandatory)][string]$Path)
    Initialize-BrandingEvidenceType
    $asset = Read-BrandingAsset -Root $Root
    $sheet = New-Object LucentBrandingEvidence.ContactSheet
    $master = ([LucentBranding.Png]::Read([System.IO.File]::ReadAllBytes($asset.Master.FullPath))).Image
    $sheet.AddMaster("master ($($asset.Master.Path), $($master.Width) px, scaled; magnified: its artwork's left edge at 8x)", $master.Width, $master.Height, $master.Pixels)
    foreach ($derivative in $asset.Derivatives) {
        $bytes = [System.IO.File]::ReadAllBytes($derivative.FullPath)
        if ($derivative.Format -eq 'ico') {
            foreach ($image in [LucentBranding.Ico]::Read($bytes)) {
                $sheet.Add("$($derivative.Path): $($image.Width) px $(if ($image.IsPng) { 'PNG' } else { '32-bit BMP' })", $image.Image.Width, $image.Image.Height, $image.Image.Pixels, $derivative.Role)
            }
        }
        else {
            $image = ([LucentBranding.Png]::Read($bytes)).Image
            $sheet.Add($derivative.Path, $image.Width, $image.Height, $image.Pixels, $derivative.Role)
        }
    }
    $sheet.Save([System.IO.Path]::GetFullPath($Path), "Lucent branding $($asset.Version): every derivative at 1:1 and magnified (nearest neighbour) on light, mid-grey and dark")
}

function New-ProgramIconSheet {
    <#
    .SYNOPSIS
        Writes a PNG of each executable's icon as Windows draws it (the group's best image for the size,
        LookupIconIdFromDirectoryEx, CreateIconFromResourceEx, DrawIconEx on a 32-bit bitmap) at the
        given sizes on light, grey and dark backgrounds.
    #>
    param([Parameter(Mandatory)][string[]]$Executables, [Parameter(Mandatory)][string]$Path, [int[]]$Sizes = @(16, 32, 48, 256))
    Initialize-BrandingEvidenceType
    $sheet = New-Object LucentBrandingEvidence.ProgramIconSheet
    foreach ($executable in $Executables) {
        $groups = @(Get-ProgramIconGroups -Path $executable)
        if ($groups.Count -eq 0) { throw "$executable has no icon group." }
        $group = $groups[0]
        $ids = [int[]]@($group.Images | ForEach-Object { $_.ResourceId })
        $data = New-Object 'byte[][]' $group.Images.Count
        for ($i = 0; $i -lt $group.Images.Count; $i++) { $data[$i] = $group.Images[$i].Data }
        $sheet.Add((Split-Path -Leaf $executable) + " (icon group $($group.Name), $($group.Images.Count) images)", $group.Directory, $ids, $data)
    }
    $sheet.Save([System.IO.Path]::GetFullPath($Path), $Sizes)
}

Export-ModuleMember -Function Initialize-BrandingType, Get-IconPadding, Get-BrandingRecipe, ConvertTo-CanonicalJson, Compare-SemanticVersion,
    Test-SemanticVersion, Read-BrandingMaster, Test-BrandingProvenance, Invoke-BrandingRendering, Read-BrandingAsset, Get-BrandingFiles,
    Invoke-BrandingGenerator, Test-IconFile, Get-ProgramIconGroups, Test-ProgramIcon, New-BrandingContactSheet, New-ProgramIconSheet, Get-BrandingRemedy, Test-BrandingPath,
    Get-BytesSha256
