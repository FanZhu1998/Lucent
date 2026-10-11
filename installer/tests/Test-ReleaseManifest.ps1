<#
.SYNOPSIS
    Checks the update channel's release tooling (installer\scripts\LucentUpdate.psm1; W12, ADR 0022)
    with an ephemeral key: nothing is stored in any key store, nothing is downloaded or uploaded.

.DESCRIPTION
    1. An ephemeral ECDSA P-256 key (CngKey.Create without a name): its public key has the form Lucent
       pins (130 hex digits, 04...) and its key id is the SHA-256 of the point.
    2. A release manifest for a stand-in installer: exactly Lucent's canonical text (members in order,
       no whitespace, JSON escapes of quotes, backslashes and control characters, UTF-8 text).
    3. Its signature verifies with the key, also with the installer; with another key, after one byte of
       the manifest changed, or for another installer, verification fails.
    4. Invalid releases are refused before anything is written: an http URL, a bad channel name, a
       version that is not SemVer, release notes above 16 KiB; and (W12 review SEC-03, SEC-04) a
       package manifest that names another installer, a version other than the package's, a package
       whose Lucent.exe checks no channel or another one, and an unsigned or test-signed package
       without -AllowUnsignedInstaller; and (ADR 0024) a corresponding-source archive that is missing
       beside the installer or differs from the one the package manifest records, and, without
       -AllowUnsignedInstaller, a package manifest that records none.
    5. Manifests that Lucent's reader refuses are refused by Test-LucentReleaseManifest too, although
       validly signed (SEC-06): an unknown member, schema_version as a string, minimum_os '10.0', an
       impossible publication time, an installer of 0 bytes, 20 KiB of release notes, an extra
       installer member; and a signature file that is not canonical.

    Registered with CTest as lucent_release_manifest_tests (label "installer"). Lucent's own reader
    checks the module's output too (src/application/tests/update_service_test.cpp,
    UpdateInterop.ThePublishersToolingProducesWhatLucentVerifies). Exit code 0 when every check
    passed. Windows PowerShell 5.1 and PowerShell 7. Keep this file ASCII.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Import-Module (Join-Path $repoRoot 'installer\scripts\LucentUpdate.psm1') -Force

$script:checks = 0
$script:failures = 0
function Assert-True([bool]$Condition, [string]$What) {
    $script:checks++
    if (-not $Condition) { $script:failures++; Write-Host "FAILED: $What" }
}
function Assert-Throws([scriptblock]$Block, [string]$What, [string]$Pattern) {
    $script:checks++
    try { & $Block; $script:failures++; Write-Host "FAILED (no error): $What" }
    catch { if ($Pattern -and $_.Exception.Message -notmatch $Pattern) { $script:failures++; Write-Host "FAILED (another error: $($_.Exception.Message)): $What" } }
}

$work = Join-Path ([System.IO.Path]::GetTempPath()) ('lucent-release-manifest-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
$key = $null
$other = $null
try {
    # 1. Ephemeral keys (no name: never persisted).
    $key = [System.Security.Cryptography.CngKey]::Create([System.Security.Cryptography.CngAlgorithm]::ECDsaP256)
    $other = [System.Security.Cryptography.CngKey]::Create([System.Security.Cryptography.CngAlgorithm]::ECDsaP256)
    Assert-True $key.IsEphemeral 'the test key is ephemeral (never stored)'
    $public = Get-LucentUpdatePublicKey -Key $key
    Assert-True ($public.PublicKey -cmatch '^04[0-9a-f]{128}$') "public key form: $($public.PublicKey)"
    $point = New-Object byte[] 65
    for ($i = 0; $i -lt 65; $i++) { $point[$i] = [Convert]::ToByte($public.PublicKey.Substring(2 * $i, 2), 16) }
    $sha = [System.Security.Cryptography.SHA256]::Create()
    $expectedId = -join ($sha.ComputeHash($point) | ForEach-Object { $_.ToString('x2') })
    $sha.Dispose()
    Assert-True ($public.KeyId -eq $expectedId) 'the key id is the SHA-256 of the point'

    # 2. The canonical manifest of a stand-in installer, with the package manifest tools\package.ps1
    #    would have written for it (unsigned: -AllowUnsignedInstaller).
    $installer = Join-Path $work 'Lucent-Setup-0.2.0-x64.exe'
    [System.IO.File]::WriteAllBytes($installer, [byte[]](1..200 | ForEach-Object { [byte]($_ % 251) }))
    $installerSha = (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash.ToLowerInvariant()
    # The corresponding-source archive tools\package.ps1 writes beside the installer (ADR 0024).
    $sourceArchive = Join-Path $work 'Lucent-0.2.0-corresponding-source.zip'
    [System.IO.File]::WriteAllBytes($sourceArchive, [byte[]](1..300 | ForEach-Object { [byte]($_ % 7) }))
    $sourceRecord = [ordered]@{ file = 'Lucent-0.2.0-corresponding-source.zip'; size = 300
        sha256 = (Get-FileHash -LiteralPath $sourceArchive -Algorithm SHA256).Hash.ToLowerInvariant() }
    $utf8 = New-Object System.Text.UTF8Encoding($false)
    function Write-Package([string]$Path, [hashtable]$Change = @{}) {
        $package = [ordered]@{ schema = 5; product = 'Lucent'; version = '0.2.0'; architecture = 'x64'
            installer = [ordered]@{ file = 'Lucent-Setup-0.2.0-x64.exe'; size = 200; sha256 = $installerSha; signed = $false }
            correspondingSource = $sourceRecord
            signed = $false; signing = $null
            updateChannel = [ordered]@{ name = 'stable'; manifestUrl = 'https://updates.example.org/lucent/release-manifest.json'
                keyIds = @($public.KeyId); publisherThumbprints = @() } }
        foreach ($key in $Change.Keys) { $package[$key] = $Change[$key] }
        [System.IO.File]::WriteAllText($Path, (ConvertTo-Json -InputObject $package -Depth 8), $utf8)
        return $Path
    }
    $package = Write-Package (Join-Path $work 'Lucent-Setup-0.2.0-x64.manifest.json')
    $notes = "Faster previews.`nFixes `"quoted`" names, tabs`tand \ backslashes; caf" + [char]0xE9 + '.'
    $manifest = Join-Path $work 'release-manifest.json'
    $made = New-LucentReleaseManifest -Installer $installer -PackageManifest $package -AllowUnsignedInstaller `
        -InstallerUrl 'https://updates.example.org/lucent/Lucent-Setup-0.2.0-x64.exe' `
        -Channel stable -ReleaseNotes $notes -Published ([datetime]::new(2026, 10, 9, 12, 0, 0, [DateTimeKind]::Utc)) -OutFile $manifest
    Assert-True ($made.Version -eq '0.2.0') 'the version comes from the package manifest'
    $expected = '{"schema":"lucent.release-manifest","schema_version":1,"product":"Lucent","channel":"stable","version":"0.2.0",' +
                '"published":"2026-10-09T12:00:00Z","architecture":"x64","minimum_os":"10.0.17763","installer":{"url":' +
                '"https://updates.example.org/lucent/Lucent-Setup-0.2.0-x64.exe","size":200,"sha256":"' + $installerSha + '"},' +
                '"release_notes":"Faster previews.\nFixes \"quoted\" names, tabs\tand \\ backslashes; caf' + [char]0xE9 + '."}'
    $written = [System.IO.File]::ReadAllText($manifest, (New-Object System.Text.UTF8Encoding($false)))
    Assert-True ($written -ceq $expected) "canonical manifest text:`n  got      $written`n  expected $expected"
    $raw = [System.IO.File]::ReadAllBytes($manifest)
    Assert-True ($raw[0] -eq [byte][char]'{') 'no byte order mark'

    # 3. Signing and verification.
    $signed = Protect-LucentReleaseManifest -Manifest $manifest -Key $key
    Assert-True ($signed.KeyId -eq $public.KeyId) 'the signature names the key'
    $envelope = Get-Content -LiteralPath ($manifest + '.sig') -Raw | ConvertFrom-Json
    Assert-True ($envelope.schema -eq 'lucent.release-signature' -and $envelope.algorithm -eq 'ecdsa-p256-sha256') 'signature document header'
    Assert-True ([Convert]::FromBase64String($envelope.signature).Length -eq 64) 'a 64-byte r || s signature'
    $result = Test-LucentReleaseManifest -Manifest $manifest -PublicKey $public.PublicKey -Installer $installer
    Assert-True ($result.Version -eq '0.2.0' -and $result.Size -eq 200 -and $result.Sha256 -eq $installerSha) 'verifies with the key and the installer'
    $otherKey = (Get-LucentUpdatePublicKey -Key $other).PublicKey
    Assert-True ((Test-LucentReleaseManifest -Manifest $manifest -PublicKey @($otherKey, $public.PublicKey)).KeyId -eq $public.KeyId) 'the matching key of several is used'
    Assert-Throws { Test-LucentReleaseManifest -Manifest $manifest -PublicKey $otherKey } 'another key is refused'
    $tampered = Join-Path $work 'tampered.json'
    $bytes = [System.IO.File]::ReadAllBytes($manifest)
    $bytes[$bytes.Length - 5] = $bytes[$bytes.Length - 5] -bxor 1
    [System.IO.File]::WriteAllBytes($tampered, $bytes)
    Copy-Item -LiteralPath ($manifest + '.sig') -Destination ($tampered + '.sig')
    Assert-Throws { Test-LucentReleaseManifest -Manifest $tampered -PublicKey $public.PublicKey } 'a changed manifest is refused'
    $wrongInstaller = Join-Path $work 'other.exe'
    [System.IO.File]::WriteAllBytes($wrongInstaller, [byte[]](1..200 | ForEach-Object { [byte](($_ + 1) % 251) }))
    Assert-Throws { Test-LucentReleaseManifest -Manifest $manifest -PublicKey $public.PublicKey -Installer $wrongInstaller } 'another installer is refused'

    # 4. Invalid releases are refused before anything is written.
    $bad = Join-Path $work 'bad.json'
    $url = 'https://updates.example.org/x.exe'
    Assert-Throws { New-LucentReleaseManifest -Installer $installer -PackageManifest $package -AllowUnsignedInstaller -InstallerUrl 'http://updates.example.org/x.exe' -Channel stable -OutFile $bad } 'an http URL'
    Assert-Throws { New-LucentReleaseManifest -Installer $installer -PackageManifest $package -AllowUnsignedInstaller -InstallerUrl $url -Channel 'Stable' -OutFile $bad } 'a channel name with capitals'
    Assert-Throws { New-LucentReleaseManifest -Installer $installer -PackageManifest $package -AllowUnsignedInstaller -InstallerUrl $url -Channel stable -Version 'v1.0' -OutFile $bad } 'a version that is not SemVer'
    Assert-Throws { New-LucentReleaseManifest -Installer $installer -PackageManifest $package -AllowUnsignedInstaller -InstallerUrl $url -Channel stable -ReleaseNotes ('x' * 16385) -OutFile $bad } 'release notes above 16 KiB'
    # The release is what tools\package.ps1 built and recorded (SEC-03, SEC-04).
    Assert-Throws { New-LucentReleaseManifest -Installer $installer -PackageManifest $package -AllowUnsignedInstaller -InstallerUrl $url -Channel stable -Version '9.9.9' -OutFile $bad } 'a version other than the package''s'
    Assert-Throws { New-LucentReleaseManifest -Installer $wrongInstaller -PackageManifest $package -AllowUnsignedInstaller -InstallerUrl $url -Channel stable -Version '0.2.0' -OutFile $bad } 'an installer the package manifest does not name'
    $noChannel = Write-Package (Join-Path $work 'no-channel.manifest.json') @{ updateChannel = $null }
    Assert-Throws { New-LucentReleaseManifest -Installer $installer -PackageManifest $noChannel -AllowUnsignedInstaller -InstallerUrl $url -Channel stable -OutFile $bad } 'a package whose Lucent checks no channel'
    Assert-Throws { New-LucentReleaseManifest -Installer $installer -PackageManifest $package -AllowUnsignedInstaller -InstallerUrl $url -Channel beta -OutFile $bad } 'a package whose Lucent checks another channel'
    $stageOnly = Write-Package (Join-Path $work 'stage-only.manifest.json') @{ installer = $null }
    Assert-Throws { New-LucentReleaseManifest -Installer $installer -PackageManifest $stageOnly -AllowUnsignedInstaller -InstallerUrl $url -Channel stable -OutFile $bad } 'a -StageOnly package'
    Assert-Throws { New-LucentReleaseManifest -Installer $installer -PackageManifest $package -InstallerUrl $url -Channel stable -OutFile $bad } 'an unsigned package without -AllowUnsignedInstaller'
    $testSigned = Write-Package (Join-Path $work 'test-signed.manifest.json') @{ signed = $true
        signing = [ordered]@{ test = $true; certificateSha1 = ('ab' * 20); timestamped = $false } }
    Assert-Throws { New-LucentReleaseManifest -Installer $installer -PackageManifest $testSigned -InstallerUrl $url -Channel stable -OutFile $bad } 'a test-signed package without -AllowUnsignedInstaller'
    # The corresponding source is published with the installer (ADR 0024).
    $noSource = Write-Package (Join-Path $work 'no-source.manifest.json') @{ correspondingSource = $null; signed = $true
        signing = [ordered]@{ test = $false; certificateSha1 = ('ab' * 20); timestamped = $true } }
    Assert-Throws { New-LucentReleaseManifest -Installer $installer -PackageManifest $noSource -InstallerUrl $url -Channel stable -OutFile $bad } `
        'a release package that records no corresponding-source archive' 'corresponding-source'
    $rehearsal = New-LucentReleaseManifest -Installer $installer -PackageManifest $noSource -AllowUnsignedInstaller -InstallerUrl $url -Channel stable -OutFile (Join-Path $work 'rehearsal.json')
    Assert-True ($rehearsal.Version -eq '0.2.0') 'a rehearsal (-AllowUnsignedInstaller) of a package without one is accepted'
    $otherName = Write-Package (Join-Path $work 'other-name.manifest.json') @{ correspondingSource = [ordered]@{ file = 'sources.zip'; size = 300; sha256 = $sourceRecord.sha256 } }
    Assert-Throws { New-LucentReleaseManifest -Installer $installer -PackageManifest $otherName -AllowUnsignedInstaller -InstallerUrl $url -Channel stable -OutFile $bad } `
        'a corresponding-source archive under another name' 'corresponding-source archive'
    [System.IO.File]::WriteAllBytes($sourceArchive, [byte[]](1..300 | ForEach-Object { [byte]($_ % 5) }))
    Assert-Throws { New-LucentReleaseManifest -Installer $installer -PackageManifest $package -AllowUnsignedInstaller -InstallerUrl $url -Channel stable -OutFile $bad } `
        'a corresponding-source archive that differs from the recorded one' 'is not the corresponding-source archive'
    Remove-Item -LiteralPath $sourceArchive
    Assert-Throws { New-LucentReleaseManifest -Installer $installer -PackageManifest $package -AllowUnsignedInstaller -InstallerUrl $url -Channel stable -OutFile $bad } `
        'a corresponding-source archive missing beside the installer' 'must be beside the installer'
    Assert-True (-not (Test-Path -LiteralPath $bad)) 'nothing was written for a refused release'

    # 5. What Lucent's reader refuses, the verifier refuses too, although validly signed (SEC-06).
    $index = $written.IndexOf('"release_notes":')
    $variants = [ordered]@{
        'an unknown member' = $written.Replace('"release_notes":', '"mirror":1,"release_notes":')
        'schema_version as a string' = $written.Replace('"schema_version":1', '"schema_version":"1"')
        'minimum_os 10.0' = $written.Replace('"minimum_os":"10.0.17763"', '"minimum_os":"10.0"')
        'an impossible publication time' = $written.Replace('"published":"2026-10-09T12:00:00Z"', '"published":"2026-13-09T12:00:00Z"')
        'an installer of 0 bytes' = $written.Replace('"size":200', '"size":0')
        '20 KiB of release notes' = $written.Substring(0, $index) + '"release_notes":"' + ('x' * 20480) + '"}'
        'an extra installer member' = $written.Replace('},"release_notes":', ',"mirror":"https://x.example.org/a.exe"},"release_notes":')
        'whitespace' = $written.Replace(',"product":', ', "product":')
    }
    foreach ($name in $variants.Keys) {
        $variant = Join-Path $work 'variant.json'
        [System.IO.File]::WriteAllText($variant, $variants[$name], $utf8)
        Protect-LucentReleaseManifest -Manifest $variant -Key $key | Out-Null
        Assert-Throws { Test-LucentReleaseManifest -Manifest $variant -PublicKey $public.PublicKey } "a signed manifest with $name"
    }
    $loose = Join-Path $work 'loose.json'
    Copy-Item -LiteralPath $manifest -Destination $loose
    $signatureText = [System.IO.File]::ReadAllText($manifest + '.sig', $utf8)
    [System.IO.File]::WriteAllText($loose + '.sig', $signatureText.Replace('","signature":', '", "signature":'), $utf8)
    Assert-Throws { Test-LucentReleaseManifest -Manifest $loose -PublicKey $public.PublicKey } 'a signature file that is not canonical'
    Assert-True ((Test-LucentReleaseManifest -Manifest $manifest -PublicKey $public.PublicKey).Version -eq '0.2.0') 'the canonical release still verifies'
}
finally {
    if ($key) { $key.Dispose() }
    if ($other) { $other.Dispose() }
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ("{0} of {1} checks passed." -f ($script:checks - $script:failures), $script:checks)
if ($script:failures -gt 0) { exit 1 }
exit 0
