<#
.SYNOPSIS
    Checks (default) or installs the prerequisites for building Lucent on Windows.

.DESCRIPTION
    Without switches, or with -CheckOnly, the script inspects the machine and prints one line
    per prerequisite with its version and location. It changes nothing. It exits with 0 when
    every required prerequisite is present and 1 when at least one is missing, so it can gate
    other scripts.

    With -Install it performs the missing setup steps, then checks again:
      - Git, Visual Studio 2022 Build Tools, Python 3.12 and Ninja through winget
        (Build Tools needs administrator rights and shows one UAC prompt);
      - a Python virtual environment holding the pinned CMake (pip);
      - a vcpkg clone checked out at the registry baseline read from vcpkg-configuration.json,
        bootstrapped with metrics disabled.
    Every step is skipped when its result is already present, so the script is safe to re-run.
    -WhatIf lists the steps -Install would perform without performing them.

    Locations follow tools/dev-env.ps1 and can be overridden with environment variables:
      LUCENT_DEV_ROOT     root of developer tools   (default: %USERPROFILE%\dev)
      LUCENT_VCPKG_ROOT   pinned vcpkg checkout     (default: $LUCENT_DEV_ROOT\vcpkg)
      LUCENT_TOOLS_VENV   Python venv with cmake    (default: $LUCENT_DEV_ROOT\toolsenv)

    Reference versions are those of the reference machine (docs/development/setup.md). A
    different MSVC or vcpkg tool version still builds, but changes vcpkg's package ABI hashes,
    so dependencies are rebuilt instead of restored from the binary cache; the check reports
    such drift as a warning rather than a failure.

    Compatible with Windows PowerShell 5.1 and PowerShell 7.

.PARAMETER CheckOnly
    Report prerequisites and exit (the default when no switch is given).

.PARAMETER Install
    Install or repair missing prerequisites, then report.

.PARAMETER IncludeQtCheckout
    With -Install: also create the optional second vcpkg checkout
    ($LUCENT_DEV_ROOT\vcpkg-qt) at the same baseline, used for the long Qt source build so its
    vcpkg filesystem lock does not block everyday configures.

.EXAMPLE
    powershell -NoProfile -File tools\bootstrap-dev.ps1
    Reports every prerequisite; exit code 1 means something required is missing.

.EXAMPLE
    powershell -NoProfile -File tools\bootstrap-dev.ps1 -Install
    Installs what is missing (one UAC prompt if Visual Studio Build Tools must be installed).

.EXAMPLE
    powershell -NoProfile -File tools\bootstrap-dev.ps1 -Install -WhatIf
    Shows the installation steps that would run.

.NOTES
    Exit codes: 0 = all required prerequisites present; 1 = a required prerequisite is missing;
    2 = an installation step failed.
#>
[CmdletBinding(SupportsShouldProcess = $true, DefaultParameterSetName = 'Check')]
param(
    [Parameter(ParameterSetName = 'Check')]
    [switch]$CheckOnly,

    [Parameter(ParameterSetName = 'Install', Mandatory = $true)]
    [switch]$Install,

    [Parameter(ParameterSetName = 'Install')]
    [switch]$IncludeQtCheckout
)

Set-StrictMode -Version 3.0
$ErrorActionPreference = 'Stop'

# --- Reference configuration ------------------------------------------------------------------
# Keep in sync with docs/development/setup.md.
$Reference = @{
    VsWingetId       = 'Microsoft.VisualStudio.2022.BuildTools'
    VsDisplayVersion = '17.14.41'
    MsvcToolset      = '14.44.35207'
    WindowsSdk       = '10.0.26100.0'
    VsComponents     = @(
        'Microsoft.VisualStudio.Workload.VCTools',
        'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
        'Microsoft.VisualStudio.Component.Windows11SDK.26100',
        'Microsoft.VisualStudio.Component.VC.CMake.Project'
    )
    PythonMinor      = '3.12'
    PythonVersion    = '3.12.10'
    PythonWingetId   = 'Python.Python.3.12'
    NinjaWingetId    = 'Ninja-build.Ninja'
    NinjaVersion     = '1.13.2'
    GitWingetId      = 'Git.Git'
    CMakeVersion     = '4.4.3'
    VcpkgUrl         = 'https://github.com/microsoft/vcpkg.git'
}
$VsOverride = '--quiet --wait --norestart --nocache ' +
    (($Reference.VsComponents | ForEach-Object { "--add $_" }) -join ' ') + ' --includeRecommended'

$RepoRoot = Split-Path -Parent $PSScriptRoot
$DevRoot = if ($env:LUCENT_DEV_ROOT) { $env:LUCENT_DEV_ROOT } else { Join-Path $env:USERPROFILE 'dev' }
$VcpkgRoot = if ($env:LUCENT_VCPKG_ROOT) { $env:LUCENT_VCPKG_ROOT } else { Join-Path $DevRoot 'vcpkg' }
$VcpkgQtRoot = Join-Path $DevRoot 'vcpkg-qt'
$ToolsVenv = if ($env:LUCENT_TOOLS_VENV) { $env:LUCENT_TOOLS_VENV } else { Join-Path $DevRoot 'toolsenv' }
$VsWhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$WingetLinks = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Links'
$WingetPackages = Join-Path $env:LOCALAPPDATA 'Microsoft\WinGet\Packages'

# --- Helpers ----------------------------------------------------------------------------------

# Runs a native program and captures stdout+stderr as strings. Never throws for a non-zero exit
# code or for text on stderr (Windows PowerShell 5.1 turns redirected stderr into errors).
function Invoke-Capture {
    param([Parameter(Mandatory = $true)][string]$FilePath, [string[]]$Arguments = @())
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        $lines = & $FilePath @Arguments 2>&1 | ForEach-Object { "$_" }
        return [pscustomobject]@{ ExitCode = $LASTEXITCODE; Output = @($lines) }
    }
    catch {
        return [pscustomobject]@{ ExitCode = -1; Output = @($_.Exception.Message) }
    }
    finally {
        $ErrorActionPreference = $previous
    }
}

# Runs a native program with its output streamed to the console; throws on a non-zero exit code
# unless the code is listed in -AllowedExitCodes.
function Invoke-Checked {
    param(
        [Parameter(Mandatory = $true)][string]$FilePath,
        [string[]]$Arguments = @(),
        [int[]]$AllowedExitCodes = @(0)
    )
    Write-Host ("  > {0} {1}" -f $FilePath, ($Arguments -join ' ')) -ForegroundColor DarkGray
    $previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try {
        # Out-Host keeps the tool's output visible instead of returning it from this function.
        & $FilePath @Arguments | Out-Host
        $code = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previous
    }
    if ($AllowedExitCodes -notcontains $code) {
        throw ("'{0}' failed with exit code {1}." -f (Split-Path -Leaf $FilePath), $code)
    }
    return $code
}

function Get-FirstLine {
    param([object[]]$Lines)
    $first = @($Lines | Where-Object { "$_".Trim() -ne '' } | Select-Object -First 1)
    if ($first.Count -eq 0) { return '' }
    return "$($first[0])".Trim()
}

function Get-OptionalProperty {
    param([object]$Object, [string]$Name)
    if ($null -eq $Object) { return $null }
    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) { return $null }
    return $property.Value
}

# Appends PATH entries that installers added to the registry (machine and user) but that this
# session does not have yet, so newly installed tools are found. Existing entries keep their order.
function Update-SessionPath {
    $current = @($env:PATH -split ';' | Where-Object { $_ })
    $known = @{}
    foreach ($entry in $current) { $known[$entry.TrimEnd('\').ToLowerInvariant()] = $true }
    $added = @()
    foreach ($scope in 'Machine', 'User') {
        $value = [Environment]::GetEnvironmentVariable('PATH', $scope)
        if (-not $value) { continue }
        foreach ($entry in ($value -split ';' | Where-Object { $_ })) {
            $expanded = [Environment]::ExpandEnvironmentVariables($entry)
            $key = $expanded.TrimEnd('\').ToLowerInvariant()
            if (-not $known.ContainsKey($key)) {
                $known[$key] = $true
                $added += $expanded
            }
        }
    }
    if ($added.Count -gt 0) { $env:PATH = (@($current) + $added) -join ';' }
}

function New-CheckResult {
    param(
        [Parameter(Mandatory = $true)][ValidateSet('OK', 'WARN', 'MISSING', 'INFO')][string]$Status,
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Detail,
        [string]$Remedy = ''
    )
    return [pscustomobject]@{ Status = $Status; Name = $Name; Detail = $Detail; Remedy = $Remedy }
}

function Get-VcpkgBaseline {
    $configPath = Join-Path $RepoRoot 'vcpkg-configuration.json'
    if (-not (Test-Path $configPath)) { throw "vcpkg-configuration.json not found at $configPath." }
    $config = (Get-Content -Raw -Path $configPath) | ConvertFrom-Json
    $registry = Get-OptionalProperty $config 'default-registry'
    $baseline = Get-OptionalProperty $registry 'baseline'
    if (-not $baseline -or $baseline -notmatch '^[0-9a-f]{40}$') {
        throw "vcpkg-configuration.json does not declare a 40-hex-digit default-registry baseline."
    }
    return $baseline
}

# --- Probes (read-only) -------------------------------------------------------------------------

function Find-Git {
    $command = Get-Command git.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($command) { return $command.Source }
    $default = Join-Path $env:ProgramFiles 'Git\cmd\git.exe'
    if (Test-Path $default) { return $default }
    return $null
}

function Get-VisualStudioInstance {
    if (-not (Test-Path $VsWhere)) { return $null }
    $result = Invoke-Capture $VsWhere @('-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
        '-latest', '-format', 'json', '-utf8')
    if ($result.ExitCode -ne 0) { return $null }
    # Windows PowerShell 5.1 emits a JSON array as one object; ForEach-Object enumerates it.
    $instances = @(($result.Output -join "`n") | ConvertFrom-Json | ForEach-Object { $_ })
    if ($instances.Count -eq 0) { return $null }
    return $instances[0]
}

function Get-AnyBuildToolsInstance {
    if (-not (Test-Path $VsWhere)) { return $null }
    $result = Invoke-Capture $VsWhere @('-products', 'Microsoft.VisualStudio.Product.BuildTools', '-latest',
        '-property', 'installationPath')
    $path = Get-FirstLine $result.Output
    if ($result.ExitCode -ne 0 -or -not $path) { return $null }
    return $path
}

function Get-MissingVsComponent {
    param([string]$InstallationPath)
    $missing = @()
    foreach ($component in $Reference.VsComponents) {
        $result = Invoke-Capture $VsWhere @('-products', '*', '-requires', $component, '-property', 'installationPath')
        $paths = @($result.Output | Where-Object { "$_".Trim() -ne '' } | ForEach-Object { "$_".Trim() })
        if ($paths -notcontains $InstallationPath) { $missing += $component }
    }
    return $missing
}

function Find-Python312 {
    $default = Join-Path $env:LOCALAPPDATA ("Programs\Python\Python{0}\python.exe" -f ($Reference.PythonMinor -replace '\.', ''))
    if (Test-Path $default) { return $default }
    $launcher = Get-Command py.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($launcher) {
        $result = Invoke-Capture $launcher.Source @("-$($Reference.PythonMinor)", '-c', 'import sys; print(sys.executable)')
        $path = Get-FirstLine $result.Output
        if ($result.ExitCode -eq 0 -and $path -and (Test-Path $path)) { return $path }
    }
    return $null
}

function Find-WingetNinja {
    $link = Join-Path $WingetLinks 'ninja.exe'
    if (Test-Path $link) { return $link }
    if (Test-Path $WingetPackages) {
        $package = Get-ChildItem -Path $WingetPackages -Directory -Filter "$($Reference.NinjaWingetId)_*" -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($package) {
            $exe = Join-Path $package.FullName 'ninja.exe'
            if (Test-Path $exe) { return $exe }
        }
    }
    return $null
}

function Get-ToolVersion {
    param([string]$Exe, [string[]]$Arguments = @('--version'))
    $result = Invoke-Capture $Exe $Arguments
    if ($result.ExitCode -ne 0) { return $null }
    return Get-FirstLine $result.Output
}

function Get-GitHead {
    param([string]$Git, [string]$Root)
    $result = Invoke-Capture $Git @('-C', $Root, 'rev-parse', 'HEAD')
    if ($result.ExitCode -ne 0) { return $null }
    return Get-FirstLine $result.Output
}

function Test-GitCommitPresent {
    param([string]$Git, [string]$Root, [string]$Commit)
    $result = Invoke-Capture $Git @('-C', $Root, 'cat-file', '-e', "$Commit^{commit}")
    return ($result.ExitCode -eq 0)
}

function Get-VcpkgToolTag {
    param([string]$Root)
    $metadata = Join-Path $Root 'scripts\vcpkg-tool-metadata.txt'
    if (-not (Test-Path $metadata)) { return $null }
    $line = Get-Content -Path $metadata | Where-Object { $_ -match '^VCPKG_TOOL_RELEASE_TAG=' } | Select-Object -First 1
    if (-not $line) { return $null }
    return ($line -split '=', 2)[1].Trim()
}

function Get-VcpkgToolVersion {
    param([string]$Root)
    $exe = Join-Path $Root 'vcpkg.exe'
    if (-not (Test-Path $exe)) { return $null }
    $line = Get-ToolVersion $exe @('version')
    if ($line -and $line -match 'version (\d{4}-\d{2}-\d{2})') { return $Matches[1] }
    return $null
}

# Checks one vcpkg checkout: present, at the baseline commit, tool bootstrapped at the version the
# checkout pins. Returns check results.
function Test-VcpkgCheckout {
    param([string]$Label, [string]$Root, [string]$Baseline, [string]$Git, [switch]$Optional)
    $results = @()
    $absent = if ($Optional) { 'INFO' } else { 'MISSING' }
    if (-not (Test-Path (Join-Path $Root '.git'))) {
        $remedy = if ($Optional) { 'tools\bootstrap-dev.ps1 -Install -IncludeQtCheckout' } else { 'tools\bootstrap-dev.ps1 -Install' }
        $detail = if ($Optional) { "not present at $Root (optional)" } else { "no git checkout at $Root" }
        return @(New-CheckResult $absent $Label $detail $remedy)
    }
    if (-not $Git) {
        return @(New-CheckResult 'MISSING' $Label "cannot inspect $Root without Git" 'install Git first')
    }

    $head = Get-GitHead $Git $Root
    if ($head -eq $Baseline) {
        $results += New-CheckResult 'OK' $Label ("{0} at baseline {1}" -f $Root, $Baseline.Substring(0, 12))
    }
    elseif (Test-GitCommitPresent $Git $Root $Baseline) {
        $results += New-CheckResult 'WARN' $Label ("{0} at {1}, not at baseline {2}: builds work but package ABI hashes differ, so the binary cache is not reused" -f $Root, "$head".Substring(0, [Math]::Min(12, "$head".Length)), $Baseline.Substring(0, 12)) `
            "git -C `"$Root`" checkout --detach $Baseline; then run bootstrap-vcpkg.bat -disableMetrics"
    }
    else {
        $results += New-CheckResult 'MISSING' $Label ("{0} does not contain baseline {1}" -f $Root, $Baseline.Substring(0, 12)) `
            "git -C `"$Root`" fetch origin; git -C `"$Root`" checkout --detach $Baseline"
    }

    $expectedTag = Get-VcpkgToolTag $Root
    $toolVersion = Get-VcpkgToolVersion $Root
    $toolLabel = "$Label tool"
    if (-not $toolVersion) {
        $results += New-CheckResult $absent $toolLabel "vcpkg.exe not bootstrapped in $Root" "`"$Root\bootstrap-vcpkg.bat`" -disableMetrics"
    }
    elseif ($expectedTag -and $toolVersion -ne $expectedTag) {
        $results += New-CheckResult 'WARN' $toolLabel "vcpkg.exe $toolVersion, checkout pins $expectedTag" "`"$Root\bootstrap-vcpkg.bat`" -disableMetrics"
    }
    else {
        $results += New-CheckResult 'OK' $toolLabel "vcpkg.exe $toolVersion"
    }
    return $results
}

function Get-PrerequisiteReport {
    $results = New-Object System.Collections.Generic.List[object]
    $baseline = Get-VcpkgBaseline

    # Platform
    $os = Get-CimInstance -ClassName Win32_OperatingSystem
    if ([Environment]::Is64BitOperatingSystem) {
        $results.Add((New-CheckResult 'OK' 'Windows' ("{0} {1} ({2})" -f $os.Caption, $os.Version, $os.OSArchitecture)))
    }
    else {
        $results.Add((New-CheckResult 'MISSING' 'Windows' 'Lucent requires 64-bit Windows (Windows 11 is the reference platform)' ''))
    }
    $results.Add((New-CheckResult 'INFO' 'PowerShell' ("{0} {1}" -f $PSVersionTable.PSEdition, $PSVersionTable.PSVersion)))
    $policy = Get-ExecutionPolicy
    $userPolicy = Get-ExecutionPolicy -Scope CurrentUser
    $results.Add((New-CheckResult 'INFO' 'Execution policy' ("effective {0}, CurrentUser {1} (CurrentUser RemoteSigned is sufficient)" -f $policy, $userPolicy)))

    $longPaths = Get-ItemProperty -Path 'HKLM:\SYSTEM\CurrentControlSet\Control\FileSystem' -Name LongPathsEnabled -ErrorAction SilentlyContinue
    $longPathsValue = if ($longPaths) { [int]$longPaths.LongPathsEnabled } else { 0 }
    if ($longPathsValue -eq 1) {
        $results.Add((New-CheckResult 'INFO' 'Long paths' 'LongPathsEnabled=1'))
    }
    else {
        $results.Add((New-CheckResult 'INFO' 'Long paths' ("LongPathsEnabled=0: keep vcpkg roots and build directories short (vcpkg root is {0} characters)" -f $VcpkgRoot.Length)))
    }

    $computer = Get-CimInstance -ClassName Win32_ComputerSystem
    $ramGb = [Math]::Round($computer.TotalPhysicalMemory / 1GB, 1)
    $threads = [Environment]::ProcessorCount
    $jobs = [Math]::Max(1, [Math]::Min($threads, [int][Math]::Floor($ramGb / 3)))
    $results.Add((New-CheckResult 'INFO' 'Memory' ("{0} GB RAM, {1} logical processors: suggested build.ps1 -Jobs {2}" -f $ramGb, $threads, $jobs)))

    # winget is needed only for -Install.
    $winget = Get-Command winget.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($winget) {
        $results.Add((New-CheckResult 'OK' 'winget' ("{0} ({1})" -f (Get-ToolVersion $winget.Source), $winget.Source)))
    }
    else {
        $results.Add((New-CheckResult 'WARN' 'winget' 'not found; needed only by -Install' 'install "App Installer" from the Microsoft Store'))
    }

    # Git
    $git = Find-Git
    if ($git) {
        $results.Add((New-CheckResult 'OK' 'Git' ("{0} ({1})" -f (Get-ToolVersion $git), $git)))
    }
    else {
        $results.Add((New-CheckResult 'MISSING' 'Git' 'git.exe not found' "winget install --id $($Reference.GitWingetId) --exact --source winget"))
    }

    # Visual Studio Build Tools, MSVC and Windows SDK
    $vs = Get-VisualStudioInstance
    if ($vs) {
        $catalog = Get-OptionalProperty $vs 'catalog'
        $displayVersion = Get-OptionalProperty $catalog 'productDisplayVersion'
        $vsDetail = "{0} {1} ({2})" -f $vs.displayName, $displayVersion, $vs.installationPath
        $missingComponents = @(Get-MissingVsComponent $vs.installationPath)
        if ($missingComponents.Count -gt 0) {
            $results.Add((New-CheckResult 'MISSING' 'VS Build Tools' ("{0}; missing components: {1}" -f $vsDetail, ($missingComponents -join ', ')) 'tools\bootstrap-dev.ps1 -Install (adds the components; UAC prompt)'))
        }
        else {
            $results.Add((New-CheckResult 'OK' 'VS Build Tools' $vsDetail))
        }

        $toolsetFile = Join-Path $vs.installationPath 'VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt'
        $toolset = if (Test-Path $toolsetFile) { (Get-Content -Path $toolsetFile | Select-Object -First 1).Trim() } else { $null }
        if (-not $toolset) {
            $results.Add((New-CheckResult 'MISSING' 'MSVC toolset' 'default MSVC toolset version not found' 'repair Visual Studio Build Tools'))
        }
        elseif ($toolset -ne $Reference.MsvcToolset) {
            $results.Add((New-CheckResult 'WARN' 'MSVC toolset' ("{0} (reference {1}): builds work, but vcpkg rebuilds packages instead of restoring them" -f $toolset, $Reference.MsvcToolset)))
        }
        else {
            $results.Add((New-CheckResult 'OK' 'MSVC toolset' $toolset))
        }
    }
    else {
        $any = Get-AnyBuildToolsInstance
        $detail = if ($any) { "Build Tools at $any lack the MSVC x64 toolset" } else { 'no Visual Studio 2022 installation with the MSVC x64 toolset' }
        $results.Add((New-CheckResult 'MISSING' 'VS Build Tools' $detail 'tools\bootstrap-dev.ps1 -Install (UAC prompt)'))
    }

    $sdkInclude = Join-Path ${env:ProgramFiles(x86)} ("Windows Kits\10\Include\{0}" -f $Reference.WindowsSdk)
    if (Test-Path $sdkInclude) {
        $results.Add((New-CheckResult 'OK' 'Windows SDK' $Reference.WindowsSdk))
    }
    else {
        $results.Add((New-CheckResult 'MISSING' 'Windows SDK' "$($Reference.WindowsSdk) headers not found" 'tools\bootstrap-dev.ps1 -Install (Windows11SDK.26100 component)'))
    }

    # Python 3.12 (hosts the pinned CMake and the research environment)
    $python = Find-Python312
    if ($python) {
        $version = Get-ToolVersion $python
        $results.Add((New-CheckResult 'OK' 'Python' ("{0} ({1}; reference {2})" -f $version, $python, $Reference.PythonVersion)))
    }
    else {
        $results.Add((New-CheckResult 'MISSING' 'Python' "Python $($Reference.PythonMinor) not found" "winget install --id $($Reference.PythonWingetId) --exact --scope user"))
    }

    # Ninja
    $ninja = Find-WingetNinja
    $vsNinja = if ($vs) { Join-Path $vs.installationPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe' } else { $null }
    if ($ninja) {
        $version = Get-ToolVersion $ninja
        $status = if ($version -eq $Reference.NinjaVersion) { 'OK' } else { 'WARN' }
        $results.Add((New-CheckResult $status 'Ninja' ("{0} ({1}; reference {2})" -f $version, $ninja, $Reference.NinjaVersion)))
        if (-not (Test-Path (Join-Path $WingetLinks 'ninja.exe')) -and $vsNinja -and (Test-Path $vsNinja)) {
            $results.Add((New-CheckResult 'INFO' 'Ninja in builds' ("tools\dev-env.ps1 prepends only {0}, which has no ninja.exe, so builds use Visual Studio's Ninja {1}" -f $WingetLinks, (Get-ToolVersion $vsNinja))))
        }
    }
    elseif ($vsNinja -and (Test-Path $vsNinja)) {
        $results.Add((New-CheckResult 'WARN' 'Ninja' ("only Visual Studio's bundled Ninja {0} is available" -f (Get-ToolVersion $vsNinja)) "winget install --id $($Reference.NinjaWingetId) --exact --scope user"))
    }
    else {
        $results.Add((New-CheckResult 'MISSING' 'Ninja' 'ninja.exe not found' "winget install --id $($Reference.NinjaWingetId) --exact --scope user"))
    }

    # Pinned CMake in the tools venv
    $venvCmake = Join-Path $ToolsVenv 'Scripts\cmake.exe'
    if (Test-Path $venvCmake) {
        $line = Get-ToolVersion $venvCmake
        $version = if ($line -match 'cmake version (\S+)') { $Matches[1] } else { $line }
        if ($version -eq $Reference.CMakeVersion) {
            $results.Add((New-CheckResult 'OK' 'CMake' ("{0} ({1})" -f $version, $venvCmake)))
        }
        else {
            $results.Add((New-CheckResult 'WARN' 'CMake' ("{0} in {1} (reference {2})" -f $version, $ToolsVenv, $Reference.CMakeVersion) "`"$ToolsVenv\Scripts\python.exe`" -m pip install cmake==$($Reference.CMakeVersion)"))
        }
    }
    else {
        $results.Add((New-CheckResult 'MISSING' 'CMake' "no cmake.exe in tools venv $ToolsVenv" 'tools\bootstrap-dev.ps1 -Install'))
    }

    # vcpkg
    foreach ($item in (Test-VcpkgCheckout -Label 'vcpkg' -Root $VcpkgRoot -Baseline $baseline -Git $git)) { $results.Add($item) }
    foreach ($item in (Test-VcpkgCheckout -Label 'vcpkg-qt' -Root $VcpkgQtRoot -Baseline $baseline -Git $git -Optional)) { $results.Add($item) }

    if ($env:VCPKG_ROOT -and ($env:VCPKG_ROOT.TrimEnd('\') -ne $VcpkgRoot.TrimEnd('\'))) {
        $results.Add((New-CheckResult 'INFO' 'VCPKG_ROOT' ("set to {0}; tools\dev-env.ps1 ignores it and uses {1}" -f $env:VCPKG_ROOT, $VcpkgRoot)))
    }
    $cache = if ($env:VCPKG_DEFAULT_BINARY_CACHE) { $env:VCPKG_DEFAULT_BINARY_CACHE } else { Join-Path $env:LOCALAPPDATA 'vcpkg\archives' }
    $cacheState = if (Test-Path $cache) { 'present' } else { 'empty (first configure builds every package from source)' }
    $results.Add((New-CheckResult 'INFO' 'Binary cache' ("{0}: {1}" -f $cache, $cacheState)))

    return $results
}

function Write-Report {
    param([object[]]$Results)
    $colors = @{ OK = 'Green'; WARN = 'Yellow'; MISSING = 'Red'; INFO = 'Gray' }
    $width = ($Results | ForEach-Object { $_.Name.Length } | Measure-Object -Maximum).Maximum
    Write-Host ''
    Write-Host 'Lucent developer prerequisites' -ForegroundColor Cyan
    Write-Host ("  repository: {0}" -f $RepoRoot)
    Write-Host ("  dev root:   {0}" -f $DevRoot)
    Write-Host ''
    foreach ($result in $Results) {
        Write-Host ("  [{0,-7}] " -f $result.Status) -ForegroundColor $colors[$result.Status] -NoNewline
        Write-Host ("{0}  {1}" -f $result.Name.PadRight($width), $result.Detail)
    }
    $remedies = @($Results | Where-Object { $_.Remedy -and $_.Status -ne 'OK' })
    if ($remedies.Count -gt 0) {
        Write-Host ''
        Write-Host 'Suggested actions:' -ForegroundColor Cyan
        foreach ($result in $remedies) {
            Write-Host ("  {0}: {1}" -f $result.Name, $result.Remedy)
        }
    }
    $counts = @{}
    foreach ($status in 'OK', 'WARN', 'MISSING', 'INFO') {
        $counts[$status] = @($Results | Where-Object { $_.Status -eq $status }).Count
    }
    Write-Host ''
    $summaryColor = if ($counts.MISSING -gt 0) { 'Red' } elseif ($counts.WARN -gt 0) { 'Yellow' } else { 'Green' }
    Write-Host ("Summary: {0} ok, {1} warning(s), {2} missing, {3} informational." -f $counts.OK, $counts.WARN, $counts.MISSING, $counts.INFO) -ForegroundColor $summaryColor
}

# --- Installation (mutating; every step is idempotent) ------------------------------------------

function Invoke-WingetInstall {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param([string]$Id, [string[]]$ExtraArguments = @())
    $winget = Get-Command winget.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $winget) { throw 'winget is not available. Install "App Installer" from the Microsoft Store, then re-run.' }
    if ($PSCmdlet.ShouldProcess($Id, 'winget install')) {
        # 0x8A15002B (no applicable update) and 0x8A150061 (already installed) are benign.
        $arguments = @('install', '--id', $Id, '--exact', '--source', 'winget',
            '--accept-package-agreements', '--accept-source-agreements') + $ExtraArguments
        Invoke-Checked $winget.Source $arguments -AllowedExitCodes @(0, -1978335189, -1978335135) | Out-Null
        Update-SessionPath
    }
}

function Install-Git {
    if (Find-Git) { Write-Host 'Git: present.'; return }
    Write-Warning 'Installing Git for Windows (machine-wide; Windows may show a UAC prompt).'
    Invoke-WingetInstall $Reference.GitWingetId
}

function Install-VisualStudioBuildTools {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param()
    # An installation with the MSVC toolset, or else Build Tools that lack it: both are repaired with
    # the Visual Studio Installer, because winget does not add components to an existing install.
    $vs = Get-VisualStudioInstance
    $installationPath = if ($vs) { $vs.installationPath } else { Get-AnyBuildToolsInstance }
    if ($installationPath) {
        $missing = @(Get-MissingVsComponent $installationPath)
        if ($missing.Count -eq 0) { Write-Host 'Visual Studio Build Tools: all required components present.'; return }
        $installer = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\setup.exe'
        Write-Warning ("Adding Visual Studio components {0}. The Visual Studio Installer needs administrator rights: approve the UAC prompt." -f ($missing -join ', '))
        if ($PSCmdlet.ShouldProcess($installationPath, 'Visual Studio Installer modify')) {
            $arguments = @('modify', '--installPath', $installationPath, '--passive', '--norestart')
            foreach ($component in $missing) { $arguments += @('--add', $component) }
            # 3010 = success, restart required.
            $code = Invoke-Checked $installer $arguments -AllowedExitCodes @(0, 3010)
            if ($code -eq 3010) { Write-Warning 'Visual Studio Installer requests a restart before the toolset is used.' }
        }
        return
    }
    Write-Warning ('Installing Visual Studio 2022 Build Tools (several GB). The installer needs administrator ' +
        'rights: approve the single UAC prompt. The download and installation take a while.')
    Invoke-WingetInstall $Reference.VsWingetId @('--override', $VsOverride)
}

function Install-Python {
    if (Find-Python312) { Write-Host "Python $($Reference.PythonMinor): present."; return }
    Invoke-WingetInstall $Reference.PythonWingetId @('--scope', 'user')
}

function Install-Ninja {
    if (Find-WingetNinja) { Write-Host 'Ninja: present.'; return }
    Invoke-WingetInstall $Reference.NinjaWingetId @('--scope', 'user')
}

function Install-ToolsVenv {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param()
    $venvPython = Join-Path $ToolsVenv 'Scripts\python.exe'
    if (-not (Test-Path $venvPython)) {
        $python = Find-Python312
        if (-not $python) {
            if ($WhatIfPreference) { Write-Host "What if: create venv $ToolsVenv once Python is installed."; return }
            throw "Python $($Reference.PythonMinor) is required to create $ToolsVenv."
        }
        if ($PSCmdlet.ShouldProcess($ToolsVenv, 'create Python virtual environment')) {
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $ToolsVenv) | Out-Null
            Invoke-Checked $python @('-m', 'venv', $ToolsVenv) | Out-Null
        }
    }
    $venvCmake = Join-Path $ToolsVenv 'Scripts\cmake.exe'
    $current = if (Test-Path $venvCmake) { Get-ToolVersion $venvCmake } else { $null }
    if ($current -and $current -match [regex]::Escape("cmake version $($Reference.CMakeVersion)")) {
        Write-Host "CMake $($Reference.CMakeVersion): present."
        return
    }
    if ($PSCmdlet.ShouldProcess("cmake==$($Reference.CMakeVersion) in $ToolsVenv", 'pip install')) {
        Invoke-Checked $venvPython @('-m', 'pip', 'install', '--disable-pip-version-check', "cmake==$($Reference.CMakeVersion)") | Out-Null
    }
}

function Sync-VcpkgCheckout {
    [CmdletBinding(SupportsShouldProcess = $true)]
    param([string]$Root, [string]$Baseline)
    $git = Find-Git
    if (-not $git) {
        if ($WhatIfPreference) { Write-Host "What if: clone vcpkg into $Root once Git is installed."; return }
        throw 'Git is required to clone vcpkg.'
    }

    if (-not (Test-Path (Join-Path $Root '.git'))) {
        if ((Test-Path $Root) -and @(Get-ChildItem -Force -Path $Root).Count -gt 0) {
            throw "$Root exists but is not a git checkout. Move it aside or set LUCENT_VCPKG_ROOT."
        }
        if ($PSCmdlet.ShouldProcess($Root, "git clone $($Reference.VcpkgUrl)")) {
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Root) | Out-Null
            Invoke-Checked $git @('clone', $Reference.VcpkgUrl, $Root) | Out-Null
        }
        else {
            return
        }
    }

    if ((Get-GitHead $git $Root) -ne $Baseline) {
        $status = Invoke-Capture $git @('-C', $Root, 'status', '--porcelain', '--untracked-files=no')
        if (@($status.Output | Where-Object { "$_".Trim() -ne '' }).Count -gt 0) {
            throw "$Root has modified tracked files; commit, stash or discard them before moving it to the baseline."
        }
        if (-not (Test-GitCommitPresent $git $Root $Baseline)) {
            if ($PSCmdlet.ShouldProcess($Root, 'git fetch origin')) {
                Invoke-Checked $git @('-C', $Root, 'fetch', 'origin') | Out-Null
            }
        }
        if ($PSCmdlet.ShouldProcess($Root, "git checkout --detach $Baseline")) {
            Invoke-Checked $git @('-C', $Root, '-c', 'advice.detachedHead=false', 'checkout', '--detach', $Baseline) | Out-Null
        }
    }
    else {
        Write-Host "vcpkg checkout ${Root}: at baseline."
    }

    $expectedTag = Get-VcpkgToolTag $Root
    $toolVersion = Get-VcpkgToolVersion $Root
    if ($toolVersion -and (-not $expectedTag -or $toolVersion -eq $expectedTag)) {
        Write-Host "vcpkg tool in ${Root}: $toolVersion."
        return
    }
    if ($PSCmdlet.ShouldProcess($Root, 'bootstrap-vcpkg.bat -disableMetrics')) {
        Invoke-Checked (Join-Path $Root 'bootstrap-vcpkg.bat') @('-disableMetrics') | Out-Null
    }
}

function Install-DevPrerequisite {
    $baseline = Get-VcpkgBaseline
    Write-Host 'Installing Lucent developer prerequisites (steps already satisfied are skipped).' -ForegroundColor Cyan
    Install-Git
    Install-VisualStudioBuildTools
    Install-Python
    Install-Ninja
    Update-SessionPath
    Install-ToolsVenv
    Sync-VcpkgCheckout -Root $VcpkgRoot -Baseline $baseline
    if ($IncludeQtCheckout) {
        Sync-VcpkgCheckout -Root $VcpkgQtRoot -Baseline $baseline
    }
}

# --- Main -----------------------------------------------------------------------------------------

if ($Install) {
    try {
        Install-DevPrerequisite
    }
    catch {
        Write-Host ''
        Write-Host ("Installation step failed: {0}" -f $_.Exception.Message) -ForegroundColor Red
        exit 2
    }
    if ($WhatIfPreference) { exit 0 }
    Update-SessionPath
}

$report = @(Get-PrerequisiteReport)
Write-Report $report
if (@($report | Where-Object { $_.Status -eq 'MISSING' }).Count -gt 0) {
    exit 1
}
if ($Install) {
    Write-Host 'Next: powershell -NoProfile -File tools\build.ps1 -Preset dev-core -Test' -ForegroundColor Cyan
}
exit 0
