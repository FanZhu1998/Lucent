<#
.SYNOPSIS
    Regenerates every research\lut result and runs the tests. Research only.

.DESCRIPTION
    Builds the upstream CPU extension when it is missing (or with -Rebuild), then runs
    reproduce_demo.py, trilinear_parity.py and model_summary.py (which rewrite research\lut\results)
    and the unit tests. Expects tools\setup.ps1 to have created the venv and the pinned checkout.
    A changed output hash is a finding to explain, not something to commit silently.
    Compatible with Windows PowerShell 5.1 and PowerShell 7.
#>
[CmdletBinding()]
param([switch]$Rebuild)

$ErrorActionPreference = 'Stop'
$tools = $PSScriptRoot
$py = (Resolve-Path (Join-Path $tools '..\.venv\Scripts\python.exe')).Path
$module = Join-Path $tools '..\build\trilinear_ext\lib\trilinear.cp312-win_amd64.pyd'

if ($Rebuild -or -not (Test-Path $module)) { & (Join-Path $tools 'build_trilinear_ext.ps1') }

function Invoke-Python([string]$What, [string[]]$Arguments) {
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'  # unittest and warnings write to stderr
    try {
        & $py -W ignore::UserWarning @Arguments 2>&1 |
            ForEach-Object { if ($_ -is [System.Management.Automation.ErrorRecord]) { $_.Exception.Message } else { "$_" } }
    } finally { $ErrorActionPreference = $saved }
    if ($LASTEXITCODE -ne 0) { throw "$What failed with exit code $LASTEXITCODE" }
}

foreach ($tool in 'reproduce_demo.py', 'trilinear_parity.py', 'model_summary.py') {
    Invoke-Python $tool @((Join-Path $tools $tool))
}
Invoke-Python 'unit tests' @('-m', 'unittest', 'discover', '-s', (Join-Path $tools '..\tests'), '-v')
Write-Host 'research\lut: results regenerated and tests passed.'
