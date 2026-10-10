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
          -InstallerUrl https://updates.example.org/lucent/Lucent-Setup-0.2.0-x64.exe -Channel stable `
          -ReleaseNotesFile notes.txt -OutFile release-manifest.json
      Protect-LucentReleaseManifest -Manifest release-manifest.json -KeyName 'Lucent release 2026'
      Test-LucentReleaseManifest -Manifest release-manifest.json -PublicKey <130 hex digits> `
          -Installer build\release\package\Lucent-Setup-0.2.0-x64.exe

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

function New-LucentReleaseManifest {
    <#
    .SYNOPSIS
        Writes the release manifest of a packaged installer (lucent.release-manifest, version 1), in
        Lucent's canonical form.
    .PARAMETER Version
        The release's version; default: the version in the installer's name (Lucent-Setup-<version>-x64.exe).
    .PARAMETER InstallerUrl
        Where the installer will be published: an https URL (Lucent downloads over https only).
    .PARAMETER AllowHttp
        Tests only: accept an http URL (Lucent's tests serve loopback http).
    #>
    [CmdletBinding()]
    param(
        [Parameter(Mandatory)][string]$Installer,
        [Parameter(Mandatory)][string]$InstallerUrl,
        [Parameter(Mandatory)][string]$Channel,
        [Parameter(Mandatory)][string]$OutFile,
        [string]$Version,
        [string]$ReleaseNotes = '',
        [string]$ReleaseNotesFile,
        [string]$MinimumOs = '10.0.17763',
        [datetime]$Published = [datetime]::UtcNow,
        [switch]$AllowHttp
    )
    $installerPath = (Resolve-Path -LiteralPath $Installer).Path
    if (-not $Version) {
        if ([System.IO.Path]::GetFileName($installerPath) -notmatch '^Lucent-Setup-(.+)-x64\.exe$') { throw 'Pass -Version: the installer is not named Lucent-Setup-<version>-x64.exe.' }
        $Version = $Matches[1]
    }
    if ($Version -notmatch $script:SemVer) { throw "'$Version' is not a semantic version (SemVer 2.0.0)." }
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

    # Members in Lucent's canonical order, no whitespace (write_release_manifest).
    $text = '{"schema":' + (ConvertTo-JsonText $script:ManifestSchema) + ',"schema_version":1,"product":"Lucent"' +
            ',"channel":' + (ConvertTo-JsonText $Channel) + ',"version":' + (ConvertTo-JsonText $Version) +
            ',"published":' + (ConvertTo-JsonText $publishedText) + ',"architecture":"x64"' +
            ',"minimum_os":' + (ConvertTo-JsonText $MinimumOs) +
            ',"installer":{"url":' + (ConvertTo-JsonText $InstallerUrl) + ',"size":' + $size +
            ',"sha256":' + (ConvertTo-JsonText $sha256) + '},"release_notes":' + (ConvertTo-JsonText $ReleaseNotes) + '}'
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
        Verifies a release as Lucent will: the signature over the manifest's exact bytes with one of the
        pinned public keys, then the manifest's members, and optionally the installer's size and SHA-256.
        Returns the manifest's facts; throws on any failure.
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
    $envelope = [System.IO.File]::ReadAllText($signaturePath, $script:Utf8) | ConvertFrom-Json
    if ($envelope.schema -ne $script:SignatureSchema -or $envelope.schema_version -ne 1 -or $envelope.algorithm -ne $script:Algorithm) {
        throw 'The signature file is not a lucent.release-signature version 1 (ecdsa-p256-sha256).'
    }
    $signatureBytes = [Convert]::FromBase64String($envelope.signature)
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
    $data = $script:Utf8.GetString($bytes) | ConvertFrom-Json
    if ($data.schema -ne $script:ManifestSchema -or $data.schema_version -ne 1 -or $data.product -ne 'Lucent' -or $data.architecture -ne 'x64') {
        throw 'The manifest is not a lucent.release-manifest version 1 for Lucent x64.'
    }
    if (-not (Test-ChannelName $data.channel)) { throw "Invalid channel '$($data.channel)'." }
    if ($data.version -notmatch $script:SemVer) { throw "Invalid version '$($data.version)'." }
    if (-not (Test-HttpUrl $data.installer.url) -or (-not $AllowHttp -and -not $data.installer.url.StartsWith('https://'))) {
        throw "The installer URL '$($data.installer.url)' is not https."
    }
    if ($data.installer.sha256 -cnotmatch '^[0-9a-f]{64}$') { throw 'Invalid installer SHA-256.' }
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
