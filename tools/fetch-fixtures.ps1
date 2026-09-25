<#
.SYNOPSIS
    Downloads Lucent's camera test fixtures into fixtures/cache/ and verifies their SHA-256 digests.

.DESCRIPTION
    Reads fixtures/manifest.json (schema "lucent.fixture-manifest", version 1) and makes sure that
    fixtures/cache/<id>/<file_name> exists and matches the manifest for every selected entry.

    - A file is downloaded to a temporary file in its destination directory and verified (size and
      SHA-256) BEFORE it is moved into place, so a failed or corrupted download never replaces a file.
    - A cached file is re-hashed; it is skipped when it verifies and downloaded again when it does not.
      -Force downloads again regardless.
    - The entry's "url" is tried first, then its optional "mirror_url".
    - Entries without "sha256" are refused unless -Bootstrap is given. With -Bootstrap such a file is
      downloaded (or the cached copy is used), and its hash and size are printed as
          BOOTSTRAP id=<id> sha256=<hex> size_bytes=<n>
      for recording in the manifest. Bootstrapped files are NOT verified: record the hash, then run
      the script again without -Bootstrap.
    - Only CC0-1.0 entries and https URLs (http only for loopback hosts, used by tests) are accepted.

    Works in Windows PowerShell 5.1 and PowerShell 7.

    Exit status: 0 when every selected fixture is present and verified (or bootstrapped),
    1 when any selected fixture failed, 2 for usage or manifest errors.

.PARAMETER Tier
    smoke (default), qualification or all.

.PARAMETER Id
    Fixture ids to process, comma-separated or repeated. Without an explicit -Tier, ids are looked up
    in every tier; with an explicit -Tier other than "all", each id must belong to that tier.

.PARAMETER Force
    Download again even when a verified copy is cached.

.PARAMETER Bootstrap
    Accept entries without sha256 and print their computed hashes for recording.

.PARAMETER Manifest
    Manifest path. Default: <repository>/fixtures/manifest.json.

.PARAMETER CacheDir
    Cache directory. Default: the "cache" directory next to the manifest.

.PARAMETER TimeoutSec
    Timeout of each HTTP request in seconds. Default: 600.

.EXAMPLE
    powershell -NoProfile -File tools\fetch-fixtures.ps1
    Fetches and verifies the smoke tier.

.EXAMPLE
    powershell -NoProfile -File tools\fetch-fixtures.ps1 -Tier all

.EXAMPLE
    powershell -NoProfile -File tools\fetch-fixtures.ps1 -Id canon-eos-r5-cr3-raw,dji-fc4382-dng -Force
#>
[CmdletBinding()]
param(
    [ValidateSet('smoke', 'qualification', 'all')]
    [string]$Tier = 'smoke',
    [string[]]$Id = @(),
    [switch]$Force,
    [switch]$Bootstrap,
    [string]$Manifest,
    [string]$CacheDir,
    [ValidateRange(1, 86400)]
    [int]$TimeoutSec = 600
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'
# Invoke-WebRequest in Windows PowerShell 5.1 is dramatically slower while it renders progress.
$ProgressPreference = 'SilentlyContinue'

$SupportedSchemaVersion = 1
$ExitOk = 0
$ExitFailed = 1
$ExitUsage = 2
$UserAgent = 'Lucent-fetch-fixtures/1'
$IdPattern = '^[a-z0-9]+(-[a-z0-9]+)*$'
$FileNamePattern = '^[A-Za-z0-9_][A-Za-z0-9._()+-]*$'
$Sha256Pattern = '^[0-9a-f]{64}$'

function Write-Line([string]$Text) {
    [Console]::Out.WriteLine($Text)
}

function Write-ErrorLine([string]$Text) {
    [Console]::Error.WriteLine($Text)
}

function Exit-WithUsageError([string]$Message) {
    Write-ErrorLine "fetch-fixtures: error: $Message"
    exit $ExitUsage
}

function Get-OptionalProperty($Object, [string]$Name) {
    if ($null -eq $Object) { return $null }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

function Test-AllowedUrl([string]$Url) {
    $uri = $null
    if (-not [Uri]::TryCreate($Url, [UriKind]::Absolute, [ref]$uri)) { return $false }
    if ($uri.Scheme -eq 'https') { return $true }
    return ($uri.Scheme -eq 'http' -and $uri.IsLoopback)
}

function Resolve-UserPath([string]$Path) {
    # Resolves relative to PowerShell's current location (not the process working directory).
    return $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
}

function Format-Size([long]$Bytes) {
    return ('{0:N1} MB' -f ($Bytes / 1MB))
}

function Get-Sha256Hex([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

# Converts one manifest entry into a validated record; returns an error message string on failure.
function ConvertTo-FixtureRecord($Entry, [int]$Index) {
    $label = "fixtures[$Index]"
    $id = Get-OptionalProperty $Entry 'id'
    if ($id -isnot [string] -or $id -notmatch $IdPattern) {
        return "${label}: 'id' must match $IdPattern"
    }
    $label = "fixture '$id'"
    $tier = Get-OptionalProperty $Entry 'tier'
    if ($tier -cne 'smoke' -and $tier -cne 'qualification') {
        return "${label}: 'tier' must be 'smoke' or 'qualification'"
    }
    $license = Get-OptionalProperty $Entry 'license'
    if ($license -cne 'CC0-1.0') {
        return "${label}: 'license' is '$license'; only CC0-1.0 fixtures are allowed"
    }
    $fileName = Get-OptionalProperty $Entry 'file_name'
    if ($fileName -isnot [string] -or $fileName -notmatch $FileNamePattern) {
        return "${label}: 'file_name' must match $FileNamePattern"
    }
    $url = Get-OptionalProperty $Entry 'url'
    if ($url -isnot [string] -or -not (Test-AllowedUrl $url)) {
        return "${label}: 'url' must be an absolute https URL"
    }
    $mirrorUrl = Get-OptionalProperty $Entry 'mirror_url'
    if ($null -ne $mirrorUrl -and ($mirrorUrl -isnot [string] -or -not (Test-AllowedUrl $mirrorUrl))) {
        return "${label}: 'mirror_url' must be an absolute https URL when present"
    }
    $sha256 = Get-OptionalProperty $Entry 'sha256'
    if ($null -ne $sha256 -and ($sha256 -isnot [string] -or $sha256 -cnotmatch $Sha256Pattern)) {
        return "${label}: 'sha256' must be 64 lowercase hexadecimal digits"
    }
    $sizeBytes = Get-OptionalProperty $Entry 'size_bytes'
    if ($null -ne $sizeBytes) {
        if (($sizeBytes -isnot [int] -and $sizeBytes -isnot [long]) -or $sizeBytes -le 0) {
            return "${label}: 'size_bytes' must be a positive integer"
        }
        $sizeBytes = [long]$sizeBytes
    }
    return [pscustomobject]@{
        Id        = $id
        Tier      = $tier
        FileName  = $fileName
        Url       = $url
        MirrorUrl = $mirrorUrl
        Sha256    = $sha256
        SizeBytes = $sizeBytes
    }
}

# Returns $null when the file matches the record's size and hash, otherwise a description.
# Empty files and HTML/XML payloads are always rejected: raw.pixls.us answers an unknown sample id
# with HTTP 200 and an empty body, and no RAW container starts with '<'.
function Test-FixtureFile($Record, [string]$Path) {
    $length = (Get-Item -LiteralPath $Path).Length
    if ($length -eq 0) {
        return 'the server returned an empty file'
    }
    $stream = [IO.File]::OpenRead($Path)
    try {
        $head = New-Object byte[] ([Math]::Min(64, $length))
        [void]$stream.Read($head, 0, $head.Length)
    }
    finally {
        $stream.Dispose()
    }
    if ([Text.Encoding]::ASCII.GetString($head).TrimStart().StartsWith('<')) {
        return 'the server returned an HTML/XML page instead of a RAW file'
    }
    if ($null -ne $Record.SizeBytes -and $length -ne $Record.SizeBytes) {
        return "size is $length bytes but the manifest expects $($Record.SizeBytes)"
    }
    if ($null -ne $Record.Sha256) {
        $actual = Get-Sha256Hex $Path
        if ($actual -ne $Record.Sha256) {
            return "SHA-256 is $actual but the manifest expects $($Record.Sha256)"
        }
    }
    return $null
}

function Write-BootstrapLine($Record, [string]$Path) {
    $hash = Get-Sha256Hex $Path
    $length = (Get-Item -LiteralPath $Path).Length
    Write-Line "BOOTSTRAP id=$($Record.Id) sha256=$hash size_bytes=$length"
}

function Move-FileIntoPlace([string]$Source, [string]$Destination) {
    if (Test-Path -LiteralPath $Destination) {
        [IO.File]::Replace($Source, $Destination, [NullString]::Value)
    }
    else {
        [IO.File]::Move($Source, $Destination)
    }
}

# Downloads a record through a temporary file, verifies it, then moves it into place.
# Returns a status string; throws with every attempt's reason when no URL produced a valid file.
function Save-VerifiedFixture($Record, [string]$Directory, [string]$Target) {
    $urls = @($Record.Url)
    if ($Record.MirrorUrl) { $urls += $Record.MirrorUrl }
    $failures = @()
    foreach ($url in $urls) {
        $temp = Join-Path $Directory ('.{0}.{1}.partial' -f $Record.FileName, [guid]::NewGuid().ToString('N'))
        try {
            $watch = [Diagnostics.Stopwatch]::StartNew()
            try {
                Invoke-WebRequest -Uri $url -OutFile $temp -UseBasicParsing -TimeoutSec $TimeoutSec `
                    -MaximumRedirection 5 -UserAgent $UserAgent
            }
            catch {
                $failures += "download from $url failed: $($_.Exception.Message)"
                continue
            }
            if (-not (Test-Path -LiteralPath $temp -PathType Leaf)) {
                $failures += "download from $url produced no file"
                continue
            }
            $problem = Test-FixtureFile $Record $temp
            if ($null -ne $problem) {
                $failures += "file from $url rejected: $problem"
                continue
            }
            $length = (Get-Item -LiteralPath $temp).Length
            foreach ($failure in $failures) {
                # A working fallback must not hide a stale primary URL.
                Write-ErrorLine "fetch-fixtures: warning: $($Record.Id): $failure"
            }
            if ($null -eq $Record.Sha256) { Write-BootstrapLine $Record $temp }
            Move-FileIntoPlace $temp $Target
            $seconds = [Math]::Max($watch.Elapsed.TotalSeconds, 0.001)
            $how = if ($null -eq $Record.Sha256) { 'downloaded, bootstrap (unverified)' } else { 'downloaded, verified' }
            return ('{0}, {1} in {2:N1} s' -f $how, (Format-Size $length), $seconds)
        }
        finally {
            if (Test-Path -LiteralPath $temp) { Remove-Item -LiteralPath $temp -Force -ErrorAction SilentlyContinue }
        }
    }
    throw ($failures -join '; ')
}

# ---- Manifest ---------------------------------------------------------------------------------

$repoRoot = Split-Path -Parent $PSScriptRoot
$Manifest = if ($Manifest) { Resolve-UserPath $Manifest } else { Join-Path $repoRoot 'fixtures\manifest.json' }
$CacheDir = if ($CacheDir) { Resolve-UserPath $CacheDir } else { Join-Path (Split-Path -Parent $Manifest) 'cache' }

if (-not (Test-Path -LiteralPath $Manifest -PathType Leaf)) {
    Exit-WithUsageError "manifest not found: $Manifest"
}
try {
    $document = Get-Content -LiteralPath $Manifest -Raw -Encoding UTF8 | ConvertFrom-Json
}
catch {
    Exit-WithUsageError "manifest is not valid JSON ($Manifest): $($_.Exception.Message)"
}

if ((Get-OptionalProperty $document 'schema') -cne 'lucent.fixture-manifest') {
    Exit-WithUsageError "manifest schema must be 'lucent.fixture-manifest' ($Manifest)"
}
$schemaVersion = Get-OptionalProperty $document 'schema_version'
if (($schemaVersion -isnot [int] -and $schemaVersion -isnot [long]) -or $schemaVersion -lt 1) {
    Exit-WithUsageError "manifest schema_version must be a positive integer ($Manifest)"
}
if ($schemaVersion -gt $SupportedSchemaVersion) {
    Exit-WithUsageError ("manifest schema_version $schemaVersion is newer than this script supports " +
        "($SupportedSchemaVersion); update tools/fetch-fixtures.ps1")
}
$rawEntries = Get-OptionalProperty $document 'fixtures'
if ($null -eq $rawEntries) {
    Exit-WithUsageError "manifest has no 'fixtures' array ($Manifest)"
}

$records = New-Object System.Collections.Generic.List[object]
$seen = @{}
$index = 0
foreach ($entry in @($rawEntries)) {
    $record = ConvertTo-FixtureRecord $entry $index
    if ($record -is [string]) { Exit-WithUsageError $record }
    if ($seen.ContainsKey($record.Id)) { Exit-WithUsageError "fixture id '$($record.Id)' appears more than once" }
    $seen[$record.Id] = $true
    $records.Add($record)
    $index++
}

# ---- Selection --------------------------------------------------------------------------------

$requestedIds = @($Id | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$tierGiven = $PSBoundParameters.ContainsKey('Tier')
if ($requestedIds.Count -gt 0) {
    $unknown = @($requestedIds | Where-Object { -not $seen.ContainsKey($_) })
    if ($unknown.Count -gt 0) { Exit-WithUsageError "unknown fixture id(s): $($unknown -join ', ')" }
    $selected = @($records | Where-Object { $requestedIds -contains $_.Id })
    if ($tierGiven -and $Tier -ne 'all') {
        $outside = @($selected | Where-Object { $_.Tier -ne $Tier })
        if ($outside.Count -gt 0) {
            Exit-WithUsageError "fixture id(s) not in tier '$Tier': $(($outside | ForEach-Object { $_.Id }) -join ', ')"
        }
    }
}
else {
    $selected = @($records | Where-Object { $Tier -eq 'all' -or $_.Tier -eq $Tier })
}

Write-Line "fetch-fixtures: manifest $Manifest"
Write-Line "fetch-fixtures: cache    $CacheDir"
if ($selected.Count -eq 0) {
    Write-Line 'fetch-fixtures: no fixtures selected'
    exit $ExitOk
}

if ($PSVersionTable.PSVersion.Major -lt 6) {
    # .NET Framework may default to TLS 1.0/1.1, which current servers refuse.
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
}

# ---- Fetch and verify -------------------------------------------------------------------------

$failed = 0
foreach ($record in $selected) {
    $directory = Join-Path $CacheDir $record.Id
    $target = Join-Path $directory $record.FileName
    try {
        if ($null -eq $record.Sha256 -and -not $Bootstrap) {
            throw ('the manifest entry has no sha256, so the file cannot be verified; ' +
                'run with -Bootstrap to download it and print its hash for recording')
        }
        $status = $null
        if ((Test-Path -LiteralPath $target -PathType Leaf) -and -not $Force) {
            if ($null -eq $record.Sha256) {
                Write-BootstrapLine $record $target
                $status = 'cached, bootstrap (unverified)'
            }
            else {
                $problem = Test-FixtureFile $record $target
                if ($null -eq $problem) {
                    $status = "cached, verified, $(Format-Size (Get-Item -LiteralPath $target).Length)"
                }
                else {
                    Write-ErrorLine "fetch-fixtures: warning: $($record.Id): cached file does not verify ($problem); downloading again"
                }
            }
        }
        if ($null -eq $status) {
            New-Item -ItemType Directory -Force -Path $directory | Out-Null
            $status = Save-VerifiedFixture $record $directory $target
        }
        Write-Line ('[ ok ] {0,-48} {1}' -f $record.Id, $status)
    }
    catch {
        $failed++
        Write-Line ('[FAIL] {0,-48} {1}' -f $record.Id, $_.Exception.Message)
    }
}

$okCount = $selected.Count - $failed
Write-Line "fetch-fixtures: $($selected.Count) selected, $okCount ok, $failed failed"
if ($failed -gt 0) { exit $ExitFailed }
exit $ExitOk
