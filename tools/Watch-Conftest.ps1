<#
.SYNOPSIS
    Watches a long vcpkg install for an autotools probe that hangs: a conftest.exe that stays alive
    without using CPU. Logs every such program and stops one whose CPU time has not changed for
    -IdleMinutes, so that the configure script goes on (the probe then fails, which for the locale
    probes of gettext's configure means "no such locale").

.DESCRIPTION
    On the reference machine (Windows 11 build 26200, Chinese_China.54936 among the installed
    locales) gettext's configure blocked indefinitely in "checking for a transitional chinese locale":
    its conftest.exe sat at 0 CPU forever. The overlay ports in cmake\ports preset that probe
    (gt_cv_locale_zh_CN=none, ADR 0023), so the known hang no longer occurs; this watchdog guards the
    other probes of a long install (the gettext tools, GLib's dependencies) the same way. Run it in a
    second PowerShell window, or as a background job, while tools\build.ps1 configures a build tree
    whose ports must be built from source:

        powershell -NoProfile -File tools\Watch-Conftest.ps1 -MaxHours 3

    It ends when -WatchPid exits (when given) or after -MaxHours. It changes nothing else.

.PARAMETER WatchPid
    A process (for example the vcpkg.exe of the install) whose end also ends the watchdog.
.PARAMETER IdleMinutes
    Minutes without CPU after which a conftest.exe is stopped (default 5; real probes take seconds).
.PARAMETER Log
    Where the watchdog writes what it saw and stopped (default: watch-conftest.log in the temp folder).
#>
[CmdletBinding()]
param(
    [int]$WatchPid = 0,
    [double]$IdleMinutes = 5,
    [double]$MaxHours = 3,
    [string]$Log = (Join-Path ([System.IO.Path]::GetTempPath()) 'watch-conftest.log')
)

$ErrorActionPreference = 'Stop'
$seen = @{}
$deadline = (Get-Date).AddHours($MaxHours)
"watchdog started $(Get-Date -Format o), watching pid $WatchPid" | Out-File -Append -Encoding utf8 $Log
while ((Get-Date) -lt $deadline) {
    if ($WatchPid -gt 0 -and -not (Get-Process -Id $WatchPid -ErrorAction SilentlyContinue)) { break }
    foreach ($p in @(Get-Process -Name conftest -ErrorAction SilentlyContinue)) {
        $cpu = $p.TotalProcessorTime.TotalSeconds
        $key = $p.Id
        if (-not $seen.ContainsKey($key) -or $seen[$key].Cpu -ne $cpu) {
            $seen[$key] = [pscustomobject]@{ Cpu = $cpu; Since = Get-Date }
            continue
        }
        $idle = ((Get-Date) - $seen[$key].Since).TotalMinutes
        if ($idle -ge $IdleMinutes) {
            $command = (Get-CimInstance Win32_Process -Filter "ProcessId = $key" -ErrorAction SilentlyContinue).CommandLine
            "$(Get-Date -Format o) stopping conftest pid $key, idle $([math]::Round($idle, 1)) min: $command" |
                Out-File -Append -Encoding utf8 $Log
            Stop-Process -Id $key -Force -ErrorAction SilentlyContinue
            $seen.Remove($key)
        }
    }
    Start-Sleep -Seconds 20
}
"watchdog ended $(Get-Date -Format o)" | Out-File -Append -Encoding utf8 $Log
