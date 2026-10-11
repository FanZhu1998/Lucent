<#
.SYNOPSIS
    The update channel's release tooling (W12, docs/adr/0022-updates-and-migrations.md): the
    publisher's signing key, the release manifest of a packaged installer and its detached signature,
    in exactly the formats Lucent reads (src/application/include/lucent/application/update_manifest.hpp).

.DESCRIPTION
    A release on an update channel is three files on an https server:

      <installer URL>                 Lucent-Setup-<version>-x64.exe (built by tools\package.ps1)
      <manifest URL>                  release-manifest.json  (lucent.release-manifest, version 1)
      <manifest URL>.sig              its detached signature (lucent.release-signature, version 1)

    Lucent fetches the manifest and the signature only when the photographer clicks "Check for
    updates", verifies the ECDSA P-256 signature over the manifest's exact bytes with a public key
    compiled into the build (LUCENT_UPDATE_PUBLIC_KEYS), and only then reads the manifest: version,
    channel, the installer's URL, size and SHA-256, the minimum Windows version and the release
    notes (plain text). The installer it downloads must have exactly that size and SHA-256 (and, when
    the build names LUCENT_UPDATE_PUBLISHER_THUMBPRINTS, a valid Authenticode signature by one of
    those certificates).

    Keys never live in the repository. New-LucentUpdateSigningKey creates a persisted, non-exportable
    ECDSA P-256 key in the current user's key storage provider (the Microsoft Software Key Storage
    Provider by default; a smart card or hardware token's provider works the same way); its public
    key (130 hex digits) is what a build pins. Signing uses that key in place (.NET's ECDsaCng over
    Windows CNG, the provider Lucent verifies with).

      Import-Module .\installer\scripts\LucentUpdate.psm1
      New-LucentUpdateSigningKey -Name 'Lucent release 2026'          # once; prints PublicKey, KeyId
      New-LucentReleaseManifest -Installer build\release\package\Lucent-Setup-0.2.0-x64.exe `
          -PackageManifest build\release\package\Lucent-Setup-0.2.0-x64.manifest.json `
          -InstallerUrl https://updates.example.org/lucent/Lucent-Setup-0.2.0-x64.exe -Channel stable `
          -ReleaseNotesFile notes.txt -OutFile release-manifest.json
      Protect-LucentReleaseManifest -Manifest release-manifest.json -KeyName 'Lucent release 2026'
      Test-LucentReleaseManifest -Manifest release-manifest.json -PublicKey <130 hex digits> `
          -Installer build\release\package\Lucent-Setup-0.2.0-x64.exe

    New-LucentReleaseManifest publishes only what tools\package.ps1 built and recorded for release
    (W12 review SEC-03, SEC-04): the Setup its package manifest names (size and SHA-256), with that
    manifest's version, which must also be the version in Setup's file name and its ProductVersion;
    signed for release (not a test signature, timestamped, Authenticode-valid by the recorded
    certificate, which the channel's Lucent.exe pins as a publisher); and whose own Lucent.exe
    checks the channel it is published on. Rehearsals and tests
    pass -AllowUnsignedInstaller, which skips only the signature and ProductVersion checks.
    Test-LucentReleaseManifest is at least as strict as Lucent's reader (SEC-06): every member, type
    and bound, and the canonical text New-LucentReleaseManifest writes, byte for byte.

    Publish the installer first, then the manifest's signature, then the manifest (a client that
    fetches between the last two sees a signature that does not match yet, and checks again later).

.NOTES
    Windows PowerShell 5.1 (.NET Framework 4.7.2 or later) and PowerShell 7. Keep this file ASCII.
    installer\tests\Test-ReleaseManifest.ps1 checks it with an ephemeral key (nothing is stored);
    src/application/tests/update_service_test.cpp verifies its output with Lucent's own reader.
#>

Set-StrictMode -Version 3.0

$script:ManifestSchema = 'lucent.release-manifest'
$script:SignatureSchema = 'lucent.release-signature'
$script:Algorithm = 'ecdsa-p256-sha256'
$script:SemVer = '^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-((?:0|[1-9]\d*|\d*[a-zA-Z-][0-9a-zA-Z-]*)(?:\.(?:0|[1-9]\d*|\d*[a-zA-Z-][0-9a-zA-Z-]*))*))?(?:\+([0-9a-zA-Z-]+(?:\.[0-9a-zA-Z-]+)*))?$'
$script:Utf8 = New-Object System.Text.UTF8Encoding($false)

function ConvertTo-HexString([byte[]]$Bytes) {
    return -join ($Bytes | ForEach-Object { $_.ToString('x2') })
}

function Get-Sha256Hex([byte[]]$Bytes) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try { return ConvertTo-HexString $sha.ComputeHash($Bytes) } finally { $sha.Dispose() }
}

# A JSON string literal escaped as Lucent's writer (nlohmann::json::dump) escapes it: quotation mark,
# reverse solidus and control characters only; everything else is written as UTF-8.
function ConvertTo-JsonText([string]$Text) {
    $builder = New-Object System.Text.StringBuilder
    [void]$builder.Append('"')
    foreach ($c in $Text.ToCharArray()) {
        $code = [int]$c
        switch ($code) {
            0x22 { [void]$builder.Append('\"'); continue }
            0x5C { [void]$builder.Append('\\'); continue }
            0x08 { [void]$builder.Append('\b'); continue }
            0x0C { [void]$builder.Append('\f'); continue }
            0x0A { [void]$builder.Append('\n'); continue }
            0x0D { [void]$builder.Append('\r'); continue }
            0x09 { [void]$builder.Append('\t'); continue }
            default {
                if ($code -lt 0x20) { [void]$builder.Append(('\u{0:x4}' -f $code)) }
                else { [void]$builder.Append($c) }
            }
        }
    }
    [void]$builder.Append('"')
    return $builder.ToString()
}

# The public key of a CNG key as Lucent pins it: the uncompressed point 04 || X || Y (130 hex
# digits), and its key id (SHA-256 of the 65 bytes).
function Get-PointOf([System.Security.Cryptography.CngKey]$Key) {
    if ($Key.Algorithm.Algorithm -ne 'ECDSA_P256') { throw "The key is a $($Key.Algorithm.Algorithm) key, not ECDSA P-256." }
    $blob = $Key.Export([System.Security.Cryptography.CngKeyBlobFormat]::EccPublicBlob)
    # BCRYPT_ECCKEY_BLOB: dwMagic, cbKey (32), then X and Y.
    if ($blob.Length -ne 72 -or [BitConverter]::ToUInt32($blob, 4) -ne 32) { throw 'Unexpected public key blob.' }
    $point = New-Object byte[] 65
    $point[0] = 4
    [Array]::Copy($blob, 8, $point, 1, 64)
    return ,$point
}

function Get-LucentUpdatePublicKey {
    <#
    .SYNOPSIS
        The public key (130 hex digits, for LUCENT_UPDATE_PUBLIC_KEYS) and key id of a signing key.
    #>
    [CmdletBinding(DefaultParameterSetName = 'Name')]
    param(
        [Parameter(Mandatory, ParameterSetName = 'Name')][string]$KeyName,
        [Parameter(ParameterSetName = 'Name')][string]$Provider = 'Microsoft Software Key Storage Provider',
        [Parameter(Mandatory, ParameterSetName = 'Key')][System.Security.Cryptography.CngKey]$Key
    )
    $opened = $null
    if ($PSCmdlet.ParameterSetName -eq 'Name') {
        $opened = [System.Security.Cryptography.CngKey]::Open($KeyName, (New-Object System.Security.Cryptography.CngProvider($Provider)))
        $Key = $opened
    }
    try {
        $point = Get-PointOf $Key
        return [pscustomobject]@{ PublicKey = (ConvertTo-HexString $point); KeyId = (Get-Sha256Hex $point) }
    }
    finally { if ($opened) { $opened.Dispose() } }
}

function New-LucentUpdateSigningKey {
    <#
    .SYNOPSIS
        Creates the publisher's persisted ECDSA P-256 signing key in the current user's key storage
        provider (never in the repository) and returns its public key and key id.
    .PARAMETER Exportable
        Allow the private key to be exported (for a backup). Off by default.
    #>
    [CmdletBinding(SupportsShouldProcess)]
    param(
        [Parameter(Mandatory)][string]$Name,
        [string]$Provider = 'Microsoft Software Key Storage Provider',
        [switch]$Exportable
    )
    if (-not $PSCmdlet.ShouldProcess("$Provider\$Name", 'Create an ECDSA P-256 signing key')) { return }
    $parameters = New-Object System.Security.Cryptography.CngKeyCreationParameters
    $parameters.Provider = New-Object System.Security.Cryptography.CngProvider($Provider)
    $parameters.KeyUsage = [System.Security.Cryptography.CngKeyUsages]::Signing
    $parameters.ExportPolicy = if ($Exportable) { [System.Security.Cryptography.CngExportPolicies]::AllowExport }
                               else { [System.Security.Cryptography.CngExportPolicies]::None }
    $key = [System.Security.Cryptography.CngKey]::Create([System.Security.Cryptography.CngAlgorithm]::ECDsaP256, $Name, $parameters)
    try { return Get-LucentUpdatePublicKey -Key $key } finally { $key.Dispose() }
}

function Test-ChannelName([string]$Name) {
    return $Name.Length -le 32 -and $Name -cmatch '^[a-z0-9]+(-[a-z0-9]+)*$'
}

function Test-HttpUrl([string]$Url) {
    if ($Url.Length -gt 2048 -or $Url -notmatch '^https?://[^/?#\s]') { return $false }
    foreach ($c in $Url.ToCharArray()) {
        $code = [int]$c
        if ($code -le 0x20 -or $code -ge 0x7F -or $c -eq '"' -or $c -eq '\' -or $c -eq '<' -or $c -eq '>') { return $false }
    }
    return $true
}

# The manifest's text in Lucent's canonical form: members in order, no whitespace (Lucent's
# write_release_manifest()). $Size is an integer, the rest strings.
function ConvertTo-ReleaseManifestText {
    param([string]$Channel, [string]$Version, [string]$Published, [string]$MinimumOs, [string]$InstallerUrl,
          [int64]$Size, [string]$Sha256, [string]$ReleaseNotes)
    return '{"schema":' + (ConvertTo-JsonText $script:ManifestSchema) + ',"schema_version":1,"product":"Lucent"' +
           ',"channel":' + (ConvertTo-JsonText $Channel) + ',"version":' + (ConvertTo-JsonText $Version) +
           ',"published":' + (ConvertTo-JsonText $Published) + ',"architecture":"x64"' +
           ',"minimum_os":' + (ConvertTo-JsonText $MinimumOs) +
           ',"installer":{"url":' + (ConvertTo-JsonText $InstallerUrl) + ',"size":' + $Size +
           ',"sha256":' + (ConvertTo-JsonText $Sha256) + '},"release_notes":' + (ConvertTo-JsonText $ReleaseNotes) + '}'
}

# "YYYY-MM-DDTHH:MM:SSZ" with plausible fields, as Lucent's is_utc_timestamp().
function Test-UtcTimestamp([string]$Text) {
    if ($Text -cnotmatch '^(\d{4})-(\d{2})-(\d{2})T(\d{2}):(\d{2}):(\d{2})Z$') { return $false }
    $month = [int]$Matches[2]; $day = [int]$Matches[3]; $hour = [int]$Matches[4]; $minute = [int]$Matches[5]; $second = [int]$Matches[6]
    return $month -ge 1 -and $month -le 12 -and $day -ge 1 -and $day -le 31 -and $hour -le 23 -and $minute -le 59 -and $second -le 60
}

$script:MinimumOsPattern = '^(0|[1-9]\d{0,8})\.(0|[1-9]\d{0,8})\.(0|[1-9]\d{0,8})$'

function Test-JsonInteger($Value) {
    return $Value -is [int] -or $Value -is [long] -or $Value -is [int64] -or $Value -is [int32]
}

function Read-PackageManifest {
    <#
    .SYNOPSIS
        Reads tools\package.ps1's package manifest (Lucent-Setup-<version>-x64.manifest.json) and
        checks that it names the installer: its file name, size and SHA-256.
    #>
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][string]$InstallerPath)
    $package = [System.IO.File]::ReadAllText((Resolve-Path -LiteralPath $Path).Path, $script:Utf8) | ConvertFrom-Json
    $names = @($package.PSObject.Properties.Name)
    if ($package.product -ne 'Lucent' -or $package.architecture -ne 'x64' -or $names -notcontains 'schema' -or [int]$package.schema -lt 4) {
        throw "$Path is not a package manifest of Lucent x64 (schema 4 or later) written by tools\package.ps1."
    }
    if ($null -eq $package.installer) { throw "$Path names no installer (a -StageOnly package is not a release)." }
    $item = Get-Item -LiteralPath $InstallerPath
    $sha256 = (Get-FileHash -LiteralPath $InstallerPath -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($package.installer.file -ne $item.Name -or [int64]$package.installer.size -ne $item.Length -or ([string]$package.installer.sha256).ToLowerInvariant() -ne $sha256) {
        throw "$Path names $($package.installer.file) ($($package.installer.size) bytes, $($package.installer.sha256)), not $($item.Name) ($($item.Length) bytes, $sha256)."
    }
    return $package
}

function New-LucentReleaseManifest {
    <#
    .SYNOPSIS
        Writes the release manifest of a packaged installer (lucent.release-manifest, version 1), in
        Lucent's canonical form.
    .PARAMETER PackageManifest
        The package manifest tools\package.ps1 wrote beside the installer
        (Lucent-Setup-<version>-x64.manifest.json): it must name this installer (size, SHA-256), and
        gives the release's version, signing and update channel.
    .PARAMETER Version
        The release's version; default: the package manifest's. Must equal it, the version in the
        installer's name (Lucent-Setup-<version>-x64.exe) and, unless -AllowUnsignedInstaller, the
        installer's ProductVersion.
    .PARAMETER InstallerUrl
        Where the installer will be published: an https URL (Lucent downloads over https only).
    .PARAMETER AllowHttp
        Tests only: accept an http URL (Lucent's tests serve loopback http).
    .PARAMETER AllowUnsignedInstaller
        Tests and rehearsals only: accept an installer that is unsigned or test-signed, or carries no
        ProductVersion, and a package manifest that records no corresponding-source archive. Never for
        a release.
    .NOTES
        A release's package manifest (schema 5) records its corresponding-source archive,
        Lucent-<version>-corresponding-source.zip (ADR 0024): it must be beside the installer with the
        recorded size and SHA-256, because it is published with it. The archive is not named in the
        release manifest (Lucent downloads only the installer); it is published at the same place.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Installer,
        [Parameter(Mandatory)][string]$PackageManifest,
        [Parameter(Mandatory)][string]$InstallerUrl,
        [Parameter(Mandatory)][string]$Channel,
        [Parameter(Mandatory)][string]$OutFile,
        [string]$Version,
        [string]$ReleaseNotes = '',
        [string]$ReleaseNotesFile,
        [string]$MinimumOs = '10.0.17763',
        [datetime]$Published = [datetime]::UtcNow,
        [switch]$AllowHttp,
        [switch]$AllowUnsignedInstaller
    )
    $installerPath = (Resolve-Path -LiteralPath $Installer).Path
    $package = Read-PackageManifest -Path $PackageManifest -InstallerPath $installerPath
    # One version everywhere (SEC-04): the package's, the file name's, -Version and Setup's own.
    if (-not $Version) { $Version = [string]$package.version }
    if ($Version -cne [string]$package.version) { throw "-Version $Version differs from the package manifest's version $($package.version)." }
    if ([System.IO.Path]::GetFileName($installerPath) -notmatch '^Lucent-Setup-(.+)-x64\.exe$' -or $Matches[1] -cne $Version) {
        throw "The installer's name $([System.IO.Path]::GetFileName($installerPath)) is not Lucent-Setup-$Version-x64.exe."
    }
    if ($Version -notmatch $script:SemVer) { throw "'$Version' is not a semantic version (SemVer 2.0.0)." }
    # The corresponding source of the LGPL libraries is published with every installer, from the same
    # place (ADR 0024): the package manifest (schema 5) records the archive, and the archive beside the
    # installer must be exactly that one. Only tests and rehearsals (-AllowUnsignedInstaller) may
    # publish a package that records none.
    $names = @($package.PSObject.Properties.Name)
    $sourceRecord = if ($names -contains 'correspondingSource') { $package.correspondingSource } else { $null }
    if ($null -eq $sourceRecord) {
        if (-not $AllowUnsignedInstaller) {
            throw "$PackageManifest records no corresponding-source archive (a package manifest of schema 5 does, ADR 0024): package the release again with tools\package.ps1."
        }
    }
    else {
        $sourceName = "Lucent-$Version-corresponding-source.zip"
        $sourcePath = Join-Path (Split-Path -Parent $installerPath) $sourceName
        if ([string]$sourceRecord.file -cne $sourceName -or -not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
            throw "The corresponding-source archive $sourceName must be beside the installer, to be published with it (ADR 0024); $sourcePath is missing or the package records another name ($($sourceRecord.file))."
        }
        $sourceSize = (Get-Item -LiteralPath $sourcePath).Length
        $sourceSha = (Get-FileHash -LiteralPath $sourcePath -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($sourceSize -ne [int64]$sourceRecord.size -or $sourceSha -ne ([string]$sourceRecord.sha256).ToLowerInvariant()) {
            throw "$sourcePath ($sourceSize bytes, SHA-256 $sourceSha) is not the corresponding-source archive the package manifest records ($($sourceRecord.size) bytes, $($sourceRecord.sha256))."
        }
    }
    # The release's own Lucent.exe must keep checking this channel (SEC-02).
    $compiled = if ($names -contains 'updateChannel') { $package.updateChannel } else { $null }
    if ($null -eq $compiled -or $compiled.name -cne $Channel) {
        $what = if ($null -eq $compiled) { 'checks no update channel' } else { "checks the channel '$($compiled.name)'" }
        throw "The installer's Lucent.exe $what, so it cannot be published on '$Channel': its users would lose this channel's updates."
    }
    if (-not $AllowUnsignedInstaller) {
        # Only a release signature is published (SEC-03): never unsigned, test-signed or untimestamped.
        $signing = $package.signing
        if (-not $package.signed -or $null -eq $signing -or $signing.test -or -not $signing.timestamped) {
            throw 'The package is not signed for release (unsigned, a test signature, or no timestamp): see tools\package.ps1 and docs\development\setup.md.'
        }
        $signature = Get-AuthenticodeSignature -LiteralPath $installerPath
        if ($signature.Status -ne 'Valid' -or $null -eq $signature.SignerCertificate -or
            $signature.SignerCertificate.Thumbprint -ne [string]$signing.certificateSha1 -or $null -eq $signature.TimeStamperCertificate) {
            throw "The installer's Authenticode signature is $($signature.Status), not a timestamped release signature by $($signing.certificateSha1)."
        }
        # The signer must be one the channel's builds check (its publisher thumbprints), or the clients
        # of this channel refuse the installer.
        $pinned = @(@($compiled.publisherThumbprints) | ForEach-Object { ([string]$_).ToLowerInvariant() })
        $signerSha1 = $signature.SignerCertificate.Thumbprint.ToLowerInvariant()
        $sha = [System.Security.Cryptography.SHA256]::Create()
        try { $signerSha256 = ConvertTo-HexString $sha.ComputeHash($signature.SignerCertificate.RawData) } finally { $sha.Dispose() }
        if ($pinned.Count -eq 0 -or ($pinned -notcontains $signerSha1 -and $pinned -notcontains $signerSha256)) {
            throw "The installer is signed by $signerSha1, which is not among the publisher certificates the channel's Lucent.exe checks ($($pinned -join ', ')); a release channel names its publishers (installer\update-channels.json)."
        }
        $productVersion = (Get-Item -LiteralPath $installerPath).VersionInfo.ProductVersion
        if ([string]$productVersion -cne $Version) { throw "The installer's ProductVersion is '$productVersion', not $Version." }
    }
    if (-not (Test-ChannelName $Channel)) { throw "'$Channel' is not a channel name ([a-z0-9] words joined by '-', at most 32 characters)." }
    if (-not (Test-HttpUrl $InstallerUrl) -or (-not $AllowHttp -and -not $InstallerUrl.StartsWith('https://'))) {
        throw "'$InstallerUrl' is not an https URL of printable ASCII characters (at most 2048)."
    }
    if ($MinimumOs -notmatch '^(0|[1-9]\d{0,8})\.(0|[1-9]\d{0,8})\.(0|[1-9]\d{0,8})$') { throw "'$MinimumOs' is not major.minor.build." }
    if ($ReleaseNotesFile) { $ReleaseNotes = [System.IO.File]::ReadAllText((Resolve-Path -LiteralPath $ReleaseNotesFile).Path, $script:Utf8) }
    $ReleaseNotes = $ReleaseNotes -replace "`r`n", "`n"
    if ($script:Utf8.GetByteCount($ReleaseNotes) -gt 16384) { throw 'The release notes have more than 16 KiB.' }
    $size = (Get-Item -LiteralPath $installerPath).Length
    if ($size -le 0 -or $size -gt 2GB) { throw "The installer has $size bytes." }
    $sha256 = (Get-FileHash -LiteralPath $installerPath -Algorithm SHA256).Hash.ToLowerInvariant()
    $publishedText = $Published.ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ', [System.Globalization.CultureInfo]::InvariantCulture)

    $text = ConvertTo-ReleaseManifestText -Channel $Channel -Version $Version -Published $publishedText -MinimumOs $MinimumOs `
        -InstallerUrl $InstallerUrl -Size $size -Sha256 $sha256 -ReleaseNotes $ReleaseNotes
    $out = [System.IO.Path]::GetFullPath($OutFile)
    [System.IO.File]::WriteAllBytes($out, $script:Utf8.GetBytes($text))
    return [pscustomobject]@{ Manifest = $out; Version = $Version; Channel = $Channel; Size = $size; Sha256 = $sha256 }
}

function Protect-LucentReleaseManifest {
    <#
    .SYNOPSIS
        Signs the manifest file's exact bytes with the publisher's key and writes <manifest>.sig
        (lucent.release-signature, version 1).
    #>
    [CmdletBinding(DefaultParameterSetName = 'Name')]
    param(
        [Parameter(Mandatory)][string]$Manifest,
        [Parameter(Mandatory, ParameterSetName = 'Name')][string]$KeyName,
        [Parameter(ParameterSetName = 'Name')][string]$Provider = 'Microsoft Software Key Storage Provider',
        [Parameter(Mandatory, ParameterSetName = 'Key')][System.Security.Cryptography.CngKey]$Key,
        [string]$OutFile
    )
    $manifestPath = (Resolve-Path -LiteralPath $Manifest).Path
    $bytes = [System.IO.File]::ReadAllBytes($manifestPath)
    $opened = $null
    if ($PSCmdlet.ParameterSetName -eq 'Name') {
        $opened = [System.Security.Cryptography.CngKey]::Open($KeyName, (New-Object System.Security.Cryptography.CngProvider($Provider)))
        $Key = $opened
    }
    try {
        $point = Get-PointOf $Key
        $ecdsa = New-Object System.Security.Cryptography.ECDsaCng($Key)
        try {
            # IEEE P1363 r || s (64 bytes) over SHA-256 of the bytes, as Lucent verifies it.
            $signature = $ecdsa.SignData($bytes, [System.Security.Cryptography.HashAlgorithmName]::SHA256)
        }
        finally { $ecdsa.Dispose() }
    }
    finally { if ($opened) { $opened.Dispose() } }
    if ($signature.Length -ne 64) { throw "Unexpected signature length $($signature.Length)." }
    $keyId = Get-Sha256Hex $point
    $text = '{"schema":"' + $script:SignatureSchema + '","schema_version":1,"algorithm":"' + $script:Algorithm +
            '","key_id":"' + $keyId + '","signature":"' + [Convert]::ToBase64String($signature) + '"}'
    $out = if ($OutFile) { [System.IO.Path]::GetFullPath($OutFile) } else { $manifestPath + '.sig' }
    [System.IO.File]::WriteAllBytes($out, $script:Utf8.GetBytes($text))
    return [pscustomobject]@{ Signature = $out; KeyId = $keyId }
}

function Test-LucentReleaseManifest {
    <#
    .SYNOPSIS
        Verifies a release at least as strictly as Lucent will (SEC-06): the signature document, the
        signature over the manifest's exact bytes with one of the pinned public keys, then every
        member of the manifest, its type and its bounds as Lucent's reader checks them, and that the
        manifest is the canonical text New-LucentReleaseManifest writes (no unknown, repeated or
        reordered member, no other number or escape form); optionally the installer's size and
        SHA-256. Returns the manifest's facts; throws on any failure.
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Manifest,
        [Parameter(Mandatory)][string[]]$PublicKey,
        [string]$Signature,
        [string]$Installer,
        [switch]$AllowHttp
    )
    $manifestPath = (Resolve-Path -LiteralPath $Manifest).Path
    $signaturePath = if ($Signature) { (Resolve-Path -LiteralPath $Signature).Path } else { $manifestPath + '.sig' }
    $bytes = [System.IO.File]::ReadAllBytes($manifestPath)
    if ($bytes.Length -gt 65536) { throw 'The manifest has more than 64 KiB.' }
    $signatureText = [System.IO.File]::ReadAllText($signaturePath, $script:Utf8)
    if ($script:Utf8.GetByteCount($signatureText) -gt 4096) { throw 'The signature file has more than 4 KiB.' }
    $envelope = $signatureText | ConvertFrom-Json
    if ($envelope.schema -cne $script:SignatureSchema -or -not (Test-JsonInteger $envelope.schema_version) -or $envelope.schema_version -ne 1 -or
        $envelope.algorithm -cne $script:Algorithm) {
        throw 'The signature file is not a lucent.release-signature version 1 (ecdsa-p256-sha256).'
    }
    if ($envelope.key_id -isnot [string] -or $envelope.key_id -cnotmatch '^[0-9a-f]{64}$' -or $envelope.signature -isnot [string]) {
        throw 'The signature file has no key id of 64 lowercase hex digits or no signature text.'
    }
    try { $signatureBytes = [Convert]::FromBase64String($envelope.signature) } catch { throw 'The signature is not base64.' }
    if ($signatureBytes.Length -ne 64 -or [Convert]::ToBase64String($signatureBytes) -cne $envelope.signature) {
        throw 'The signature is not the canonical base64 of 64 bytes (r || s).'
    }
    $canonicalEnvelope = '{"schema":"' + $script:SignatureSchema + '","schema_version":1,"algorithm":"' + $script:Algorithm +
                         '","key_id":"' + $envelope.key_id + '","signature":"' + $envelope.signature + '"}'
    if ($signatureText -cne $canonicalEnvelope) { throw 'The signature file is not in the canonical form Protect-LucentReleaseManifest writes.' }
    $verified = $false
    foreach ($hex in $PublicKey) {
        $hex = $hex.Trim().ToLowerInvariant()
        if ($hex -notmatch '^04[0-9a-f]{128}$') { throw "'$hex' is not a P-256 public key (130 hex digits starting with 04)." }
        $point = New-Object byte[] 65
        for ($i = 0; $i -lt 65; $i++) { $point[$i] = [Convert]::ToByte($hex.Substring(2 * $i, 2), 16) }
        if ((Get-Sha256Hex $point) -ne $envelope.key_id) { continue }
        $blob = New-Object byte[] 72
        [Array]::Copy([BitConverter]::GetBytes([uint32]0x31534345), 0, $blob, 0, 4)  # BCRYPT_ECDSA_PUBLIC_P256_MAGIC
        [Array]::Copy([BitConverter]::GetBytes([uint32]32), 0, $blob, 4, 4)
        [Array]::Copy($point, 1, $blob, 8, 64)
        $key = [System.Security.Cryptography.CngKey]::Import($blob, [System.Security.Cryptography.CngKeyBlobFormat]::EccPublicBlob)
        $ecdsa = New-Object System.Security.Cryptography.ECDsaCng($key)
        try { $verified = $ecdsa.VerifyData($bytes, $signatureBytes, [System.Security.Cryptography.HashAlgorithmName]::SHA256) }
        finally { $ecdsa.Dispose(); $key.Dispose() }
        break
    }
    if (-not $verified) { throw 'The manifest is not signed by any of the given keys (or was changed after signing).' }
    $manifestText = $script:Utf8.GetString($bytes)
    try { $data = $manifestText | ConvertFrom-Json } catch { throw "The manifest is not valid JSON: $($_.Exception.Message)" }
    $members = @($data.PSObject.Properties.Name)
    foreach ($name in @('schema', 'schema_version', 'product', 'channel', 'version', 'published', 'architecture', 'minimum_os', 'installer', 'release_notes')) {
        if ($members -cnotcontains $name) { throw "The manifest has no member '$name'." }
    }
    if ($data.schema -cne $script:ManifestSchema -or -not (Test-JsonInteger $data.schema_version) -or $data.schema_version -ne 1 -or
        $data.product -cne 'Lucent' -or $data.architecture -cne 'x64') {
        throw 'The manifest is not a lucent.release-manifest version 1 for Lucent x64.'
    }
    foreach ($name in @('channel', 'version', 'published', 'minimum_os', 'release_notes')) {
        if ($data.$name -isnot [string]) { throw "The manifest's $name is not a string." }
    }
    if (-not (Test-ChannelName $data.channel)) { throw "Invalid channel '$($data.channel)'." }
    if ($data.version -cnotmatch $script:SemVer) { throw "Invalid version '$($data.version)'." }
    if (-not (Test-UtcTimestamp $data.published)) { throw "The publication time '$($data.published)' is not YYYY-MM-DDTHH:MM:SSZ." }
    if ($data.minimum_os -cnotmatch $script:MinimumOsPattern) { throw "The minimum Windows version '$($data.minimum_os)' is not major.minor.build." }
    if ($script:Utf8.GetByteCount($data.release_notes) -gt 16384) { throw 'The release notes have more than 16 KiB.' }
    $entry = $data.installer  # ($installer is the -Installer parameter: a string)
    if ($null -eq $entry -or $entry -is [string] -or $entry -is [System.ValueType] -or $entry -is [System.Array]) {
        throw "The manifest's installer is not an object."
    }
    $installerMembers = @($entry.PSObject.Properties.Name)
    foreach ($name in @('url', 'size', 'sha256')) {
        if ($installerMembers -cnotcontains $name) { throw "The manifest's installer has no member '$name'." }
    }
    if ($entry.url -isnot [string] -or -not (Test-HttpUrl $entry.url) -or (-not $AllowHttp -and -not $entry.url.StartsWith('https://'))) {
        throw "The installer URL '$($entry.url)' is not https."
    }
    if (-not (Test-JsonInteger $entry.size) -or [int64]$entry.size -lt 1 -or [int64]$entry.size -gt 2GB) {
        throw "The installer size '$($entry.size)' is not a whole number of bytes from 1 to 2 GiB."
    }
    if ($entry.sha256 -isnot [string] -or $entry.sha256 -cnotmatch '^[0-9a-f]{64}$') { throw 'Invalid installer SHA-256.' }
    # Anything else (an unknown or repeated member, another order, whitespace, another escape or number
    # form) is not what New-LucentReleaseManifest writes.
    $canonical = ConvertTo-ReleaseManifestText -Channel $data.channel -Version $data.version -Published $data.published `
        -MinimumOs $data.minimum_os -InstallerUrl $entry.url -Size ([int64]$entry.size) -Sha256 $entry.sha256 `
        -ReleaseNotes $data.release_notes
    if ($manifestText -cne $canonical) {
        throw 'The manifest is not in the canonical form New-LucentReleaseManifest writes (an unknown, repeated or reordered member, whitespace, or another escape or number form).'
    }
    $data = [pscustomobject]@{ version = $data.version; channel = $data.channel; installer = $entry }
    if ($Installer) {
        $installerPath = (Resolve-Path -LiteralPath $Installer).Path
        $size = (Get-Item -LiteralPath $installerPath).Length
        $sha256 = (Get-FileHash -LiteralPath $installerPath -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($size -ne [int64]$data.installer.size -or $sha256 -ne $data.installer.sha256) {
            throw "The installer ($size bytes, $sha256) is not the one the manifest names ($($data.installer.size) bytes, $($data.installer.sha256))."
        }
    }
    return [pscustomobject]@{ Version = $data.version; Channel = $data.channel; KeyId = $envelope.key_id
                              InstallerUrl = $data.installer.url; Size = [int64]$data.installer.size; Sha256 = $data.installer.sha256 }
}

Export-ModuleMember -Function New-LucentUpdateSigningKey, Get-LucentUpdatePublicKey, New-LucentReleaseManifest,
    Protect-LucentReleaseManifest, Test-LucentReleaseManifest
