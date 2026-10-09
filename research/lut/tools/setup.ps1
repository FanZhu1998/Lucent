<#
.SYNOPSIS
    Creates the learned-appearance research environment: research\lut\.venv and the pinned
    Image-Adaptive-3DLUT checkout. Research only.

.DESCRIPTION
    1. research\lut\.venv from CPython 3.12 (%LOCALAPPDATA%\Programs\Python\Python312) with
       research\lut\requirements.txt in --require-hashes --no-deps mode, then `pip check`.
       research\.venv (the decoder reference environment) is not touched.
    2. research\third_party\Image-Adaptive-3DLUT (gitignored) cloned from GitHub and checked out
       at b491f6df64a588864739a157db271e5c848e1805 with line-ending conversion disabled, so the
       working tree is byte-identical to the commit's blobs (verified).

    -Force recreates the venv. The checkout is never deleted by this script.
    Compatible with Windows PowerShell 5.1 and PowerShell 7.
#>
[CmdletBinding()]
param([switch]$Force)

$ErrorActionPreference = 'Stop'
$pinnedCommit = 'b491f6df64a588864739a157db271e5c848e1805'
$upstream = 'https://github.com/HuiZeng/Image-Adaptive-3DLUT.git'
$lutRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$repoRoot = (Resolve-Path (Join-Path $lutRoot '..\..')).Path
$python312 = Join-Path $env:LOCALAPPDATA 'Programs\Python\Python312\python.exe'
$venv = Join-Path $lutRoot '.venv'
$checkout = Join-Path $repoRoot 'research\third_party\Image-Adaptive-3DLUT'

function Invoke-Native([string]$What, [scriptblock]$Command) {
    # Windows PowerShell 5.1 turns native stderr (git progress, pip notices) into errors under 'Stop'.
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        & $Command 2>&1 |
            ForEach-Object { if ($_ -is [System.Management.Automation.ErrorRecord]) { $_.Exception.Message } else { "$_" } }
    } finally { $ErrorActionPreference = $saved }
    if ($LASTEXITCODE -ne 0) { throw "$What failed with exit code $LASTEXITCODE" }
}

# --- 1. venv ---------------------------------------------------------------------------
if ($Force -and (Test-Path $venv)) { cmd /c rmdir /s /q "$venv" }
if (-not (Test-Path (Join-Path $venv 'Scripts\python.exe'))) {
    if (-not (Test-Path $python312)) { throw "CPython 3.12 not found at $python312" }
    Invoke-Native 'venv' { & $python312 -m venv $venv }
    $py = Join-Path $venv 'Scripts\python.exe'
    Invoke-Native 'pip install' {
        & $py -m pip install --disable-pip-version-check --require-hashes --no-deps -r (Join-Path $lutRoot 'requirements.txt')
    }
    Invoke-Native 'pip check' { & $py -m pip check }
}
$py = Join-Path $venv 'Scripts\python.exe'
Invoke-Native 'version check' {
    & $py -c "import torch, torchvision, numpy, PIL; print('torch', torch.__version__, 'torchvision', torchvision.__version__, 'numpy', numpy.__version__, 'pillow', PIL.__version__, 'cuda', torch.cuda.is_available())"
}

# --- 2. pinned checkout ----------------------------------------------------------------
if (-not (Test-Path (Join-Path $checkout '.git'))) {
    New-Item -ItemType Directory -Force -Path (Split-Path $checkout) | Out-Null
    Invoke-Native 'git clone' { git clone --no-checkout $upstream $checkout }
    Invoke-Native 'git config' { git -C $checkout config core.autocrlf false }
    Invoke-Native 'git config' { git -C $checkout config core.eol lf }
    Invoke-Native 'git checkout' { git -C $checkout -c advice.detachedHead=false checkout --detach $pinnedCommit }
}
$head = (& git -C $checkout rev-parse HEAD).Trim()
if ($head -ne $pinnedCommit) { throw "The checkout is at $head, expected $pinnedCommit." }
$status = & git -C $checkout status --porcelain --ignored
if ($status) { throw "The checkout has local changes or untracked files:`n$($status -join "`n")" }
# Every working-tree file must hash to its committed blob (no line-ending conversion).
$mismatch = @()
foreach ($line in (& git -C $checkout ls-files -s)) {
    $parts = $line -split '\s+', 4
    $blob = (& git -C $checkout hash-object -- $parts[3]).Trim()
    if ($blob -ne $parts[1]) { $mismatch += $parts[3] }
}
if ($mismatch.Count -gt 0) { throw "Working-tree files differ from their blobs: $($mismatch -join ', ')" }
Write-Host "Checkout $checkout at $head; every file matches its committed blob."
Write-Host "Next: research\lut\tools\build_trilinear_ext.ps1, then research\lut\tools\run_all.ps1"
