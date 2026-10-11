#Requires -Version 5.1
<#
.SYNOPSIS
    The clean-machine gate's host side (tools\Test-CleanMachine.ps1, docs\release\clean-machine.md;
    blueprint section 10, "Offline installation").

.DESCRIPTION
    - New-LucentSandboxConfiguration   the Windows Sandbox configuration (.wsb) of a prepared gate
                                       folder: networking, the virtual GPU, the clipboard, printers,
                                       audio and video input off; the input folder read-only at
                                       C:\LucentGate\input, the results folder writable at
                                       C:\LucentGate\results; at logon the gate
                                       (installer\sandbox\Invoke-CleanMachineGate.ps1).
    - Test-LucentSandboxConfiguration  checks a .wsb: well-formed, valid against wsb-configuration.xsd
                                       (Microsoft's documented elements and values), and the gate's
                                       rules (networking disabled, exactly the read-only input and the
                                       writable results folder, the logon command runs the gate).
    - Test-CleanMachineResult          checks the gate.json a gate wrote against what the host
                                       prepared: every check the gate makes present and passed (the
                                       environment's only on a clean machine), one export per photo in
                                       each format, the package's installer.
    - $CleanMachineGateChecks          the ids of the gate's checks.

    Windows PowerShell 5.1 compatible. Keep this file ASCII.
#>

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

# Where the gate's folders are inside the sandbox.
$script:SandboxRoot = 'C:\LucentGate'
# The ids of the checks Invoke-CleanMachineGate.ps1 makes; "installer-test" unless it ran with
# -SkipInstallerTest. The environment's two are required in Windows Sandbox only.
$script:GateCheckIds = @('environment.clean', 'environment.offline', 'install.exit', 'install.registration', 'install.files',
    'run1.exit', 'run1.screenshot', 'run1.qt', 'run1.assets', 'run1.auto', 'run1.jpeg',
    'run2.exit', 'run2.screenshot', 'run2.qt', 'run2.assets', 'run2.tiff', 'fixtures.unchanged',
    'uninstall.finished', 'uninstall.removed', 'uninstall.data-kept', 'installer-test')

function ConvertTo-XmlText([string]$Text) { return [System.Security.SecurityElement]::Escape($Text) }

function New-LucentSandboxConfiguration {
    <#
    .SYNOPSIS
        The .wsb text for the gate folders -InputDir (mapped read-only) and -ResultsDir (writable).
        -GateArguments are added to the logon command; -Manual opens Explorer at the input instead.
    #>
    param([Parameter(Mandatory)][string]$InputDir, [Parameter(Mandatory)][string]$ResultsDir, [ValidateRange(2048, 65536)][int]$MemoryMB = 4096,
          [string[]]$GateArguments = @(), [switch]$Manual)
    foreach ($folder in $InputDir, $ResultsDir) {
        if (-not [System.IO.Path]::IsPathRooted($folder) -or $folder -notmatch '^[A-Za-z]:\\') { throw "$folder is not an absolute folder on a drive." }
        if ($folder -match '[<>"|?*]') { throw "$folder has a character a mapped folder cannot have." }
    }
    $gate = "$script:SandboxRoot\input\scripts\Invoke-CleanMachineGate.ps1"
    foreach ($argument in $GateArguments) { if ($argument -match '[<>"&|]') { throw "The gate argument '$argument' has a character the logon command cannot carry." } }
    $command = if ($Manual) { "explorer.exe $script:SandboxRoot\input" } else {
        ((@('powershell.exe', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $gate, '-Root', $script:SandboxRoot) + $GateArguments) -join ' ') }
    $lines = @(
        '<!-- Lucent clean-machine gate (tools\Test-CleanMachine.ps1, docs\release\clean-machine.md): offline, the package read-only, the results writable. -->',
        '<Configuration>',
        '  <Networking>Disable</Networking>',
        '  <vGPU>Disable</vGPU>',
        '  <ClipboardRedirection>Disable</ClipboardRedirection>',
        '  <PrinterRedirection>Disable</PrinterRedirection>',
        '  <AudioInput>Disable</AudioInput>',
        '  <VideoInput>Disable</VideoInput>',
        "  <MemoryInMB>$MemoryMB</MemoryInMB>",
        '  <MappedFolders>',
        '    <MappedFolder>',
        "      <HostFolder>$(ConvertTo-XmlText $InputDir)</HostFolder>",
        "      <SandboxFolder>$script:SandboxRoot\input</SandboxFolder>",
        '      <ReadOnly>true</ReadOnly>',
        '    </MappedFolder>',
        '    <MappedFolder>',
        "      <HostFolder>$(ConvertTo-XmlText $ResultsDir)</HostFolder>",
        "      <SandboxFolder>$script:SandboxRoot\results</SandboxFolder>",
        '      <ReadOnly>false</ReadOnly>',
        '    </MappedFolder>',
        '  </MappedFolders>',
        '  <LogonCommand>',
        "    <Command>$(ConvertTo-XmlText $command)</Command>",
        '  </LogonCommand>',
        '</Configuration>')
    return (($lines -join "`r`n") + "`r`n")
}

function Test-LucentSandboxConfiguration {
    <#
    .SYNOPSIS
        Checks the .wsb at -Path (see the module description). -InputDir and -ResultsDir, when given,
        must be the mapped host folders. -Manual: the logon command opens Explorer instead of the gate.
        Returns Valid and Problems.
    #>
    param([Parameter(Mandatory)][string]$Path, [string]$InputDir, [string]$ResultsDir, [switch]$Manual)
    $problems = New-Object System.Collections.Generic.List[string]
    $schemaPath = Join-Path $PSScriptRoot 'wsb-configuration.xsd'
    $settings = New-Object System.Xml.XmlReaderSettings
    $settings.ValidationType = [System.Xml.ValidationType]::Schema
    $settings.DtdProcessing = [System.Xml.DtdProcessing]::Prohibit
    $schemaReader = [System.Xml.XmlReader]::Create($schemaPath)
    try { [void]$settings.Schemas.Add($null, $schemaReader) } finally { $schemaReader.Close() }
    $settings.add_ValidationEventHandler({ param($sender, $e) $problems.Add(("schema, line {0}: {1}" -f $e.Exception.LineNumber, $e.Message)) })
    $reader = [System.Xml.XmlReader]::Create($Path, $settings)
    try {
        while ($reader.Read()) { }
    }
    catch [System.Xml.XmlException] {
        $problems.Add("not well-formed XML: $($_.Exception.Message)")
    }
    finally {
        $reader.Close()
    }
    if ($problems.Count -eq 0) {
        [xml]$document = [System.IO.File]::ReadAllText($Path)
        $configuration = $document.Configuration
        $networking = $configuration.SelectSingleNode('Networking')
        if (-not $networking -or $networking.InnerText -ne 'Disable') { $problems.Add('networking is not disabled: the gate runs offline') }
        $mapped = @($configuration.SelectNodes('MappedFolders/MappedFolder'))
        $inputs = @($mapped | Where-Object { $_.SelectSingleNode('SandboxFolder') -and $_.SandboxFolder -eq "$script:SandboxRoot\input" })
        $outputs = @($mapped | Where-Object { $_.SelectSingleNode('SandboxFolder') -and $_.SandboxFolder -eq "$script:SandboxRoot\results" })
        if ($mapped.Count -ne 2 -or $inputs.Count -ne 1 -or $outputs.Count -ne 1) {
            $problems.Add("the gate maps exactly two folders, $script:SandboxRoot\input and $script:SandboxRoot\results (found $($mapped.Count))")
        }
        if ($inputs.Count -eq 1 -and (-not $inputs[0].SelectSingleNode('ReadOnly') -or $inputs[0].ReadOnly -ne 'true')) { $problems.Add('the input folder is not read-only') }
        if ($outputs.Count -eq 1 -and $outputs[0].SelectSingleNode('ReadOnly') -and $outputs[0].ReadOnly -ne 'false') { $problems.Add('the results folder is not writable') }
        if ($InputDir -and $inputs.Count -eq 1 -and $inputs[0].HostFolder -ne $InputDir) { $problems.Add("the input folder maps $($inputs[0].HostFolder), not $InputDir") }
        if ($ResultsDir -and $outputs.Count -eq 1 -and $outputs[0].HostFolder -ne $ResultsDir) { $problems.Add("the results folder maps $($outputs[0].HostFolder), not $ResultsDir") }
        foreach ($folder in $mapped) {
            if (-not (Test-Path -LiteralPath $folder.HostFolder -PathType Container)) { $problems.Add("the host folder $($folder.HostFolder) does not exist") }
        }
        $command = $configuration.SelectSingleNode('LogonCommand/Command')
        if (-not $command) { $problems.Add('there is no logon command') }
        elseif ($Manual) { if ($command.InnerText -notmatch '^explorer\.exe ') { $problems.Add('the manual configuration''s logon command does not open Explorer') } }
        elseif ($command.InnerText -notmatch '^powershell\.exe .*-File C:\\LucentGate\\input\\scripts\\Invoke-CleanMachineGate\.ps1 -Root C:\\LucentGate(\s|$)') {
            $problems.Add('the logon command does not run the gate (installer\sandbox\Invoke-CleanMachineGate.ps1) from the input folder')
        }
    }
    return [pscustomobject]@{ Valid = ($problems.Count -eq 0); Problems = $problems.ToArray() }
}

function Test-CleanMachineResult {
    <#
    .SYNOPSIS
        Checks a gate.json (-Gate, parsed) against what the host prepared: -Photos photos,
        -InstallerSha256 the package's Setup; -Rehearsal accepts an unclean, online machine (the
        environment's checks recorded, not required); -SkipInstallerTest when the gate ran without
        the installer test. Returns Passed and Problems.
    #>
    param([Parameter(Mandatory)]$Gate, [Parameter(Mandatory)][int]$Photos, [string]$InstallerSha256, [switch]$Rehearsal, [switch]$SkipInstallerTest)
    $problems = New-Object System.Collections.Generic.List[string]
    $names = @($Gate.PSObject.Properties.Name)
    if ($names -notcontains 'gate' -or $Gate.gate -ne 'clean-machine' -or $names -notcontains 'checks') {
        return [pscustomobject]@{ Passed = $false; Problems = @('not a clean-machine gate result') }
    }
    $expectedMode = if ($Rehearsal) { 'rehearsal' } else { 'sandbox' }
    if ($Gate.mode -ne $expectedMode) { $problems.Add("the gate ran as a $($Gate.mode), not a $expectedMode") }
    $checks = @($Gate.checks)
    $byId = @{}
    foreach ($check in $checks) { $byId[[string]$check.id] = $check }
    foreach ($id in $script:GateCheckIds) {
        if ($SkipInstallerTest -and $id -eq 'installer-test') { continue }
        if (-not $byId.ContainsKey($id)) { $problems.Add("the gate did not report '$id'"); continue }
        $required = -not ($Rehearsal -and $id -like 'environment.*')
        if ($required -and -not [bool]$byId[$id].passed) { $problems.Add("failed: $($byId[$id].name) ($($byId[$id].detail))") }
    }
    foreach ($check in $checks) {
        if ($script:GateCheckIds -notcontains [string]$check.id -and -not [bool]$check.passed) { $problems.Add("failed: $($check.name) ($($check.detail))") }
    }
    $runs = if ($names -contains 'runs') { $Gate.runs } else { $null }
    $jpegs = if ($runs) { @($runs.run1.jpeg).Count } else { 0 }
    $tiffs = if ($runs) { @($runs.run2.tiff).Count } else { 0 }
    if ($jpegs -ne $Photos -or $tiffs -ne $Photos) { $problems.Add("$Photos photo(s) given, $jpegs JPEG and $tiffs TIFF export(s) recorded") }
    $fixtures = if ($names -contains 'fixtures') { @($Gate.fixtures).Count } else { 0 }
    if ($fixtures -ne $Photos) { $problems.Add("$Photos photo(s) given, the gate saw $fixtures") }
    if ($InstallerSha256 -and (-not ($names -contains 'installer') -or $Gate.installer.sha256 -ne $InstallerSha256)) {
        $problems.Add('the gate installed another Setup than the package''s')
    }
    $passed = ($problems.Count -eq 0) -and [bool]$Gate.passed
    if ($problems.Count -eq 0 -and -not [bool]$Gate.passed) { $problems.Add('the gate says it failed') }
    return [pscustomobject]@{ Passed = $passed; Problems = $problems.ToArray() }
}

$CleanMachineGateChecks = $script:GateCheckIds
Export-ModuleMember -Function New-LucentSandboxConfiguration, Test-LucentSandboxConfiguration, Test-CleanMachineResult -Variable CleanMachineGateChecks
