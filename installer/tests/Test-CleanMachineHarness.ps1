<#
.SYNOPSIS
    Self-test of the clean-machine gate's harness (installer\sandbox, tools\Test-CleanMachine.ps1;
    docs\release\clean-machine.md) that needs neither Windows Sandbox nor a package.

.DESCRIPTION
    - The Windows Sandbox configuration the gate writes is well-formed, valid against
      installer\sandbox\wsb-configuration.xsd (Microsoft's documented elements and values) and keeps
      the gate's rules: networking disabled, the input read-only, the results writable, the logon
      command running the gate. Configurations that break the schema (an unknown element, a value
      outside Enable/Disable/Default, a relative folder, malformed XML) or a rule (networking on, a
      writable input, a third folder, another logon command) are refused.
    - The gate's scripts parse in Windows PowerShell 5.1 and are ASCII; every check id the host
      requires is one the gate makes.
    - The gate's helpers (Invoke-CleanMachineGate.ps1, loaded on their own): a log line it keeps is a
      plain JSON string, where a Get-Content line serialises with PowerShell's provider properties (W12
      sandbox run SBX-1), and the gate reads no log with Get-Content; a result beyond 1 MiB is refused;
      the TIFF reader takes a 16-bit RGB TIFF in either byte order and tells an 8-bit, a 4-sample, a
      planar, a truncated file and a BigTIFF apart (CMG-04); the elevation record names the token and
      the UAC settings (CMG-01).
    - The host's check of a gate.json (Test-CleanMachineResult) passes a complete result and refuses
      a missing or failed check, an unclean or online machine (unless a rehearsal), exports that do
      not match the photos (an empty object or null for one photo's export included, CMG-03), other
      photos than the host gave, a version rehearsal without the upgrade and refused downgrade,
      another installer, and a gate that says it failed.
    - Reading a result the sandbox may still be writing (Read-CleanMachineJson, SBX-2): a file another
      process holds open is read once it is closed; one held past the timeout, and incomplete JSON,
      fail with the reason. "wsb list" output gives the running sandboxes' ids (CMG-02).
    - tools\Test-CleanMachine.ps1 -Check writes the host's verdict, host-check.json, beside
      gate-request.json and not into the results folder the sandbox can write, also when there is no
      gate.json (CMG-03).
    Exit code 0 when every check passes.

.EXAMPLE
    powershell -NoProfile -File installer\tests\Test-CleanMachineHarness.ps1

.NOTES
    Windows PowerShell 5.1 compatible. Keep this file ASCII.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 3.0
$installerDir = Split-Path -Parent $PSScriptRoot
$repoRoot = Split-Path -Parent $installerDir
Import-Module (Join-Path $installerDir 'sandbox\LucentSandbox.psm1') -Force

$failures = 0
$checks = 0
function Assert-That([string]$Name, [bool]$Condition, [string]$Detail = '') {
    $script:checks++
    if ($Condition) { Write-Host "[PASS] $Name" -ForegroundColor Green }
    else { $script:failures++; Write-Host "[FAIL] $Name$(if ($Detail) { " - $Detail" })" -ForegroundColor Red }
}

$folder = Join-Path ([System.IO.Path]::GetTempPath()) ('lucent-gate-harness-' + [guid]::NewGuid().ToString('N'))
$inputDir = Join-Path $folder 'input'
$resultsDir = Join-Path $folder 'results'
New-Item -ItemType Directory -Path $inputDir, $resultsDir | Out-Null
$utf8 = New-Object System.Text.UTF8Encoding($false)
try {
    # --- The configuration -------------------------------------------------------------------------
    $wsb = Join-Path $folder 'gate.wsb'
    $text = New-LucentSandboxConfiguration -InputDir $inputDir -ResultsDir $resultsDir -MemoryMB 4096 -GateArguments @('-Shutdown')
    [System.IO.File]::WriteAllText($wsb, $text)
    $result = Test-LucentSandboxConfiguration -Path $wsb -InputDir $inputDir -ResultsDir $resultsDir
    Assert-That 'The gate''s configuration is valid (schema and rules)' $result.Valid (@($result.Problems) -join '; ')
    [xml]$document = $text
    Assert-That 'Networking, the virtual GPU, the clipboard and printers are off' `
        ($document.Configuration.Networking -eq 'Disable' -and $document.Configuration.vGPU -eq 'Disable' -and
         $document.Configuration.ClipboardRedirection -eq 'Disable' -and $document.Configuration.PrinterRedirection -eq 'Disable')
    $mapped = @($document.Configuration.MappedFolders.MappedFolder)
    Assert-That 'The input is mapped read-only and the results writable' `
        ($mapped.Count -eq 2 -and $mapped[0].HostFolder -eq $inputDir -and $mapped[0].ReadOnly -eq 'true' -and
         $mapped[1].HostFolder -eq $resultsDir -and $mapped[1].ReadOnly -eq 'false')
    Assert-That 'The logon command runs the gate from the read-only input, and shuts down after it' `
        ($document.Configuration.LogonCommand.Command -eq 'powershell.exe -NoProfile -ExecutionPolicy Bypass -File C:\LucentGate\input\scripts\Invoke-CleanMachineGate.ps1 -Root C:\LucentGate -Shutdown')
    $manual = Join-Path $folder 'manual.wsb'
    [System.IO.File]::WriteAllText($manual, (New-LucentSandboxConfiguration -InputDir $inputDir -ResultsDir $resultsDir -Manual))
    $result = Test-LucentSandboxConfiguration -Path $manual -Manual
    Assert-That 'The manual configuration (Explorer at the input) is valid' $result.Valid (@($result.Problems) -join '; ')

    $refused = @(
        @('an unknown element', '<Networking>Disable</Networking>', '<Networking>Disable</Networking><Network>Disable</Network>', 'schema'),
        @('a value outside Enable, Disable and Default', '<vGPU>Disable</vGPU>', '<vGPU>Off</vGPU>', 'schema'),
        @('a misspelled element', '<AudioInput>Disable</AudioInput>', '<Audioinput>Disable</Audioinput>', 'schema'),
        @('a relative host folder', "<HostFolder>$([System.Security.SecurityElement]::Escape($resultsDir))</HostFolder>", '<HostFolder>results</HostFolder>', 'schema'),
        @('a memory size that is not a number', '<MemoryInMB>4096</MemoryInMB>', '<MemoryInMB>lots</MemoryInMB>', 'schema'),
        @('malformed XML', '</Configuration>', '</Configuratio>', 'well-formed'),
        @('networking enabled', '<Networking>Disable</Networking>', '<Networking>Enable</Networking>', 'networking'),
        @('networking left at its default', '<Networking>Disable</Networking>', '', 'networking'),
        @('a writable input', '<ReadOnly>true</ReadOnly>', '<ReadOnly>false</ReadOnly>', 'read-only'),
        @('another logon command', 'Invoke-CleanMachineGate.ps1', 'Other.ps1', 'logon command'),
        @('a third mapped folder', '  </MappedFolders>', "    <MappedFolder><HostFolder>$([System.Security.SecurityElement]::Escape($folder))</HostFolder><SandboxFolder>C:\Extra</SandboxFolder></MappedFolder>`r`n  </MappedFolders>", 'exactly two'))
    foreach ($case in $refused) {
        $bad = Join-Path $folder 'bad.wsb'
        $index = $text.IndexOf($case[1])
        if ($index -lt 0) { Assert-That "Refused: $($case[0])" $false "the configuration has no '$($case[1])'"; continue }
        [System.IO.File]::WriteAllText($bad, $text.Substring(0, $index) + $case[2] + $text.Substring($index + $case[1].Length))
        $result = Test-LucentSandboxConfiguration -Path $bad -InputDir $inputDir -ResultsDir $resultsDir
        Assert-That "Refused: $($case[0])" (-not $result.Valid -and @($result.Problems | Where-Object { $_ -match [regex]::Escape($case[3]) }).Count -gt 0) (@($result.Problems) -join '; ')
    }
    $threw = $false
    try { New-LucentSandboxConfiguration -InputDir 'relative\input' -ResultsDir $resultsDir | Out-Null } catch { $threw = $true }
    Assert-That 'A relative folder is refused before any configuration is written' $threw

    # --- The scripts -------------------------------------------------------------------------------
    $scripts = @('installer\sandbox\Invoke-CleanMachineGate.ps1', 'installer\sandbox\LucentSandbox.psm1', 'installer\tests\Test-InstalledPackage.ps1',
        'tools\Test-CleanMachine.ps1')
    foreach ($relative in $scripts) {
        $path = Join-Path $repoRoot $relative
        $errors = $null
        [void][System.Management.Automation.Language.Parser]::ParseFile($path, [ref]$null, [ref]$errors)
        $bytes = [System.IO.File]::ReadAllBytes($path)
        $nonAscii = @($bytes | Where-Object { $_ -gt 0x7E -or ($_ -lt 0x20 -and $_ -ne 0x0A -and $_ -ne 0x0D -and $_ -ne 0x09) }).Count
        Assert-That "$relative parses in Windows PowerShell $($PSVersionTable.PSVersion.Major).$($PSVersionTable.PSVersion.Minor) and is ASCII" `
            (@($errors).Count -eq 0 -and $nonAscii -eq 0) ("{0} parse error(s){1}; {2} non-ASCII byte(s)" -f @($errors).Count, $(if ($errors) { ': ' + $errors[0].Message } else { '' }), $nonAscii)
    }
    $gateScript = Join-Path $repoRoot 'installer\sandbox\Invoke-CleanMachineGate.ps1'
    $gateText = [System.IO.File]::ReadAllText($gateScript)
    $made = New-Object System.Collections.Generic.List[string]
    foreach ($match in [regex]::Matches($gateText, "Add-Check '([a-z0-9.-]+)'")) { $made.Add($match.Groups[1].Value) }
    foreach ($match in [regex]::Matches($gateText, "Test-Run '(run\d)'")) { foreach ($suffix in 'exit', 'screenshot', 'qt', 'assets') { $made.Add("$($match.Groups[1].Value).$suffix") } }
    foreach ($match in [regex]::Matches($gateText, "Test-Outputs '(run\d\.[a-z]+)'")) { $made.Add($match.Groups[1].Value) }
    $unknown = @($CleanMachineGateChecks | Where-Object { $made -notcontains $_ })
    Assert-That 'Every check the host requires is one the gate makes' ($unknown.Count -eq 0 -and $CleanMachineGateChecks.Count -ge 20) ($unknown -join ', ')

    # --- The gate's helpers, loaded on their own ---------------------------------------------------
    $parseErrors = $null
    $gateAst = [System.Management.Automation.Language.Parser]::ParseFile($gateScript, [ref]$null, [ref]$parseErrors)
    foreach ($name in 'ConvertTo-GateJsonText', 'Read-TextLines', 'Get-TiffLayout', 'Get-GateElevation') {
        $definition = @($gateAst.FindAll({ param($node) $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name }, $true))
        Assert-That "The gate defines $name" ($definition.Count -eq 1)
        if ($definition.Count -eq 1) { . ([scriptblock]::Create($definition[0].Extent.Text)) }
    }

    # SBX-1: a log line the result keeps is a plain string.
    $log = Join-Path $folder 'run1-stderr.log'
    [System.IO.File]::WriteAllText($log, "lucent: auto: started on a.dng`r`nlucent: auto: Revision 1 by Auto; review recommended: no`r`n", $utf8)
    # At depth 2 only: at the gate's depth of 8 one such line takes minutes and gigabytes to serialise.
    $decorated = ConvertTo-Json -InputObject ([ordered]@{ auto = @(Get-Content -LiteralPath $log -Encoding UTF8)[1] }) -Depth 2
    Assert-That 'SBX-1: a Get-Content line serialises with PowerShell''s provider properties (what made gate.json 109 MB)' ($decorated -match '"PSPath"') "$($decorated.Length) characters at depth 2"
    $lines = Read-TextLines $log
    $plain = ConvertTo-GateJsonText ([ordered]@{ auto = $lines[1]; lines = @($lines) })
    Assert-That 'SBX-1: a line read by the gate (Read-TextLines) is a plain JSON string' `
        ($plain -notmatch 'PSPath' -and ($plain | ConvertFrom-Json).auto -ceq 'lucent: auto: Revision 1 by Auto; review recommended: no' -and $plain.Length -lt 400) $plain
    $none = Read-TextLines (Join-Path $folder 'missing.log')
    Assert-That 'SBX-1: a missing log reads as no lines' ($none -is [string[]] -and $none.Count -eq 0)
    $readers = @($gateAst.FindAll({ param($node) $node -is [System.Management.Automation.Language.CommandAst] -and $node.GetCommandName() -in 'Get-Content', 'gc', 'cat', 'type' }, $true))
    Assert-That 'SBX-1: the gate reads no file with Get-Content' ($readers.Count -eq 0) (@($readers | ForEach-Object { $_.Extent.Text }) -join ' | ')
    $threw = $false
    try { ConvertTo-GateJsonText ([ordered]@{ big = ('x' * 2000) }) -Limit 1000 | Out-Null } catch { $threw = $_.Exception.Message -match 'more than 1000' }
    Assert-That 'SBX-1: a result beyond the size limit is refused, not written' $threw

    # CMG-04: the TIFF reader.
    function New-TestTiff([string]$Order = 'II', [int[]]$Bits = @(16, 16, 16), [int]$Samples = 3, [int]$Planar = 1, [int]$Version = 42) {
        $little = $Order -eq 'II'
        $bytes = New-Object byte[] 2048
        $put16 = { param([int]$At, [int]$Value) if ($little) { $bytes[$At] = $Value -band 0xFF; $bytes[$At + 1] = ($Value -shr 8) -band 0xFF } else { $bytes[$At] = ($Value -shr 8) -band 0xFF; $bytes[$At + 1] = $Value -band 0xFF } }
        $put32 = { param([int]$At, [int64]$Value) if ($little) { & $put16 $At ($Value -band 0xFFFF); & $put16 ($At + 2) (($Value -shr 16) -band 0xFFFF) } else { & $put16 $At (($Value -shr 16) -band 0xFFFF); & $put16 ($At + 2) ($Value -band 0xFFFF) } }
        $bytes[0] = [byte][char]$Order[0]; $bytes[1] = [byte][char]$Order[1]
        & $put16 2 $Version
        & $put32 4 8
        $entries = @(@(256, 3, 1, 10), @(257, 3, 1, 10), @(258, 3, $Bits.Count, 0), @(277, 3, 1, $Samples), @(284, 3, 1, $Planar))
        & $put16 8 $entries.Count
        $extra = 200
        for ($i = 0; $i -lt $entries.Count; $i++) {
            $at = 10 + 12 * $i
            & $put16 $at $entries[$i][0]; & $put16 ($at + 2) $entries[$i][1]; & $put32 ($at + 4) $entries[$i][2]
            if ($entries[$i][0] -eq 258) {
                if ($Bits.Count -le 2) { for ($k = 0; $k -lt $Bits.Count; $k++) { & $put16 ($at + 8 + 2 * $k) $Bits[$k] } }
                else { & $put32 ($at + 8) $extra; for ($k = 0; $k -lt $Bits.Count; $k++) { & $put16 ($extra + 2 * $k) $Bits[$k] } }
            }
            else { & $put16 ($at + 8) $entries[$i][3] }
        }
        return $bytes
    }
    foreach ($order in 'II', 'MM') {
        $layout = Get-TiffLayout (New-TestTiff -Order $order)
        Assert-That "CMG-04: a 16-bit RGB TIFF ($order) reads as BitsPerSample 16,16,16, SamplesPerPixel 3, chunky" `
            (-not $layout.Problem -and (@($layout.BitsPerSample) -join ',') -eq '16,16,16' -and $layout.SamplesPerPixel -eq 3 -and $layout.PlanarConfiguration -eq 1 -and $layout.ByteOrder -eq $order) `
            ("{0}; {1}; {2}; {3}" -f $layout.Problem, (@($layout.BitsPerSample) -join ','), $layout.SamplesPerPixel, $layout.PlanarConfiguration)
    }
    $eight = Get-TiffLayout (New-TestTiff -Bits @(8, 8, 8))
    Assert-That 'CMG-04: an 8-bit TIFF reads as 8-bit (the gate refuses it)' (-not $eight.Problem -and (@($eight.BitsPerSample) -join ',') -eq '8,8,8') (@($eight.BitsPerSample) -join ',')
    $rgba = Get-TiffLayout (New-TestTiff -Bits @(16, 16, 16, 16) -Samples 4)
    Assert-That 'CMG-04: a 16-bit RGBA TIFF reads as four samples' (-not $rgba.Problem -and $rgba.SamplesPerPixel -eq 4 -and @($rgba.BitsPerSample).Count -eq 4)
    $planar = Get-TiffLayout (New-TestTiff -Planar 2)
    Assert-That 'CMG-04: a planar TIFF reads as PlanarConfiguration 2' (-not $planar.Problem -and $planar.PlanarConfiguration -eq 2)
    $short = (New-TestTiff)[0..60]
    Assert-That 'CMG-04: a truncated TIFF is not one' ([bool](Get-TiffLayout ([byte[]]$short)).Problem) (Get-TiffLayout ([byte[]]$short)).Problem
    Assert-That 'CMG-04: a BigTIFF is not a classic TIFF' ([string](Get-TiffLayout (New-TestTiff -Version 43)).Problem -match 'not 42')
    Assert-That 'CMG-04: a JPEG is not a TIFF' ([bool](Get-TiffLayout ([byte[]](0xFF, 0xD8, 0xFF, 0xE0, 0, 0, 0, 0, 0, 0))).Problem)
    $gateTiffCheck = $gateText -match "SamplesPerPixel -ne 3" -and $gateText -match "-join ','\) -ne '16,16,16'"
    Assert-That 'CMG-04: the gate requires 16, 16, 16 and three samples of every exported TIFF' $gateTiffCheck

    # CMG-01: the elevation record.
    $elevation = Get-GateElevation
    Assert-That 'CMG-01: the elevation record names the token and the UAC settings' `
        ($elevation.Contains('administratorsRole') -and $elevation.Contains('integrityLevel') -and $elevation.Contains('elevationType') -and
         $elevation.uac.Contains('EnableLUA') -and $elevation.uac.Contains('ConsentPromptBehaviorAdmin') -and [string]$elevation.runtimePrompt) `
        ("integrity {0}, elevation type {1}, EnableLUA {2}, ConsentPromptBehaviorAdmin {3}: {4}" -f $elevation.integrityLevel, $elevation.elevationType,
            $elevation.uac['EnableLUA'], $elevation.uac['ConsentPromptBehaviorAdmin'], $elevation.runtimePrompt)
    Assert-That 'CMG-01: the elevation record serialises as plain data' ((ConvertTo-GateJsonText $elevation).Length -lt 2000)

    # --- The host's check of a result --------------------------------------------------------------
    $hash = 'ab' * 32
    function New-Gate([switch]$Rehearsal, [int]$Photos = 3, [switch]$NewerInstaller) {
        $checks = @($CleanMachineGateChecks | ForEach-Object { [pscustomobject]@{ id = $_; name = "check $_"; passed = $true; detail = '' } })
        if ($NewerInstaller) { ($checks | Where-Object { $_.id -eq 'installer-test' }).name = 'Test-InstalledPackage.ps1 -RuntimeOutcomes -NewerInstaller passed' }
        $exports = @(1..$Photos | ForEach-Object { [pscustomobject]@{ file = "photo$_.jpg"; bytes = 1000; sha256 = $hash } })
        return [pscustomobject]@{
            gate = 'clean-machine'; format = 1; mode = $(if ($Rehearsal) { 'rehearsal' } else { 'sandbox' }); passed = $true
            installer = [pscustomobject]@{ file = 'Lucent-Setup-0.1.0-x64.exe'; sha256 = 'abc' }
            fixtures = @(1..$Photos | ForEach-Object { [pscustomobject]@{ file = "fixtures\f$_\x$_.raw" } })
            runs = [pscustomobject]@{ run1 = [pscustomobject]@{ jpeg = $exports }; run2 = [pscustomobject]@{ tiff = $exports } }
            notes = @()
            checks = $checks }
    }
    $good = New-Gate
    $result = Test-CleanMachineResult -Gate $good -Photos 3 -Fixtures @('x1.raw', 'x2.raw', 'x3.raw') -InstallerSha256 'abc'
    Assert-That 'Result: a complete gate passes' $result.Passed (@($result.Problems) -join '; ')
    $gate = New-Gate; $gate.checks = @($gate.checks | Where-Object { $_.id -ne 'run1.auto' })
    $result = Test-CleanMachineResult -Gate $gate -Photos 3
    Assert-That 'Result: a missing check fails' (-not $result.Passed -and ($result.Problems -join ' ') -match "did not report 'run1.auto'") (@($result.Problems) -join '; ')
    $gate = New-Gate; ($gate.checks | Where-Object { $_.id -eq 'run2.tiff' }).passed = $false
    $result = Test-CleanMachineResult -Gate $gate -Photos 3
    Assert-That 'Result: a failed check fails' (-not $result.Passed -and ($result.Problems -join ' ') -match 'failed: check run2.tiff') (@($result.Problems) -join '; ')
    $gate = New-Gate; ($gate.checks | Where-Object { $_.id -eq 'environment.offline' }).passed = $false
    $result = Test-CleanMachineResult -Gate $gate -Photos 3
    Assert-That 'Result: an online machine fails the gate' (-not $result.Passed) (@($result.Problems) -join '; ')
    $gate = New-Gate -Rehearsal; ($gate.checks | Where-Object { $_.id -like 'environment.*' }) | ForEach-Object { $_.passed = $false }
    $result = Test-CleanMachineResult -Gate $gate -Photos 3 -Rehearsal
    Assert-That 'Result: a local rehearsal records the unclean, online machine without failing' $result.Passed (@($result.Problems) -join '; ')
    $result = Test-CleanMachineResult -Gate (New-Gate -Rehearsal) -Photos 3
    Assert-That 'Result: a rehearsal is not taken for the gate' (-not $result.Passed) (@($result.Problems) -join '; ')
    $result = Test-CleanMachineResult -Gate (New-Gate) -Photos 4
    Assert-That 'Result: exports that do not match the photos fail' (-not $result.Passed -and ($result.Problems -join ' ') -match '4 photo\(s\) given') (@($result.Problems) -join '; ')
    $result = Test-CleanMachineResult -Gate (New-Gate) -Photos 3 -InstallerSha256 'def'
    Assert-That 'Result: another installer fails' (-not $result.Passed) (@($result.Problems) -join '; ')
    $gate = New-Gate; $gate.passed = $false
    $result = Test-CleanMachineResult -Gate $gate -Photos 3
    Assert-That 'Result: a gate that says it failed fails' (-not $result.Passed) (@($result.Problems) -join '; ')
    $gate = New-Gate; $gate.checks = @($gate.checks | Where-Object { $_.id -ne 'installer-test' })
    $result = Test-CleanMachineResult -Gate $gate -Photos 3 -SkipInstallerTest
    Assert-That 'Result: without the installer test (-SkipInstallerTest) its check is not required' $result.Passed (@($result.Problems) -join '; ')
    $gate = New-Gate; $gate.checks = @($gate.checks) + @([pscustomobject]@{ id = 'gate.error'; name = 'The gate ran to its end'; passed = $false; detail = 'boom' })
    $result = Test-CleanMachineResult -Gate $gate -Photos 3
    Assert-That 'Result: an error of the gate fails' (-not $result.Passed -and ($result.Problems -join ' ') -match 'boom') (@($result.Problems) -join '; ')

    # CMG-03: one photo, its exports as an older gate wrote them for no file ({} or null), through JSON.
    $one = New-Gate -Photos 1
    Assert-That 'CMG-03: one photo with one recorded export passes' (Test-CleanMachineResult -Gate $one -Photos 1).Passed
    foreach ($empty in '{ }', 'null') {
        $json = (ConvertTo-Json -InputObject (New-Gate -Photos 1) -Depth 8) -replace '"jpeg":\s*\{[^{}]*\}', ('"jpeg": ' + $empty) -replace '"jpeg":\s*\[[^\[\]]*\]', ('"jpeg": ' + $empty)
        $parsed = $json | ConvertFrom-Json
        $result = Test-CleanMachineResult -Gate $parsed -Photos 1
        Assert-That "CMG-03: a JPEG export recorded as $empty is no export (one photo)" (-not $result.Passed -and ($result.Problems -join ' ') -match '0 JPEG') (@($result.Problems) -join '; ')
    }
    $gate = New-Gate -Photos 1; $gate.runs.run2.tiff = @([pscustomobject]@{ file = 'photo1.tif'; bytes = 1000 })
    $result = Test-CleanMachineResult -Gate $gate -Photos 1
    Assert-That 'CMG-03: an export recorded without its SHA-256 is no export' (-not $result.Passed -and ($result.Problems -join ' ') -match '0 TIFF') (@($result.Problems) -join '; ')
    $result = Test-CleanMachineResult -Gate (New-Gate) -Photos 3 -Fixtures @('x1.raw', 'x2.raw', 'other.raw')
    Assert-That 'CMG-03: other photos than the host gave fail' (-not $result.Passed -and ($result.Problems -join ' ') -match 'not the ones the host gave') (@($result.Problems) -join '; ')
    $result = Test-CleanMachineResult -Gate (New-Gate) -Photos 3 -Fixtures @($null)
    Assert-That 'CMG-03: a request without file names (an older gate-request.json) compares the count only' $result.Passed (@($result.Problems) -join '; ')
    $result = Test-CleanMachineResult -Gate (New-Gate) -Photos 3 -VersionRehearsal
    Assert-That 'CMG-03: a version rehearsal given but the installer test without -NewerInstaller fails' (-not $result.Passed -and ($result.Problems -join ' ') -match 'without -NewerInstaller') (@($result.Problems) -join '; ')
    $result = Test-CleanMachineResult -Gate (New-Gate -NewerInstaller) -Photos 3 -VersionRehearsal
    Assert-That 'CMG-03: a version rehearsal given and the installer test with -NewerInstaller passes' $result.Passed (@($result.Problems) -join '; ')

    # --- Reading what the sandbox writes (SBX-2) and the running sandboxes (CMG-02) -----------------
    $jsonPath = Join-Path $folder 'gate.json'
    [System.IO.File]::WriteAllText($jsonPath, '{ "gate": "clean-machine", "passed": true }', $utf8)
    $locked = [System.IO.File]::Open($jsonPath, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
    $message = ''
    try { Read-CleanMachineJson -Path $jsonPath -TimeoutSeconds 1 -IntervalMilliseconds 200 | Out-Null } catch { $message = $_.Exception.Message }
    finally { $locked.Dispose() }
    Assert-That 'SBX-2: a result another process holds open past the timeout fails with the reason' ($message -match 'could not be read within 1 s' -and $message -match 'another process') $message
    $marker = Join-Path $folder 'locked.txt'
    $holder = Start-Job -ArgumentList $jsonPath, $marker -ScriptBlock {
        param($Path, $Marker)
        $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
        [System.IO.File]::WriteAllText($Marker, 'locked')
        Start-Sleep -Seconds 3
        $stream.Dispose()
    }
    try {
        $wait = [System.Diagnostics.Stopwatch]::StartNew()
        while (-not (Test-Path -LiteralPath $marker) -and $wait.Elapsed.TotalSeconds -lt 60) { Start-Sleep -Milliseconds 100 }
        $read = $null; $message = ''
        $clock = [System.Diagnostics.Stopwatch]::StartNew()
        try { $read = Read-CleanMachineJson -Path $jsonPath -TimeoutSeconds 60 -IntervalMilliseconds 200 } catch { $message = $_.Exception.Message }
        Assert-That 'SBX-2: a result held open by another process is read once it is closed' ((Test-Path -LiteralPath $marker) -and $read -and $read.gate -eq 'clean-machine' -and $clock.Elapsed.TotalSeconds -ge 1) `
            ("{0:N1} s; {1}" -f $clock.Elapsed.TotalSeconds, $message)
    }
    finally { Wait-Job $holder -Timeout 60 | Out-Null; Remove-Job $holder -Force }
    [System.IO.File]::WriteAllText($jsonPath, '{ "gate": "clean-mach', $utf8)
    $message = ''
    try { Read-CleanMachineJson -Path $jsonPath -TimeoutSeconds 1 -IntervalMilliseconds 200 | Out-Null } catch { $message = $_.Exception.Message }
    Assert-That 'SBX-2: an incomplete result fails after the timeout as incomplete JSON' ($message -match 'not complete JSON') $message
    $ids = @(ConvertFrom-WsbListOutput @('[', '  "0E2D1C3B-4A59-6877-8695-A4B3C2D1E0F9",', '  "0e2d1c3b-4a59-6877-8695-a4b3c2d1e0f9"', ']'))
    Assert-That 'CMG-02: "wsb list --raw" gives the running sandbox''s id once' ($ids.Count -eq 1 -and $ids[0] -eq '0e2d1c3b-4a59-6877-8695-a4b3c2d1e0f9') ($ids -join ', ')
    Assert-That 'CMG-02: no sandbox in an empty list' (@(ConvertFrom-WsbListOutput @('[]')).Count -eq 0 -and @(ConvertFrom-WsbListOutput @()).Count -eq 0)

    # --- tools\Test-CleanMachine.ps1 -Check: the host's verdict beside the request (CMG-03) ----------
    $powershell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    $host1 = Join-Path $folder 'gate-checked'
    New-Item -ItemType Directory -Path (Join-Path $host1 'results') -Force | Out-Null
    [System.IO.File]::WriteAllText((Join-Path $host1 'gate-request.json'), (ConvertTo-Json -InputObject ([ordered]@{
        gate = 'clean-machine'; mode = 'sandbox'; installer = 'Lucent-Setup-0.1.0-x64.exe'; installerSha256 = 'abc'; versionRehearsal = $null
        photos = 3; fixtures = @('x1.raw', 'x2.raw', 'x3.raw'); skipInstallerTest = $false }) -Depth 4), $utf8)
    [System.IO.File]::WriteAllText((Join-Path $host1 'results\gate.json'), (ConvertTo-Json -InputObject (New-Gate) -Depth 8), $utf8)
    & $powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $repoRoot 'tools\Test-CleanMachine.ps1') -Check $host1 | Out-Null
    $exit = $LASTEXITCODE
    $verdictPath = Join-Path $host1 'host-check.json'
    $verdict = if (Test-Path -LiteralPath $verdictPath) { [System.IO.File]::ReadAllText($verdictPath) | ConvertFrom-Json } else { $null }
    Assert-That 'CMG-03: -Check passes a complete result and writes host-check.json beside gate-request.json, not into results\' `
        ($exit -eq 0 -and $verdict -and [bool]$verdict.passed -and $verdict.outcome -eq 'checked' -and -not (Test-Path -LiteralPath (Join-Path $host1 'results\host-check.json'))) "exit code $exit"
    Assert-That 'CMG-02: the host''s verdict records the host''s Windows build' ($verdict -and $verdict.host -and [string]$verdict.host.build)
    $host2 = Join-Path $folder 'gate-missing'
    New-Item -ItemType Directory -Path (Join-Path $host2 'results') -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $host1 'gate-request.json') -Destination $host2
    & $powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -File (Join-Path $repoRoot 'tools\Test-CleanMachine.ps1') -Check $host2 | Out-Null
    $exit = $LASTEXITCODE
    $verdictPath = Join-Path $host2 'host-check.json'
    $verdict = if (Test-Path -LiteralPath $verdictPath) { [System.IO.File]::ReadAllText($verdictPath) | ConvertFrom-Json } else { $null }
    Assert-That 'CMG-03: without gate.json -Check fails and still writes a failed host-check.json' ($exit -eq 1 -and $verdict -and -not [bool]$verdict.passed -and $verdict.outcome -eq 'no result') "exit code $exit"
}
finally {
    Remove-Item -LiteralPath $folder -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ("{0} of {1} checks passed." -f ($checks - $failures), $checks)
if ($failures -gt 0) { exit 1 }
exit 0
