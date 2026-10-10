# Developer setup

This guide sets up a Windows machine to build and test Lucent from source. It records the exact
versions and commands used on the reference machine, so a new machine ends up in the same state.
The reasoning behind these choices is in
[ADR 0002 (build and dependency management)](../adr/0002-build-and-dependency-management.md).

**Contents:** [Reference versions](#reference-versions) ·
[Quick path](#quick-path-bootstrap-script) · [Manual setup](#manual-setup-step-by-step) ·
[Environment](#how-the-build-environment-is-loaded) · [Presets](#presets-and-vcpkg-features) ·
[Build and test](#build-and-test) · [Running Lucent.exe](#running-lucentexe-from-a-build-tree) ·
[Packaging](#packaging-the-installer) · [Fixtures](#test-fixtures) ·
[Research environment](#research-environment) · [Qt and UI builds](#qt-and-ui-builds) ·
[Troubleshooting](#troubleshooting)

## Reference versions

Verified on 2026-09-24 on the reference machine: Windows 11 Pro x64 (build 26200), 6 cores /
12 threads, 11.9 GB RAM, `LongPathsEnabled=0`.

| Component | Version | Installed with | Location |
|---|---|---|---|
| Visual Studio 2022 Build Tools | 17.14.41 (MSVC toolset 14.44.35207, compiler 19.44.35229) | winget `Microsoft.VisualStudio.2022.BuildTools` (machine-wide, one UAC prompt) | `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools` |
| Windows SDK | 10.0.26100.0 | Build Tools component `Windows11SDK.26100` | `C:\Program Files (x86)\Windows Kits\10` |
| Python | 3.12.10 | winget `Python.Python.3.12 --scope user` | `%LOCALAPPDATA%\Programs\Python\Python312` |
| Ninja | 1.13.2 | winget `Ninja-build.Ninja --scope user` | `%LOCALAPPDATA%\Microsoft\WinGet\Packages\Ninja-build.Ninja_*` |
| CMake | 4.4.3 | `pip install cmake==4.4.3` in a venv | `%USERPROFILE%\dev\toolsenv` |
| vcpkg | registry baseline `e143402b5545bab137cc465225a82da63cb2148f`, tool 2026-07-27 | `git clone` + `bootstrap-vcpkg.bat` | `%USERPROFILE%\dev\vcpkg` |
| vcpkg for Qt builds (optional) | same commit | `git clone` | `%USERPROFILE%\dev\vcpkg-qt` |
| Git for Windows | 2.55.0 | pre-installed (winget `Git.Git` if missing) | `C:\Program Files\Git` |
| PowerShell | Windows PowerShell 5.1 (PowerShell 7 also works) | Windows | |

Other MSVC or vcpkg tool versions still build Lucent. However, the compiler identity and the vcpkg
checkout are part of every package's ABI hash, so any difference means dependencies are rebuilt
from source instead of restored from the binary cache.

## Quick path: bootstrap script

From the repository root, in PowerShell:

```powershell
git clone https://github.com/FanZhu1998/Lucent.git
cd Lucent

# 1. Report what is installed (read-only). Exit code 1 means something required is missing.
powershell -NoProfile -File tools\bootstrap-dev.ps1

# 2. Install or repair whatever is missing (one UAC prompt if Build Tools must be installed).
powershell -NoProfile -File tools\bootstrap-dev.ps1 -Install

# 3. Build and run the tests (the definition-of-done command).
powershell -NoProfile -File tools\build.ps1 -Preset dev-core -Test
```

`tools\bootstrap-dev.ps1` options:

| Invocation | Effect |
|---|---|
| (no switch) or `-CheckOnly` | Prints one line per prerequisite with version and location, then suggested fixes. Changes nothing. Exit 0 = all required present, 1 = something required missing. |
| `-Install` | Installs missing tools with winget (Git, Build Tools, Python, Ninja), creates the tools venv with the pinned CMake, clones vcpkg, checks out the baseline commit read from `vcpkg-configuration.json` and bootstraps it. Already-satisfied steps are skipped, so re-running is safe. Exit 2 = an installation step failed. |
| `-Install -IncludeQtCheckout` | Also creates the optional `vcpkg-qt` checkout (see [Qt and UI builds](#qt-and-ui-builds)). |
| `-Install -WhatIf` | Lists the installation steps without performing them. |

The script reads the same location overrides as `tools\dev-env.ps1` (see
[environment variables](#how-the-build-environment-is-loaded)). Output on the reference machine,
abbreviated:

```text
  [OK     ] Windows           Microsoft Windows 11 Pro 10.0.26200 (64-bit)
  [INFO   ] Long paths        LongPathsEnabled=0: keep vcpkg roots and build directories short (vcpkg root is 24 characters)
  [INFO   ] Memory            11.9 GB RAM, 12 logical processors: suggested build.ps1 -Jobs 3
  [OK     ] VS Build Tools    Visual Studio Build Tools 2022 17.14.41 (September 2026) (C:\Program Files (x86)\...\BuildTools)
  [OK     ] MSVC toolset      14.44.35207
  [OK     ] Windows SDK       10.0.26100.0
  [OK     ] Python            Python 3.12.10 (...\Python312\python.exe; reference 3.12.10)
  [OK     ] Ninja             1.13.2 (...\WinGet\Packages\Ninja-build.Ninja_...\ninja.exe; reference 1.13.2)
  [INFO   ] Ninja in builds   tools\dev-env.ps1 prepends only ...\WinGet\Links, which has no ninja.exe, so builds use Visual Studio's Ninja 1.12.1
  [OK     ] CMake             4.4.3 (C:\Users\...\dev\toolsenv\Scripts\cmake.exe)
  [OK     ] vcpkg             C:\Users\...\dev\vcpkg at baseline e143402b5545
  [OK     ] vcpkg tool        vcpkg.exe 2026-07-27
Summary: 13 ok, 0 warning(s), 0 missing, 6 informational.
```

## Manual setup, step by step

These are the commands `-Install` runs, for machines where you prefer to run them yourself.

### 0. PowerShell execution policy

Lucent's scripts are unsigned local files. `RemoteSigned` for the current user is enough and does
not need administrator rights:

```powershell
Set-ExecutionPolicy -Scope CurrentUser RemoteSigned
```

If the repository was downloaded as a ZIP rather than cloned, Windows marks the scripts as coming
from the internet. Unblock them with `Get-ChildItem -Recurse tools | Unblock-File`.

### 1. Git

```powershell
winget install --id Git.Git --exact --source winget
```

### 2. Visual Studio 2022 Build Tools (administrator: one UAC prompt)

This installs MSVC, the Windows 11 SDK 10.0.26100 and the CMake/Ninja integration. The download
is several GB.

```powershell
winget install --id Microsoft.VisualStudio.2022.BuildTools --exact --source winget `
  --override "--quiet --wait --norestart --nocache --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.Windows11SDK.26100 --add Microsoft.VisualStudio.Component.VC.CMake.Project --includeRecommended"
```

If Build Tools are already installed without some of these components, `bootstrap-dev.ps1
-Install` adds them with the Visual Studio Installer
(`setup.exe modify --installPath <path> --add <component> --passive --norestart`), which also asks
for elevation.

### 3. Python 3.12 (per user)

```powershell
winget install --id Python.Python.3.12 --exact --source winget --scope user
```

Use the full path `%LOCALAPPDATA%\Programs\Python\Python312\python.exe`. A bare `python` on PATH
may resolve to the Microsoft Store alias in `WindowsApps`.

### 4. Ninja (per user)

```powershell
winget install --id Ninja-build.Ninja --exact --source winget --scope user
```

### 5. Pinned CMake in a tools venv

```powershell
& "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe" -m venv "$env:USERPROFILE\dev\toolsenv"
& "$env:USERPROFILE\dev\toolsenv\Scripts\python.exe" -m pip install cmake==4.4.3
```

### 6. vcpkg at the project baseline

The commit must be exactly the `baseline` in `vcpkg-configuration.json`:

```powershell
git clone https://github.com/microsoft/vcpkg.git "$env:USERPROFILE\dev\vcpkg"
git -C "$env:USERPROFILE\dev\vcpkg" -c advice.detachedHead=false checkout --detach e143402b5545bab137cc465225a82da63cb2148f
& "$env:USERPROFILE\dev\vcpkg\bootstrap-vcpkg.bat" -disableMetrics
```

`bootstrap-vcpkg.bat` downloads the vcpkg tool release that the checkout pins
(`scripts\vcpkg-tool-metadata.txt`, currently 2026-07-27).

### 7. Optional: second vcpkg checkout for Qt

```powershell
git clone https://github.com/microsoft/vcpkg.git "$env:USERPROFILE\dev\vcpkg-qt"
git -C "$env:USERPROFILE\dev\vcpkg-qt" -c advice.detachedHead=false checkout --detach e143402b5545bab137cc465225a82da63cb2148f
& "$env:USERPROFILE\dev\vcpkg-qt\bootstrap-vcpkg.bat" -disableMetrics
```

### 8. Verify

```powershell
powershell -NoProfile -File tools\bootstrap-dev.ps1
```

## How the build environment is loaded

`tools\build.ps1` dot-sources `tools\dev-env.ps1`, which:

1. finds the newest Visual Studio installation with the MSVC x64 toolset (`vswhere`) and enters its
   x64 developer shell (`Enter-VsDevShell`);
2. sets `VCPKG_ROOT` to Lucent's pinned checkout **after** entering the developer shell;
3. puts the tools-venv CMake (and `%LOCALAPPDATA%\Microsoft\WinGet\Links`, if it contains
   `ninja.exe`) first on `PATH`;
4. sets `VCPKG_DISABLE_METRICS=1` and `LUCENT_DEV_ENV_LOADED=1`. It runs only once per session.

| Variable | Default | Purpose |
|---|---|---|
| `LUCENT_DEV_ROOT` | `%USERPROFILE%\dev` | Root for developer tools |
| `LUCENT_VCPKG_ROOT` | `%LUCENT_DEV_ROOT%\vcpkg` | Pinned vcpkg checkout |
| `LUCENT_TOOLS_VENV` | `%LUCENT_DEV_ROOT%\toolsenv` | Python venv holding the pinned CMake |

> **Do not set `VCPKG_ROOT` yourself. Set `LUCENT_VCPKG_ROOT` instead.** `Enter-VsDevShell` points
> `VCPKG_ROOT` at the vcpkg copy bundled with Visual Studio. That copy's helper scripts differ from
> the pinned checkout, so every package gets a different ABI hash. On the reference machine this
> silently disabled the binary cache: every dependency was rebuilt from source. `dev-env.ps1`
> therefore ignores any existing `VCPKG_ROOT` and sets it again from `LUCENT_VCPKG_ROOT` after
> entering the shell. See [Troubleshooting](#every-package-rebuilds-from-source-vcpkg_root).

To work interactively with plain CMake commands, load the environment once per PowerShell window:

```powershell
. .\tools\dev-env.ps1
cmake --preset dev-core
cmake --build build\dev-core --parallel 2
ctest --preset dev-core --parallel 2
```

IDEs read `CMakePresets.json`, whose toolchain file is `$env{VCPKG_ROOT}\scripts\buildsystems\vcpkg.cmake`.
An IDE must therefore see `VCPKG_ROOT` pointing at the pinned checkout, for example VS Code started
with `code .` from a window where `dev-env.ps1` has been loaded. An IDE that sets up its own
Visual Studio environment gets the bundled vcpkg described above. IDE integration has not been
validated yet; `tools\build.ps1` is the supported path.

## Presets and vcpkg features

| Preset | Build type | vcpkg features | Qt needed | Use |
|---|---|---|---|---|
| `dev-core` | RelWithDebInfo | `tests` | No | Everyday development of the libraries, `lucent-probe` and tests. **Required to pass for every increment.** |
| `dev` | RelWithDebInfo | `tests;ui` | Yes | Full application: `Lucent.exe` (Qt Quick UI, W3) and its tests. Must also pass from W3 on. |
| `release` | Release | `tests;ui` | Yes | Packaging builds |

- Engine dependencies (LibRaw, Little CMS, libjpeg-turbo, libtiff, SQLite, nlohmann-json,
  tl-expected, zlib) are unconditional. `tests` adds GoogleTest, and `ui` adds Qt (see
  `vcpkg.json`).
- All presets use the overlay triplet `x64-windows-lucent` (`cmake/triplets/`) as both the target
  and the host triplet: shared libraries, dynamic CRT, **release-only**. Developer builds are
  therefore RelWithDebInfo against the release CRT (`/MD`). There is no Debug-CRT configuration.
- Each preset builds into `build\<preset>` unless `-BuildDir` is given. vcpkg installs the
  packages for that build into `<build dir>\vcpkg_installed`.

## Build and test

The definition-of-done command for every increment (`docs/development/roadmap.md`):

```powershell
powershell -NoProfile -File tools\build.ps1 -Preset dev-core -Test
```

`tools\build.ps1` parameters:

| Parameter | Default | Meaning |
|---|---|---|
| `-Preset` | `dev-core` | `dev-core`, `dev` or `release` |
| `-BuildDir` | `build\<preset>` | Separate build tree, for example one per branch or per parallel worker |
| `-Jobs` | `4` | Parallel compile jobs and parallel test processes. Use 2–4 on a 12 GB machine |
| `-Target` | all | Build a single target |
| `-Test` | off | Run CTest after building |
| `-TestRegex` | all | Only tests whose name matches (`ctest -R`), for example `LibRawAdapter` |
| `-ConfigureOnly` | off | Configure (and install vcpkg packages) without building |
| `-Fresh` | off | Discard the CMake cache and reconfigure from scratch |

Examples:

```powershell
# Separate build tree with modest parallelism
powershell -NoProfile -File tools\build.ps1 -Preset dev-core -BuildDir build\my-feature -Jobs 2 -Test

# Rebuild one target and run matching tests
powershell -NoProfile -File tools\build.ps1 -Target <target> -Test -TestRegex <regex>
```

All executables and the DLLs they need are placed in `<build dir>\bin`. From a window where
`dev-env.ps1` is loaded, a test executable can be run directly, for example
`.\build\dev-core\bin\<test>.exe --gtest_filter=Suite.*`.

Warnings are errors (`/W4 /WX`). Fix the warning in Lucent code. Do not disable warnings globally
or edit `cmake\LucentCompilerOptions.cmake` to silence them.

## Running Lucent.exe from a build tree

`tools\build.ps1 -Preset dev` builds `<build dir>\bin\Lucent.exe`. The build also runs the
`lucent_ui_runtime` target, which copies the vcpkg runtime DLLs into `bin` and writes `bin\qt.conf`
pointing Qt at the plugins and QML imports under `<build dir>\vcpkg_installed`. The program then
starts by double-click or from any shell, without `dev-env.ps1`. This `qt.conf` is for build trees
only: the installer deploys Qt with windeployqt (W5) and never ships it.

```powershell
.\build\dev\bin\Lucent.exe                                # empty library
.\build\dev\bin\Lucent.exe D:\Photos\IMG_0001.CR3 D:\Card   # positional paths are added, like drag and drop
```

`Lucent.exe` is a WIN32 program without a console. The application services' log (for example why
the camera support database was refused) goes to `<cache>\logs\lucent.log`
(`%LOCALAPPDATA%\Lucent\cache\logs`, or the `--data-root` folder's `cache\logs`; rotated at 8 MiB,
five files). Its own progress lines (`lucent: ...`) reach a terminal only when stderr is redirected.
Set `QT_FORCE_STDERR_LOGGING=1` to get Qt's and QML's warnings there as well:

```powershell
$env:QT_FORCE_STDERR_LOGGING = '1'
.\build\dev\bin\Lucent.exe --import fixtures\cache\canon-eos-r5-cr3-raw --open-first --measure 2> lucent.log
```

**Developer-only options** exist for verifying a build without clicking through the window. They
are not part of the user interface, may change without notice, and are documented in
`apps\lucent\src\developer_automation.hpp` and by `Lucent.exe --help`:

| Option | Effect |
|---|---|
| `--import <path>` (repeatable), `--include-subfolders` | Add photos or folders when the window opens |
| `--open-first`, `--view <library\|edit\|exports>`, `--zoom <fit\|100>` | Open the first photo in Edit, show a view, zoom to 100 % |
| `--export-all <folder>`, `--export-format <jpeg\|tiff16>`, `--export-color-space <srgb\|display-p3>`, `--export-quality <1..100>` | Export every photo once the import has finished |
| `--show-export-dialog` | Open the Export dialog (with `--open-first`) |
| `--measure` | Report start-up time, first visible thumbnail, preview latency and UI-thread responsiveness on stderr |
| `--screenshot <file.png>`, `--quit-after-screenshot`, `--settle-timeout <ms>` | Save the window once no work is pending and two rendered frames agree, then exit (0 saved, 1 save failed, 3 not settled) |

For example, screenshots of the three views on the smoke fixtures (after
`tools\fetch-fixtures.ps1 -Tier smoke`):

```powershell
$bin = '.\build\dev\bin\Lucent.exe'
& $bin --import fixtures\cache --include-subfolders --view library --screenshot shots\library.png --quit-after-screenshot
& $bin --import fixtures\cache --include-subfolders --open-first --screenshot shots\edit.png --quit-after-screenshot
& $bin --import fixtures\cache --include-subfolders --export-all $env:TEMP\lucent-out --view exports --screenshot shots\exports.png --quit-after-screenshot
```

## Packaging the installer

`tools\package.ps1` builds `Lucent-Setup-<version>-x64.exe`, the per-user installer
([ADR 0009](../adr/0009-windows-packaging.md)). It needs, besides the build prerequisites:

- **Inno Setup 6.3 or later.** `ISCC.exe` is taken from `-Iscc` or `LUCENT_ISCC` (either must be
  valid; there is no fallback), else found on `PATH`, in the uninstall registry ("Inno Setup 6" /
  "Inno Setup version 6.x", its `InstallLocation`), or as `Inno Setup 6` under Program Files or
  `%LOCALAPPDATA%\Programs`. A compiler outside those standard folders causes a warning; the
  manifest records the compiler's path, version and SHA-256. The reference machine has 6.7.3 (a
  portable copy registered by another project), whose compiler reports "Non-commercial use only";
  Inno Setup's commercial terms are reviewed before a public release.
- **Microsoft's `vc_redist.x64.exe`**, taken from the Visual Studio installation that
  `dev-env.ps1` enters (`VCToolsRedistDir`; on the reference machine 14.44.35211). Override with
  `-VCRedist` or the CMake cache variable `LUCENT_VC_REDIST`. It must carry Microsoft's valid
  Authenticode signature.
- **The source archives of libjpeg-turbo and PCRE2** in vcpkg's downloads folder
  (`%VCPKG_ROOT%\downloads`, or `VCPKG_DOWNLOADS`): two licence texts the ports do not install are
  taken from them. They are there when the ports were built on the machine, not after a pure
  binary-cache restore; then run `vcpkg install --only-downloads` for those ports.
- **Optional: spdx-tools** (`pip install spdx-tools`) to validate the SBOM. `pyspdxtools.exe` is
  found through `-SpdxValidator`, `LUCENT_PYSPDXTOOLS` or `PATH`; without it only the built-in
  structural check runs.

```powershell
# Build the release preset (Lucent.exe only), lay out, audit, license and compile the installer
powershell -NoProfile -File tools\package.ps1

# From scratch, with the full test suite first
powershell -NoProfile -File tools\package.ps1 -Fresh -RunTests -Jobs 4

# Package the existing build tree; stop before Inno Setup (the manifest then records no installer)
powershell -NoProfile -File tools\package.ps1 -SkipBuild -StageOnly
```

| Step | What happens | Output (in `<build dir>\package` unless `-OutputDir`) |
|---|---|---|
| Build | `tools\build.ps1 -Preset release` in the same PowerShell process (target `Lucent`; everything and the tests with `-RunTests`). Then the installer, checksum, manifest and SBOM copy of an earlier run of the same version are deleted | `build\release` |
| Lay out | `cmake --install --component Lucent`: `windeployqt`, unused Qt Quick styles removed, `qt.conf`, the DLL closure from vcpkg (`cmake/LucentPackaging.cmake`) | `stage\`, `install.log`, `<build dir>\packaging\windeployqt.log` |
| Audit | Every import resolves to the folder, Windows or the Visual C++ runtime, and every root DLL is imported; the required Qt plugins and every QML module Lucent uses (with the plugin DLL its `qmldir` names) are present; every file is Lucent's or unmodified from a vcpkg port; one GUI executable, x64 only. Errors stop the run | `audit.txt`, `audit.json` |
| Smoke run | The staged `Lucent.exe` starts with `PATH` reduced to Windows' folders and a temporary `--data-root`, saves a screenshot and must exit with 0, without Qt or QML loading errors and without changing the folder | `smoke\` |
| Sign | Only with `LUCENT_SIGN_CERT_SHA1` set (see below): `Lucent.exe` and `lucent-worker.exe`, each signature verified | |
| Licences | `stage\licenses\`: copyright files of the shipped ports, missing licence texts, Qt's LGPL-3.0 and GPL-3.0, `LGPL-NOTICE.txt`, `THIRD-PARTY-NOTICES.txt` (with the third-party code inside Qt from Qt's own SBOMs, and the libraries linked statically into `Lucent.exe`), `Lucent.spdx.json` (SPDX 2.3, checked structurally and by `pyspdxtools` when available); `stage\Lucent-files.txt`, the installed-file list that the next version's upgrade uses | `Lucent-Setup-<version>-x64.spdx.json` |
| Builder paths | Every byte of every file of the folder is scanned for the build machine's folders: the user profile, the repository, the build tree, the vcpkg root, installed tree, buildtrees, packages and downloads folders, and any `X:\Users\<name>\` path, as 8-bit or UTF-16 text, with either separator and in any letter case. A hit (a PDB path in a DLL's debug directory, a `__FILE__` string, a path in a text file) stops the run with the file and byte offset. Setup and the manifest are scanned too | `builder-paths.txt`, `builder-paths.json` |
| Installer | `installer\lucent.iss` with the version from CMake; Setup and the uninstaller signed when signing is configured | `Lucent-Setup-<version>-x64.exe`, `.exe.sha256`, `iscc.log` |
| Manifest | Version, git commit, signing (certificate, test or release, timestamp, every signature checked), toolchain, audit summary (including the builder-path scan), SBOM summary, the RAW extensions of "Open with Lucent", every installed file with its SHA-256, the installer's size and SHA-256 (none with `-StageOnly`) | `Lucent-Setup-<version>-x64.manifest.json` |

The script prints the installer's path, size and SHA-256. On the reference machine (2026-10-09) the
0.1.0 package takes about a minute after the build and produces a 39.9 MiB installer (about 41.9 MB;
24.4 MiB of it the redistributable) for a 62.9 MiB, 308-file installation. It also prints a reminder that the
installer is not for public distribution until W12 settles how the LGPL libraries' source is
provided (ADR 0009, decision 10).

**Licence decisions** that cannot be derived from the build (LGPL and FreeType elections, credit
lines, missing texts, reviewed static libraries, ports that must never ship) are in
`installer\license-policy.json`. Each entry
names the port version it was reviewed for; when a dependency upgrade ships another version,
packaging stops until the entry and the [licence ledger](../legal/third-party-notices.md) are
reviewed.

**No build-machine paths.** The package must not name the folders of the machine that built it
(ADR 0002 and ADR 0009 amendments, W12). The triplet links every port with `/PDBALTPATH:%_PDB%`
and compiles it with `/d1trimfile:` for the vcpkg folders; Lucent's release build does the same
for its own folders. Because the DLLs then record only their PDBs' file names, vcpkg's own PDB
copying finds nothing (every port's log warns "Could not find a matching pdb file"); the triplet's
post-portfile step (`cmake/triplets/post-portfile/install-pdbs.cmake`) installs the PDBs beside the
DLLs instead, so the dependencies' symbols stay available. Changing the triplet or that step
rebuilds every port once (1.2 hours on the reference machine, mostly Qt), and so does saving either
with other line endings: vcpkg hashes their bytes. If the builder-path scan fails,
`builder-paths.txt` names each file, byte offset and folder; a hit in a "PE debug directory" is a
PDB path (a missing `/PDBALTPATH`), one in "file data" usually a `__FILE__` string.
`installer\tests\Test-BuilderPathScan.ps1` checks the scan itself without a build.

**Signing** is off by default, and packages built so far are unsigned. To sign `Lucent.exe`,
`lucent-worker.exe`, Setup and the uninstaller with a code-signing certificate whose private key is
in the current user's personal store (`Cert:\CurrentUser\My`):

```powershell
$env:LUCENT_SIGN_CERT_SHA1 = '<certificate thumbprint>'
$env:LUCENT_SIGN_TIMESTAMP_URL = '<RFC 3161 timestamp URL of the certificate issuer>'
# optional: $env:LUCENT_SIGNTOOL = '<path to signtool.exe>' (default: signtool.exe from the Windows SDK)
powershell -NoProfile -File tools\package.ps1
```

`tools\package.ps1` checks the certificate before it builds anything (in that store, with its
private key, valid now, usable for code signing; a thumbprint pasted from the certificate dialog
works) and refuses a self-signed certificate or a missing timestamp server. It verifies every
signature it makes: by that thumbprint, verifying offline with WinVerifyTrust (digest and chain, no
revocation check, no network) and timestamped; a Setup that fails is deleted. The manifest records
the certificate and every signature. Inno Setup signs the uninstaller inside Setup, so only an
installation shows it: `installer\tests\Test-InstalledPackage.ps1 -SignerThumbprint <thumbprint>`
checks Setup, the installed `Lucent.exe` and `lucent-worker.exe`, and `unins000.exe` (a signed
uninstaller keeps its messages in `unins000.msg`, Inno Setup's file).

**Rehearsing signing without a real certificate.** Use a temporary self-signed certificate in the
current user's personal store, never in a Root or TrustedPublisher store, and `LUCENT_SIGN_TEST=1`:
the one verification failure accepted is then the untrusted root, a timestamp is optional (none is
made without `LUCENT_SIGN_TIMESTAMP_URL`: requesting one sends each file's digest to the timestamp
server), and the manifest records `signing.test = true`. Such a package is never distributed.
Delete the certificate and its private key afterwards:

```powershell
$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=Lucent signing rehearsal' `
    -CertStoreLocation Cert:\CurrentUser\My -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 `
    -KeyExportPolicy NonExportable -NotAfter (Get-Date).AddDays(1)
$env:LUCENT_SIGN_CERT_SHA1 = $cert.Thumbprint; $env:LUCENT_SIGN_TEST = '1'
powershell -NoProfile -File tools\package.ps1 -SkipBuild
# signtool verify /pa /v <file> now ends with "A certificate chain processed, but terminated in a
# root certificate which is not trusted by the trust provider": expected for a test certificate.
$pkg = 'build\release\package'
powershell -NoProfile -File installer\tests\Test-InstalledPackage.ps1 -Installer "$pkg\Lucent-Setup-0.1.0-x64.exe" `
    -Manifest "$pkg\Lucent-Setup-0.1.0-x64.manifest.json" -Fixture <RAW file> -SignerThumbprint $cert.Thumbprint -AllowUntrustedRoot
Remove-Item "Cert:\CurrentUser\My\$($cert.Thumbprint)" -DeleteKey
# New-SelfSignedCertificate also puts a copy (without the key) into CurrentUser\CA:
Remove-Item "Cert:\CurrentUser\CA\$($cert.Thumbprint)" -ErrorAction SilentlyContinue
Get-ChildItem Cert:\ -Recurse | Where-Object Thumbprint -eq $cert.Thumbprint   # must list nothing
Remove-Item Env:\LUCENT_SIGN_CERT_SHA1, Env:\LUCENT_SIGN_TEST
```

The W12 rehearsal of this procedure (ADR 0009 amendment, decision 13 and its evidence) signed
`Lucent.exe`, `lucent-worker.exe`, Setup and the uninstaller, passed all 35 checks of
`Test-InstalledPackage.ps1` with the signer check, and left no certificate or key behind. Then run
`tools\package.ps1 -SkipBuild -StageOnly` (or a full package) without the variables, so no
test-signed Setup stays in the output folder.

**Testing the installer.** `installer\tests\Test-InstalledPackage.ps1` needs no developer tools. It
refuses to run when Lucent is already installed for the user, installs silently into a temporary
folder (with the Desktop shortcut and "Open with Lucent" tasks), checks the files against the
manifest and `Lucent-files.txt`, checks that what Windows opens each RAW type with is unchanged
(`AssocQueryString`), runs the installed `Lucent.exe` with `PATH` reduced to Windows' folders and a
temporary `--data-root`, records the modules it loads, upgrades in place while Lucent runs (with
stale listed files, which must go, and another program's files, which must stay), uninstalls and
checks that no file, shortcut or registry entry is left while the data root remains.
`-RuntimeOutcomes` also checks Setup's handling of the runtime installer's results through a test
hook that runs nothing (honoured only where the runtime is already installed):

```powershell
$pkg = 'build\release\package'
powershell -NoProfile -File installer\tests\Test-InstalledPackage.ps1 `
    -Installer "$pkg\Lucent-Setup-0.1.0-x64.exe" -Manifest "$pkg\Lucent-Setup-0.1.0-x64.manifest.json" `
    -Fixture fixtures\cache\canon-eos-r5-cr3-raw\Canon_EOS_R5_RAW_ISO_100_nocrop_nodual.CR3 -RuntimeOutcomes
```

The clean-machine gate (a Windows 10/11 machine or Windows Sandbox without developer tools or the
Visual C++ runtime, offline) runs the same script and is still pending (ADR 0009, decision 11).

Useful Setup options for silent installs: `/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /DIR="<folder>"
/TASKS="desktopicon,openwith" /LOG="<file>"`; the uninstaller is `<folder>\unins000.exe` with
`/VERYSILENT /SUPPRESSMSGBOXES /NORESTART`.

## Test fixtures

Unit tests use synthetic data and always run. Tests that need real camera files look for them under
`fixtures\` (the `LUCENT_FIXTURE_DIR` compile definition). When a fixture is absent they call
`GTEST_SKIP()` with a message, so a fresh checkout still passes.

To download the small, route-diverse smoke set used by fixture-dependent tests:

```powershell
powershell -NoProfile -File tools\fetch-fixtures.ps1 -Tier smoke
```

`fixtures\manifest.json` lists every fixture with its source, SHA-256 digest, tier and licence.
`-Tier qualification` or `-Tier all` fetches the wider camera/mode matrix from blueprint section 3.
Run `Get-Help .\tools\fetch-fixtures.ps1 -Detailed` for the other options. Files are downloaded
on demand into `fixtures\cache\` (git-ignored), verified against their digests and never committed.
The W1 fixtures come from raw.pixls.us, and each is individually marked CC0-1.0. The manifest
records the licence check and the known gaps in camera coverage.

## Research environment

The Python research environment (rawpy reference decoding and numerical parity scripts) is
described in [`research/README.md`](../../research/README.md). It is a development tool only: the
installed application never needs Python (blueprint Appendix D, ADR 0003). Keep it separate from
the tools venv that holds CMake.

## Qt and UI builds

The libraries, `lucent-probe` and `lucent-render` need no Qt: use the `dev-core` preset. `Lucent.exe`
and its UI tests need the `dev` preset. On a machine whose binary cache holds Qt (the reference
machine's does), the first `dev` configure restores it in about a minute; otherwise Qt is built
from source as described below.

- **Qt 6.11.1 is built from source through vcpkg** by the `ui` manifest feature. The sources are
  pinned by SHA-512 in the port files, and `qtbase` is built with its default features disabled and
  an explicit minimal feature list (see `vcpkg.json`), plus `qtdeclarative`, `qtshadertools` and
  `qtsvg`. The first build takes hours and a lot of memory. Once a machine has built Qt, the binary
  cache restores it.
- **Why not prebuilt Qt:** installing prebuilt Qt 6.11.3 with aqtinstall was tried and abandoned.
  Qt's online repository publishes only SHA-1 checksums for 6.11 archives, aqtinstall expects
  SHA-256 from a trusted mirror, and downgrading verification to SHA-1 was rejected as a security
  weakening (ADR 0002).
- **Alternative for individual developers:** the official Qt Online Installer (requires a Qt
  account, is interactive and is not automated). The presets do not target it, so using it would
  need local CMake changes. It is not a supported configuration.
- **Building Qt without blocking other work:** vcpkg holds a filesystem lock on its checkout for
  the whole of a long build. Build Qt through the second checkout (`vcpkg-qt`, same commit) so
  everyday configures against the main checkout are not blocked. Because both checkouts are at the
  same commit, the packages have identical ABI hashes and land in the shared binary cache, and the
  main checkout then restores them instead of rebuilding. From a **new** PowerShell window (so
  `dev-env.ps1` has not already loaded the main checkout):

  ```powershell
  $env:LUCENT_VCPKG_ROOT = "$env:USERPROFILE\dev\vcpkg-qt"
  $env:VCPKG_MAX_CONCURRENCY = '4'   # limit vcpkg's parallel port builds on a 12 GB machine
  powershell -NoProfile -File tools\build.ps1 -Preset dev -BuildDir build\qt-warm -ConfigureOnly -Jobs 2
  ```

  This command follows from the design above but has not yet been run end to end. The reference
  machine's current Qt build was started separately with the `vcpkg-qt` checkout.

## Troubleshooting

### "running scripts is disabled on this system"

Set the execution policy for your user: `Set-ExecutionPolicy -Scope CurrentUser RemoteSigned`. No
administrator rights are needed. See [step 0](#0-powershell-execution-policy) for ZIP downloads.

### `'vswhere.exe' is not recognized as an internal or external command`

Visual Studio's developer-shell scripts print this line while `Enter-VsDevShell` runs, because
they invoke `vswhere.exe`, which is not on `PATH`. It is harmless: the environment loads correctly
and the next line reports `Lucent dev environment: MSVC 14.44.35207, ...`.

### Paths too long (`LongPathsEnabled=0`)

The reference machine runs with Win32 long paths disabled (`MAX_PATH` = 260). vcpkg's build trees,
especially Qt's, create deep paths: `qtdeclarative` failed in `moc` with a 263-character path when
its build tree lived under `C:\Users\Alcor\dev\vcpkg-qt\buildtrees`. `tools\dev-env.ps1` therefore
sets `LUCENT_VCPKG_BUILDTREES` (default `<LUCENT_DEV_ROOT>\vb`), and the root `CMakeLists.txt` passes
it to vcpkg as `--x-buildtrees-root`. For a manual `vcpkg install`, pass
`--x-buildtrees-root=<short dir>` yourself. Keep build directories short too. With a long user name, point
`LUCENT_DEV_ROOT` at a short directory such as `C:\dev`, and use short `-BuildDir` names. Enabling
long paths needs administrator rights and is not required.

### Out of memory, or the machine becomes unresponsive while building

Each MSVC compiler process can use close to 1 GB. On the 12 GB reference machine use `-Jobs 2` to
`-Jobs 4`; the bootstrap check prints a suggestion based on installed memory. vcpkg builds ports
with all cores by default. Set `$env:VCPKG_MAX_CONCURRENCY` (for example `4`) before a configure
that must build packages from source, such as the first Qt build. If several builds run at once,
their `-Jobs` values add up.

### "waiting to take filesystem lock on ...\.vcpkg-root"

Another vcpkg process (another build directory, another developer session or the Qt build) is using
the same vcpkg checkout. Wait: vcpkg continues when the lock is released, and a fresh build
directory then restores from the binary cache in about 30 seconds. Do not delete lock files or kill
the other process. The lock is released when that process exits. Long Qt builds belong in the
`vcpkg-qt` checkout so they do not hold this lock.

### Every package rebuilds from source (`VCPKG_ROOT`)

A healthy configure reports `Restored N package(s) from ...\vcpkg\archives` and finishes in under a
minute. If vcpkg starts building LibRaw, zlib and the rest from source:

1. Check the vcpkg path in the line `Lucent dev environment: ... vcpkg at <path>`. It must be the
   pinned checkout, not `...\Microsoft Visual Studio\...\VC\vcpkg`. Build through `tools\build.ps1`
   or dot-source `tools\dev-env.ps1`, and set `LUCENT_VCPKG_ROOT`, never `VCPKG_ROOT`.
2. Check that the checkout is at the baseline and the tool is bootstrapped:
   `powershell -NoProfile -File tools\bootstrap-dev.ps1`.
3. A different MSVC version also changes every ABI hash. That is expected after a Build Tools
   update (ADR 0002, decision 10).

### `dev-env.ps1` keeps using the wrong vcpkg checkout

The environment is loaded only once per PowerShell session (`LUCENT_DEV_ENV_LOADED=1`), and child
`powershell` processes inherit it. To switch checkouts, open a new PowerShell window, set
`LUCENT_VCPKG_ROOT` and run the build again.

### Which Ninja is used?

`dev-env.ps1` puts `%LOCALAPPDATA%\Microsoft\WinGet\Links` first on `PATH` only if it contains
`ninja.exe`. On the reference machine winget installed Ninja 1.13.2 into its package directory
without creating that link, so builds use the Ninja 1.12.1 bundled with Visual Studio. Both work.
The bootstrap check reports which one builds will use.

### Configure fails after changing presets, features or the triplet

Reconfigure from scratch: `powershell -NoProfile -File tools\build.ps1 -Preset dev-core -Fresh`.
Deleting the build directory also works; its packages are restored from the binary cache.
