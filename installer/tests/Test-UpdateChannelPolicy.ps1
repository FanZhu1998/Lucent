<#
.SYNOPSIS
    Checks how tools\package.ps1 reads, compares and enforces the update channel a package's
    Lucent.exe checks (installer\scripts\LucentPackaging.psm1; W12 review SEC-02, ADR 0022).

.DESCRIPTION
    1. Key ids are the SHA-256 of each pinned public key's 65 bytes, as Lucent and
       LucentUpdate.psm1 compute them (an ephemeral key: nothing is stored).
    2. The build tree's record (lucent-package.json, updateChannel) and the staged Lucent.exe's
       start-up line ("update channel '<name>' (<URL>): keys ...; publishers ..." or "update channel:
       none") give the same channel record.
    3. The policy: a Setup must declare its channel (-UpdateChannel); 'none' needs a build without a
       channel; a name needs exactly that entry of the expected channels (URL, key ids, publisher
       thumbprints), which must name a publisher; -StageOnly only warns about an unlisted channel.
    4. installer\update-channels.json is a valid "lucent.update-channels" document; invalid entries
       are refused.

    Registered with CTest as lucent_update_channel_policy_tests (label "installer"). Exit code 0 when
    every check passed. Windows PowerShell 5.1 and PowerShell 7. Keep this file ASCII.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Import-Module (Join-Path $repoRoot 'installer\scripts\LucentPackaging.psm1') -Force
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

$work = Join-Path ([System.IO.Path]::GetTempPath()) ('lucent-update-channel-test-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work | Out-Null
$key = $null
try {
    # 1. Key ids.
    $key = [System.Security.Cryptography.CngKey]::Create([System.Security.Cryptography.CngAlgorithm]::ECDsaP256)
    $public = Get-LucentUpdatePublicKey -Key $key
    Assert-True ((Get-UpdateKeyId $public.PublicKey) -eq $public.KeyId) 'the key id is the one LucentUpdate.psm1 prints'
    Assert-True ((Get-UpdateKeyId $public.PublicKey.ToUpperInvariant()) -eq $public.KeyId) 'key ids ignore letter case'
    Assert-Throws { Get-UpdateKeyId '04abc' } 'a malformed key'

    # 2. The build tree's record and Lucent.exe's own report.
    $url = 'https://updates.example.org/lucent/stable/release-manifest.json'
    $thumbprint = 'ab' * 20
    $none = [pscustomobject]@{ updateChannel = [pscustomobject]@{ name = ''; manifestUrl = ''; publicKeys = @(); publisherThumbprints = @() } }
    $info = [pscustomobject]@{ updateChannel = [pscustomobject]@{ name = 'stable'; manifestUrl = $url
        publicKeys = @($public.PublicKey); publisherThumbprints = @($thumbprint.ToUpperInvariant()) } }
    Assert-True ($null -eq (Get-LucentUpdateChannelRecord -Info $none)) 'a build without a channel has no record'
    $compiled = Get-LucentUpdateChannelRecord -Info $info
    Assert-True ($compiled.name -eq 'stable' -and $compiled.manifestUrl -eq $url) 'the record names the channel and its URL'
    Assert-True ((@($compiled.keyIds) -join ',') -eq $public.KeyId) 'the record holds key ids'
    Assert-True ((@($compiled.publisherThumbprints) -join ',') -ceq $thumbprint) 'thumbprints in lowercase'

    $logged = ConvertFrom-UpdateChannelLogLine -Lines @('noise', "2026-10-09 12:00:00.000 info ui.backend: update channel 'stable' ($url): keys $($public.KeyId); publishers $thumbprint")
    Assert-True $logged.Found 'the configured line is found'
    Assert-True (@(Compare-UpdateChannel -Expected $compiled -Actual $logged.Channel).Count -eq 0) 'the logged channel equals the build tree''s'
    $parenthesised = ConvertFrom-UpdateChannelLogLine -Lines @("update channel 'beta' (https://x.example.org/a(1)/m.json): keys ; publishers none")
    Assert-True ($parenthesised.Found -and $parenthesised.Channel.manifestUrl -eq 'https://x.example.org/a(1)/m.json') 'a URL with parentheses'
    Assert-True (@($parenthesised.Channel.publisherThumbprints).Count -eq 0) 'publishers none'
    $loggedNone = ConvertFrom-UpdateChannelLogLine -Lines @('ui.backend: update channel: none (this build never checks for updates)')
    Assert-True ($loggedNone.Found -and $null -eq $loggedNone.Channel) 'the line of a build without a channel'
    Assert-True (-not (ConvertFrom-UpdateChannelLogLine -Lines @('something else')).Found) 'no line: not found'
    $otherKey = [pscustomobject]@{ updateChannel = [pscustomobject]@{ name = 'stable'; manifestUrl = $url
        publicKeys = @('04' + ('11' * 64)); publisherThumbprints = @($thumbprint) } }
    Assert-True (@(Compare-UpdateChannel -Expected $compiled -Actual (Get-LucentUpdateChannelRecord -Info $otherKey)).Count -eq 1) 'another key is a difference'
    Assert-True (@(Compare-UpdateChannel -Expected $compiled -Actual $null).Count -eq 1) 'no channel where one is expected'

    # 3. The policy.
    $expected = @(New-Object psobject -Property @{ name = 'stable'; manifestUrl = $url; keyIds = @($public.KeyId); publisherThumbprints = @($thumbprint) })
    $channelsFile = Join-Path $work 'update-channels.json'
    [System.IO.File]::WriteAllText($channelsFile, (ConvertTo-Json -Depth 5 -InputObject ([ordered]@{ schema = 'lucent.update-channels'; version = 1
        channels = @([ordered]@{ name = 'stable'; manifestUrl = $url; keyIds = @($public.KeyId); publisherThumbprints = @($thumbprint) },
                     [ordered]@{ name = 'unsigned'; manifestUrl = $url; keyIds = @($public.KeyId); publisherThumbprints = @() }) })))
    $expected = @(Read-ExpectedUpdateChannels -Path $channelsFile)
    Assert-True ($expected.Count -eq 2) 'the expected channels are read'
    $policy = { param($Compiled, $Declared, [switch]$StageOnly) Test-LucentUpdateChannelPolicy -Compiled $Compiled -Declared $Declared -Expected $expected -StageOnly:$StageOnly }
    Assert-True ((& $policy $null '' -StageOnly).Errors.Count -eq 0) 'a staged folder need not declare its channel'
    Assert-True ((& $policy $null '').Errors.Count -eq 1) 'a Setup must declare its channel'
    Assert-True ((& $policy $null 'none').Errors.Count -eq 0) "'none' for a build without a channel"
    Assert-True ((& $policy $compiled 'none').Errors.Count -eq 1) "'none' for a build that checks a channel"
    Assert-True ((& $policy $compiled 'stable').Errors.Count -eq 0) 'exactly the expected channel'
    Assert-True ((& $policy $null 'stable').Errors.Count -eq 1) 'a named channel for a build without one'
    Assert-True ((& $policy $compiled 'beta').Errors.Count -ge 1) 'a channel the file does not list'
    Assert-True ((& $policy (Get-LucentUpdateChannelRecord -Info $otherKey) 'stable').Errors.Count -eq 1) 'another release-manifest key'
    $otherUrl = [pscustomobject]@{ updateChannel = [pscustomobject]@{ name = 'stable'; manifestUrl = 'https://evil.example.org/m.json'
        publicKeys = @($public.PublicKey); publisherThumbprints = @($thumbprint) } }
    Assert-True ((& $policy (Get-LucentUpdateChannelRecord -Info $otherUrl) 'stable').Errors.Count -eq 1) 'another manifest URL'
    $noPublisher = [pscustomobject]@{ updateChannel = [pscustomobject]@{ name = 'unsigned'; manifestUrl = $url
        publicKeys = @($public.PublicKey); publisherThumbprints = @() } }
    $result = & $policy (Get-LucentUpdateChannelRecord -Info $noPublisher) 'unsigned'
    Assert-True ($result.Errors.Count -eq 1 -and $result.Errors[0] -match 'names no publisher') 'a release channel must name a publisher'
    $staged = & $policy (Get-LucentUpdateChannelRecord -Info $otherUrl) '' -StageOnly
    Assert-True ($staged.Errors.Count -eq 0 -and $staged.Warnings.Count -eq 1) 'an unlisted channel in a staged folder is a warning'

    # 4. The committed file, and invalid entries.
    $committed = @(Read-ExpectedUpdateChannels -Path (Join-Path $repoRoot 'installer\update-channels.json'))
    Assert-True ($null -ne $committed) 'installer\update-channels.json is valid'
    $bad = Join-Path $work 'bad.json'
    foreach ($entry in @(
            [ordered]@{ name = 'Stable'; manifestUrl = $url; keyIds = @(); publisherThumbprints = @() },
            [ordered]@{ name = 'stable'; manifestUrl = 'http://updates.example.org/m.json'; keyIds = @(); publisherThumbprints = @() },
            [ordered]@{ name = 'stable'; manifestUrl = $url; keyIds = @('XYZ'); publisherThumbprints = @() },
            [ordered]@{ name = 'stable'; manifestUrl = $url; keyIds = @(); publisherThumbprints = @('abc') })) {
        [System.IO.File]::WriteAllText($bad, (ConvertTo-Json -Depth 5 -InputObject ([ordered]@{ schema = 'lucent.update-channels'; version = 1; channels = @($entry) })))
        Assert-Throws { Read-ExpectedUpdateChannels -Path $bad } "an invalid entry: $($entry | ConvertTo-Json -Compress)"
    }
}
finally {
    if ($key) { $key.Dispose() }
    Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ("{0} of {1} checks passed." -f ($script:checks - $script:failures), $script:checks)
if ($script:failures -gt 0) { exit 1 }
exit 0
