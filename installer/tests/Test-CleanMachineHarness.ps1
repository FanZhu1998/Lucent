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
    - The host's check of a gate.json (Test-CleanMachineResult) passes a complete result and refuses
      a missing or failed check, an unclean or online machine (unless a rehearsal), exports that do
      not match the photos, another installer, and a gate that says it failed.
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
    $gateText = [System.IO.File]::ReadAllText((Join-Path $repoRoot 'installer\sandbox\Invoke-CleanMachineGate.ps1'))
    $made = New-Object System.Collections.Generic.List[string]
    foreach ($match in [regex]::Matches($gateText, "Add-Check '([a-z0-9.-]+)'")) { $made.Add($match.Groups[1].Value) }
    foreach ($match in [regex]::Matches($gateText, "Test-Run '(run\d)'")) { foreach ($suffix in 'exit', 'screenshot', 'qt', 'assets') { $made.Add("$($match.Groups[1].Value).$suffix") } }
    foreach ($match in [regex]::Matches($gateText, "Test-Outputs '(run\d\.[a-z]+)'")) { $made.Add($match.Groups[1].Value) }
    $unknown = @($CleanMachineGateChecks | Where-Object { $made -notcontains $_ })
    Assert-That 'Every check the host requires is one the gate makes' ($unknown.Count -eq 0 -and $CleanMachineGateChecks.Count -ge 20) ($unknown -join ', ')

    # --- The host's check of a result --------------------------------------------------------------
    function New-Gate([switch]$Rehearsal) {
        $checks = @($CleanMachineGateChecks | ForEach-Object { [pscustomobject]@{ id = $_; name = "check $_"; passed = $true; detail = '' } })
        $exports = @(1..3 | ForEach-Object { [pscustomobject]@{ file = "photo$_.jpg"; bytes = 1000; sha256 = 'aa' } })
        return [pscustomobject]@{
            gate = 'clean-machine'; format = 1; mode = $(if ($Rehearsal) { 'rehearsal' } else { 'sandbox' }); passed = $true
            installer = [pscustomobject]@{ file = 'Lucent-Setup-0.1.0-x64.exe'; sha256 = 'abc' }
            fixtures = @(1..3 | ForEach-Object { [pscustomobject]@{ file = "fixtures\f$_\x.raw" } })
            runs = [pscustomobject]@{ run1 = [pscustomobject]@{ jpeg = $exports }; run2 = [pscustomobject]@{ tiff = $exports } }
            checks = $checks }
    }
    $good = New-Gate
    $result = Test-CleanMachineResult -Gate $good -Photos 3 -InstallerSha256 'abc'
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
}
finally {
    Remove-Item -LiteralPath $folder -Recurse -Force -ErrorAction SilentlyContinue
}

Write-Host ("{0} of {1} checks passed." -f ($checks - $failures), $checks)
if ($failures -gt 0) { exit 1 }
exit 0
