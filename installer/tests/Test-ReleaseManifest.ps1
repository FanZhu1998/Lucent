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
       version that is not SemVer, release notes above 16 KiB.

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
function Assert-Throws([scriptblock]$Block, [string]$What) {
    $script:checks++
    try { & $Block; $script:failures++; Write-Host "FAILED (no error): $What" } catch { }
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

    # 2. The canonical manifest of a stand-in installer.
    $installer = Join-Path $work 'Lucent-Setup-0.2.0-x64.exe'
    [System.IO.File]::WriteAllBytes($installer, [byte[]](1..200 | ForEach-Object { [byte]($_ % 251) }))
    $installerSha = (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash.ToLowerInvariant()
    $notes = "Faster previews.`nFixes `"quoted`" names, tabs`tand \ backslashes; caf" + [char]0xE9 + '.'
    $manifest = Join-Path $work 'release-manifest.json'
    $made = New-LucentReleaseManifest -Installer $installer -InstallerUrl 'https://updates.example.org/lucent/Lucent-Setup-0.2.0-x64.exe' `
        -Channel stable -ReleaseNotes $notes -Published ([datetime]::new(2026, 10, 9, 12, 0, 0, [DateTimeKind]::Utc)) -OutFile $manifest
    Assert-True ($made.Version -eq '0.2.0') 'the version comes from the installer name'
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
    Assert-Throws { New-LucentReleaseManifest -Installer $installer -InstallerUrl 'http://updates.example.org/x.exe' -Channel stable -OutFile $bad } 'an http URL'
    Assert-Throws { New-LucentReleaseManifest -Installer $installer -InstallerUrl 'https://updates.example.org/x.exe' -Channel 'Stable' -OutFile $bad } 'a channel name with capitals'
    Assert-Throws { New-LucentReleaseManifest -Installer $installer -InstallerUrl 'https://updates.example.org/x.exe' -Channel stable -Version 'v1.0' -OutFile $bad } 'a version that is not SemVer'
    Assert-Throws { New-LucentReleaseManifest -Installer $installer -InstallerUrl 'https://updates.example.org/x.exe' -Channel stable -ReleaseNotes ('x' * 16385) -OutFile $bad } 'release notes above 16 KiB'
    Assert-True (-not (Test-Path -LiteralPath $bad)) 'nothing was written for a refused release'
}
finally {
    if ($key) { $key.Dispose() }
    if ($other) { $other.Dispose() }
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ("{0} of {1} checks passed." -f ($script:checks - $script:failures), $script:checks)
if ($script:failures -gt 0) { exit 1 }
exit 0
