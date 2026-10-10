# ADR 0009: Windows packaging: installed layout, dependency audit and per-user Inno Setup installer

- **Status:** Accepted (amended 2026-10-09: "Amendment: build-machine paths and the signing rehearsal" below)
- **Date:** 2026-10-03
- **Workflow:** W5 (Installer slice)

## Context

Blueprint section 9 ("Double-click Windows delivery") asks for `Lucent-Setup-<version>-x64.exe` built
with Inno Setup: a per-user installer (`PrivilegesRequired=lowest`, `%LOCALAPPDATA%\Programs\Lucent`)
with Start Menu and Desktop shortcuts, optional RAW file associations and an uninstaller; Qt, its
plugins and QML imports and every other library bundled, using "Qt's deployment tools" but
inspecting the result; Microsoft's official offline Visual C++ redistributable, run elevated only
when the installed runtime is not sufficient, with success, failure and reboot-required outcomes
handled "before offering launch"; Authenticode signing with keys outside the repository; and an
SBOM and licence bundle "from the actual build". Section 8 separates the installed application
folder from per-user data (catalog, settings) and the disposable cache. ADR 0002 makes every vcpkg
dependency a DLL (triplet `x64-windows-lucent`, dynamic CRT, release-only), and ADR 0003 requires
LGPL libraries (Qt, LibRaw) to ship as replaceable, unmodified DLLs, Qt's build tools and the test
libraries never to ship, and the notice bundle and SBOM to be generated from the files the
deployment step ships.

Facts established on the reference machine (2026-10-03):

- vcpkg lays Qt out differently from a Qt SDK: DLLs in `bin`, plugins in `Qt6/plugins`, QML modules
  in `Qt6/qml`, and `windeployqt.exe` with its own `qt.conf` in `tools/Qt6/bin`. Qt is relocatable.
- `windeployqt` (Qt 6.11.1) for Lucent's QML deploys every Qt Quick Controls style (FluentWinUI3
  alone is 854 files), the QML debugging, TUIO and TLS plugins, patches `Qt6Core.dll` unless told not
  to, and copies no non-Qt DLL: FreeType, HarfBuzz, PCRE2, zlib, LibRaw, Little CMS and the rest must
  be added by other means.
- `Qt6Core.dll` imports `icuuc.dll`, Windows' own ICU (catalog-signed by Microsoft Windows, version
  resource "The ICU Project"), present since Windows 10 1703.
- The x64 Visual C++ runtime registration exists in both registry views and they disagree: the
  64-bit view reports 14.51.36247 (matching `System32\vcruntime140.dll`), the 32-bit view 14.44.35211.
- The Visual Studio 2022 Build Tools carry `vc_redist.x64.exe` 14.44.35211.0, Authenticode-signed
  by Microsoft Corporation. Inno Setup 6.7.3 is installed (registered in the uninstall registry);
  its compiler reports "Non-commercial use only".
- Windows Sandbox is not available on this machine, and no clean virtual machine was available.

## Decision

1. **Installed layout.** `cmake --install <build> --prefix <folder> --component Lucent` produces the
   folder Setup installs:

   | Path | Contents |
   |---|---|
   | `Lucent.exe` | The application (WIN32 subsystem), the only executable |
   | `*.dll` | Qt modules and every other runtime DLL, byte-identical to the vcpkg installation |
   | `plugins\<type>\` | Qt plugins: `platforms` (qwindows), `imageformats` (gif, ico, jpeg, svg), `iconengines` (svg) |
   | `qml\` | The QML modules Lucent imports, with the Fusion and Basic styles only |
   | `qt.conf` | `Prefix = .`, `Plugins = plugins`, `QmlImports = qml`; no absolute or build-tree path |
   | `licenses\` | Licences, notices and the SBOM (decision 10), added by `tools\package.ps1` |
   | `Lucent-files.txt` | Every file the package installs, relative to the folder (itself and the SBOM included); the next version's Setup uses it to upgrade (decision 8) |
   | `unins000.exe`, `unins000.dat` | Inno Setup's uninstaller (installed by Setup) |

   The developer tools (`lucent-probe`, `lucent-render`) and the tests have no install rules; the
   build-tree `bin\qt.conf` (ADR 0007) is never installed. The release preset builds the package.

2. **Deployment method** (`cmake/LucentPackaging.cmake`, run by `cmake --install`):
   1. `windeployqt` (vcpkg's, as `Qt6::windeployqt`) with `--qmldir apps/lucent/qml`,
      `--plugindir plugins`, `--qml-deploy-dir qml`, `--no-patchqt` (Qt6Core stays identical to the
      vcpkg build; `qt.conf` provides the prefix), `--no-compiler-runtime`, `--no-translations`,
      `--no-system-d3d-compiler`, `--no-system-dxc-compiler`, `--no-opengl-sw` and
      `--skip-plugin-types qmltooling,generic,networkinformation,tls`. The Qt plugins Lucent cannot
      run without (`LUCENT_PACKAGE_REQUIRED_PLUGINS`: `platforms\qwindows.dll`, and
      `imageformats\qjpeg.dll`, through which camera previews and thumbnails are decoded) must be
      among the deployed files, or the install fails.
   2. QML modules Lucent never loads are removed: the FluentWinUI3, Imagine, Material, Universal and
      Windows styles, `QtQuick.NativeStyle`, and those styles' variants of the non-native dialogs.
      Lucent selects Fusion in code (`QQuickStyle::setStyle`, which takes precedence over
      `QT_QUICK_CONTROLS_STYLE`: the pruned layout starts normally with that variable set to
      `Material`), and Fusion falls back to Basic.
   3. `qt.conf` is written for the installed layout.
   4. `file(GET_RUNTIME_DEPENDENCIES)` (dumpbin) resolves the imports of `Lucent.exe` and of every
      plugin against the folder, then vcpkg's `bin`, then only `%SystemRoot%\System32` and
      `%SystemRoot%` (the developer `PATH` is never searched). DLLs found in vcpkg's `bin` are
      copied; Windows DLLs and the Visual C++ runtime DLLs are never copied; a DLL found anywhere
      else, an unresolved import or a conflict fails the install. Root DLLs that nothing imports any
      more (those of the removed styles) are deleted, and empty folders left by `windeployqt` too.
   5. A second resolution pass against the folder alone must find everything.

   `tools\package.ps1` drives the whole pipeline: release build through `tools\build.ps1` (target
   `Lucent`, or everything plus tests with `-RunTests`; run in the same PowerShell process, so paths
   need no command-line quoting), install, audit and smoke run, optional signing, licences, the
   installed-file list and SBOM, Inno Setup, manifest. It first deletes the installer, checksum,
   manifest and SBOM copy of an earlier run of the same version, so a manifest never pairs a new
   layout with an old installer; with `-StageOnly` the manifest records no installer.

3. **Dependency audit** (`installer\scripts\LucentPackaging.psm1`, `Invoke-LayoutAudit`). An
   independent second check, in PowerShell with `dumpbin /dependents`, that fails packaging on any
   error:
   - every import and delay-load import of every PE file resolves to a DLL in the folder, an API set
     (`api-ms-*`, `ext-ms-*`), a Windows DLL (present in `System32` and catalog-signed by
     "Microsoft Windows"), or one of the Visual C++ runtime DLLs that `vc_redist.x64.exe` installs
     (the names are taken from the redistributable's folder in the Visual Studio installation);
   - every file is `Lucent.exe` (identical to the build output), a generated file (`qt.conf`,
     `licenses\`), or a file of a vcpkg port, byte-identical to the vcpkg installation (mapped through
     `vcpkg_installed\vcpkg\info\*.list`); ports listed as never shipped (GoogleTest, Qt Shader
     Tools, Qt Language Server, build helpers) are errors;
   - every DLL at the root of the folder is imported by something (a DLL nothing imports means the
     plugin or QML module that used it is gone);
   - the DLLs Qt loads by name at run time, which no import table shows, are present: the required
     Qt plugins, and every QML module Lucent uses (each module the QML sources import that Qt
     provides, plus `QtQuick.Controls.Fusion`, the style selected in code, followed through the
     non-optional `import` and `depends` lines of their `qmldir` files), and for every deployed
     `qmldir` the plugin DLL it names (`plugin` or `optional plugin`);
   - exactly one executable, `Lucent.exe`, with the Windows GUI subsystem; every PE file is x64; no
     `pdb`, `lib`, `exp`, `ilk` or `obj` file; no `.conf` or `.ini` naming a build-tree path or a path
     outside the folder. Binaries are not scanned for paths: the vcpkg-built DLLs carry their
     builder's PDB paths in their debug directories (W12 can add `/PDBALTPATH` to the triplet);
     `Lucent.exe` carries none.

   Then a **smoke run** starts the staged `Lucent.exe` with `PATH=%SystemRoot%\System32;%SystemRoot%`,
   no Qt variables, `QML_DISABLE_DISK_CACHE=1` and a temporary `--data-root`, waits for the window
   to settle and saves a screenshot. Packaging fails unless it exits with 0, Qt and QML report no
   loading error on stderr, and the folder is unchanged afterwards. This covers what the static
   checks cannot: a QML module reached only from QML compiled into a plugin (for example
   `QtQuick.Controls.Fusion.impl`), or a platform plugin that fails to load.

   The report (`audit.txt`, `audit.json`) lists the shipped ports, the runtime and Windows imports,
   the QML modules checked and every file's origin. For 0.1.0: 277 files before the licences,
   64 PE files, 17 ports, 13 QML modules checked, no warning; runtime imports `msvcp140`,
   `msvcp140_1`, `msvcp140_2`, `msvcp140_atomic_wait`, `vcruntime140`, `vcruntime140_1`. Deleting
   `qwindows.dll`, `qjpeg.dll`, the Fusion style plugin, `Qt.labs.folderlistmodel` or
   `QtQuick.Layouts` from a copy of the folder makes the audit fail; deleting
   `QtQuick\Controls\Fusion\impl` or `qwindows.dll` makes the smoke run fail.

4. **Privileges.** `PrivilegesRequired=lowest`, no `PrivilegesRequiredOverridesAllowed`: Setup never
   elevates itself, installs into `{autopf}\Lucent` = `%LOCALAPPDATA%\Programs\Lucent` (the folder can
   be changed), writes only to `HKCU` (`HKA`), and creates per-user shortcuts. An all-users
   installation is not offered. Lucent runs with standard user rights. `ArchitecturesAllowed` and
   `ArchitecturesInstallIn64BitMode` are `x64os`: x64 Windows only. Windows on Arm would run the x64
   build under emulation, but there the runtime detection (registry key and `System32` file
   versions) and Microsoft's advice (the Arm64 redistributable) differ, and nothing was tested, so
   Setup refuses it until that path is tested. `MinVersion=10.0.17763` (Windows 10 1809).

5. **Visual C++ runtime.** The official `vc_redist.x64.exe` from the Visual Studio installation is
   bundled unmodified; `tools\package.ps1` refuses one that is not validly signed by Microsoft
   Corporation, and passes its version (14.44.35211) to Setup as the minimum. Setup
   (`PrepareToInstall`) treats the runtime as present when `HKLM\SOFTWARE\Microsoft\VisualStudio\
   14.0\VC\Runtimes\x64` reports `Installed=1` and `Major.Minor.Bld` at least that version in either
   registry view, and every runtime DLL Lucent imports (from the audit) exists in the 64-bit
   `System32` with at least that version. Otherwise it extracts the redistributable and runs it with
   `ShellExec('runas', ..., '/install /quiet /norestart')`, then:

   | Exit code | Meaning | Setup |
   |---|---|---|
   | 0 | Installed | Re-checks; continues, or stops if the runtime is still not sufficient |
   | 1638 | A newer runtime is installed | Re-checks (Setup ran it because its own check failed); continues, or stops with the advice to restart or repair the redistributable |
   | 3010, 1641 | Installed, restart required | Continues, then asks for a restart (`NeedRestart`) and does not offer "Launch Lucent" |
   | 1602, 1223, or the elevation prompt declined (`ShellExec` fails with 1223) | Cancelled | Stops before installing anything, with a message naming the runtime and what to do |
   | 1618 | Another installation is running | Stops with a message to retry later |
   | other, or failure to start | Failed | Stops with the code and the manual remedy |

   The hidden Setup parameter `/LucentTestVCRedistExitCode=<code>` simulates an exit code, and
   `/LucentTestVCRedistExitCode=start-failed:<error>` a redistributable that could not be started
   (`start-failed:1223`: the elevation prompt declined), without running or elevating anything, so
   the outcome handling can be tested on any machine. Setup honours it only when the installed
   runtime is already sufficient; otherwise it logs that the hook is ignored and runs the real
   redistributable. The hook therefore cannot produce an installation that lacks the runtime, and
   it stays in the shipped Setup so the installer that is released is the one that was tested.

6. **"Open with Lucent".** An optional, unchecked task registers, per user, the ProgID
   `Lucent.RawPhoto` (`"<app>\Lucent.exe" "%1"`) and `Applications\Lucent.exe` with `SupportedTypes`
   listing every RAW extension of the importer (`kRawExtensions` in
   `src/application/src/import.cpp`, read by `tools\package.ps1`, 44 today). It never writes an
   extension's default value or `UserChoice`, and it never changes what a double-click opens:
   - A `Lucent.RawPhoto` value under `Software\Classes\.<ext>\OpenWithProgids` is added only for an
     extension that already has a default program when Setup runs (a `UserChoice` or
     `UserChoiceLatest` ProgID, or an existing ProgID as the extension's default value). The
     default keeps winning, and Lucent appears in that type's "Open with" menu.
   - Without a default program, an `OpenWithProgids` entry would change the result of a
     double-click: Windows resolves such a type through its `OpenWithProgids` list, so Lucent would
     become the program that opens it, or a type that Photos alone listed would start asking which
     program to use. For those types Lucent is listed through `SupportedTypes` only: it is offered
     under "Open with" > "Choose another app", and the user may pick it there.

   On the reference machine 33 of the 44 types had a default program; the 11 others (`.cs1`, `.dc2`,
   `.gpr`, `.kc2`, `.qtk`, `.sti`, `.x3f` with no program, and `.mdc`, `.ptx`, `.pxn`, `.rwz` with
   Photos listed but not set as default) get `SupportedTypes` only. `AssocQueryString` (command,
   program name, ProgID) returns the same for all 44 before installation, while installed and after
   uninstallation. Uninstall deletes Lucent's keys and values and removes extension keys only when
   they are left empty. `ChangesAssociations=yes` refreshes Explorer.

7. **User data on uninstall.** The uninstaller removes the program folder, its shortcuts and
   registry entries, and keeps the catalog and its backups, settings (`HKCU\Software\Lucent`) and
   caches in `%LOCALAPPDATA%\Lucent`. Its confirmation and completion messages say so; photographs
   and exports are never touched.

8. **Upgrades.** A fixed `AppId` (`{ADBA4913-BBBD-42D8-90EF-0E9C80412232}`, never to change) makes a
   newer Setup upgrade in place. Files the previous version installed and the new one no longer
   ships must not linger, and the folder may be shared with other programs (Setup accepts an
   existing, non-empty folder), so Setup deletes only what Lucent itself installed:
   - Every package carries `Lucent-files.txt`, the relative path of every file it installs
     (checked against the folder when packaging).
   - Before copying anything (`ssInstall`), Setup reads the previous version's list from the
     target folder. A folder without a list (a fresh installation, or a version without one; none
     was published) is never cleaned. A list that does not start with its header line, or that
     holds an absolute path, a `..` or a wildcard, disables the cleanup entirely.
   - After the new files are in place (`ssPostInstall`), Setup deletes exactly the listed files
     that the new list does not contain, then the folders that this leaves empty (`RemoveDir`
     removes only empty folders). By then Restart Manager has already closed a running Lucent
     (`CloseApplications=yes`), so its files are not in use.

   On the reference machine an installation into a folder that already held another program's
   DLLs, plugins, QML module and licence file kept all of them through an upgrade and an
   uninstallation, while the stale files planted in the list were removed after Restart Manager
   had closed a running Lucent. Inno Setup's own uninstaller likewise removes only the files it
   recorded.

9. **Signing.** Off by default: the 0.1.0 package and its binaries are **unsigned**, and Windows
   SmartScreen will warn about them. Setting `LUCENT_SIGN_CERT_SHA1` (certificate thumbprint in the
   user's store) and `LUCENT_SIGN_TIMESTAMP_URL` (RFC 3161) makes `tools\package.ps1` sign
   `Lucent.exe` with `signtool` (`/fd sha256 /tr ... /td sha256`) and passes the same command to
   Inno Setup (`/S`), which signs Setup and the uninstaller (`SignedUninstaller=yes`); the
   signatures are verified. Third-party DLLs are not re-signed. Keys never live in the repository.

10. **Licences and SBOM**, generated into `licenses\` from the audited folder:
    - `Lucent-LICENSE.txt`; for each shipped port (from the audit's provenance, not a hand list) and
      each header-only direct dependency compiled into `Lucent.exe` (nlohmann-json, tl-expected: the
      direct dependencies in `vcpkg.json` that install no binary), `<port>\copyright.txt` from vcpkg;
    - licence texts vcpkg lacks, extracted from the port's upstream source archive after checking
      its SHA-512 against vcpkg's SPDX record (`README.ijg` of libjpeg-turbo, `LICENCE.md` of PCRE2);
      Qt's `LGPL-3.0-only.txt` and `GPL-3.0-only.txt` extracted from the qtbase copyright file;
    - the **third-party code inside the shipped Qt files**, from Qt's own SBOMs
      (`vcpkg_installed\<triplet>\sbom\<qt port>-<version>.spdx`): the Qt targets whose files ship
      are found through the SBOM's file entries, and every `<target>_Attribution_<id>` package they
      depend on is listed with its name, version, holders, licence and source location (32 for
      0.1.0: Unicode data, hash functions, TinyCBOR, the easing equations, the Public Suffix List
      under MPL-2.0 with its source URL, the Vulkan and D3D12 memory allocators, the Wintab API,
      the JavaScriptCore macro assembler, Yoga, XSVG and others). Packaging fails if one of their
      licences has no text in the shipped Qt copyright files;
    - the **libraries linked statically into `Lucent.exe`**: the vcpkg `.lib` files on its link line
      (`build.ninja`) whose members are object files (`lib /list`). Each needs a reviewed entry in
      the policy, checked against Qt's SBOM where it has one, and packaging refuses one used under
      an LGPL or GPL licence. For 0.1.0 this is Qt's Windows entry point, `Qt6EntryPoint.lib`
      (`LicenseRef-Qt-Commercial OR BSD-3-Clause`, used under BSD-3-Clause);
    - `LGPL-NOTICE.txt`: the LGPL libraries (Qt, LibRaw), the right to replace their DLLs, the
      statically linked non-LGPL code, the upstream archives with SHA-512 and, for each recipe, the
      git tree hash of the port folder (what vcpkg's SPDX `downloadLocation` names; it is not a
      commit) and, when the registry baseline commit holds exactly that tree, a link to the port
      folder at that commit (all ports of 0.1.0);
    - `THIRD-PARTY-NOTICES.txt`: every component with version, licence, files, the required
      credit lines (IJG, FreeType, libtiff's LZW code), the statically linked code and the
      third-party code inside Qt;
    - `Lucent.spdx.json`, valid SPDX 2.3: Lucent as a package with `filesAnalyzed` and a package
      verification code over its own files (the SBOM itself excluded); for each component vcpkg's
      port, binary and source packages, the recipe files (with the SHA-1 that SPDX requires, taken
      from the identical file in the vcpkg ports tree) and the binary files that actually ship
      (SHA-1 and SHA-256), linked by `DYNAMIC_LINK` or `STATIC_LINK`; the statically linked
      libraries (`STATIC_LINK`) and Qt's third-party code (contained in the Qt binary packages),
      with the texts of the `LicenseRef-` licences they use; the bundled redistributable as a
      `RUNTIME_DEPENDENCY_OF`. Packaging checks the document structurally (identifiers, SHA-1 on
      every file, no file contained in a package with `filesAnalyzed` false, every `LicenseRef-`
      defined) and, when `pyspdxtools` is available (`-SpdxValidator`, `LUCENT_PYSPDXTOOLS` or
      `PATH`), validates it with spdx-tools; the 0.1.0 SBOM passes spdx-tools 0.8.5.

    `installer\license-policy.json` holds only what cannot be derived: elections (Qt LGPL-3.0-only,
    LibRaw LGPL-2.1-only, FreeType FTL, Qt's entry point BSD-3-Clause), credit lines, missing texts,
    licences vcpkg does not declare (double-conversion), reviewed static libraries, and
    never-shipped ports. Each entry names the version it was reviewed for, and packaging fails when
    another version ships, so a dependency upgrade forces a licence review.

    How the corresponding source is provided to recipients (bundled archive or written offer)
    remains the W12 release decision of ADR 0003. Until then the installer is **not for public
    distribution**: it names the exact sources and recipes, but provides neither the sources nor a
    written offer, which LGPL-2.1 section 6 (LibRaw) and LGPL-3.0/GPL-3.0 section 6 (Qt) require
    when the libraries are conveyed in object form. `tools\package.ps1` prints this reminder.

11. **Verification.** `installer\tests\Test-InstalledPackage.ps1` (no developer tools needed)
    installs silently into a temporary folder with both tasks; compares every installed file with
    the package manifest and with `Lucent-files.txt`; checks `SupportedTypes`, that
    `OpenWithProgids` names Lucent exactly for the types that had a default program, and that
    `AssocQueryString` reports the same program for every RAW type as before; runs the installed
    `Lucent.exe` with `PATH=%SystemRoot%\System32;%SystemRoot%` and `--data-root`, `--import`,
    `--open-first`, `--screenshot`, recording the modules it loads; upgrades in place while Lucent
    runs, with stale listed files and another program's files planted in the root, `plugins`,
    `qml` and `licenses`; uninstalls, and checks that the folder, shortcuts, uninstall
    registration and association values are gone, the extension keys, effective associations and
    Explorer `FileExts` state are as before, and the data root remains. It also removes the
    temporary folder that Inno Setup's finished uninstaller leaves in `%TEMP%`.
    `-RuntimeOutcomes` adds the simulated outcomes 1602, 1223, 1618 and `start-failed:1223` (stop),
    0 and 1638 (install after a re-check) and 3010 (install, restart requested). On the reference
    machine all 46 checks pass.
    **Pending gate:** the same script on a clean Windows 10/11 machine without developer tools or
    the Visual C++ runtime (Windows Sandbox or a VM), offline, which exercises the real elevated
    redistributable path, including a declined elevation prompt and an interactive run of the
    wizard. It could not run here and must pass before a public release.

## Consequences

- `tools\package.ps1` produces a 38.4 MiB installer (40.3 MB; 24.4 MiB of it the redistributable)
  for a 55.2 MiB, 305-file installation in about a minute after the build. On the reference
  machine the installed copy rendered a CR3 and a NEF fixture with a Windows-only `PATH`; all 49
  Lucent and Qt modules came from the installation folder and the Visual C++ runtime from
  `System32`.
- A dependency the deployment misses fails the package build rather than a user's first launch.
  Imports are checked by the audit; the DLLs Qt loads by name at run time (every Qt plugin and
  every QML plugin) by the required-plugin and QML checks; and the start-up path by the smoke run.
  A QML module that is loaded only on a path the smoke run does not visit, and that no `qmldir`
  import names, is still caught only by the installer test or a person.
- The removed-styles list, the skipped plugin types and the required plugins must be revisited if
  Lucent changes its Qt Quick Controls style, uses networking, or decodes another image format
  through Qt.
- Packaging needs the upstream source archives of libjpeg-turbo and PCRE2 in vcpkg's downloads
  folder (present when the ports were built on the machine, absent after a pure binary-cache
  restore); it fails with instructions otherwise.
- The package is unsigned until a certificate exists; SmartScreen reputation is a separate matter.
- The package must not be distributed publicly before W12 settles the corresponding-source
  provision (decision 10).
- Inno Setup is not pinned by the repository; Setup requires 6.3 or later and was built with 6.7.3,
  whose compiler reports "Non-commercial use only". Its commercial terms must be reviewed before a
  public release (blueprint section 9). An explicit `-Iscc` or `LUCENT_ISCC` must be valid (no
  fallback); a compiler found elsewhere than a standard Inno Setup folder (on the reference
  machine, a portable copy registered by another project) causes a warning, and the manifest
  records its path, version and SHA-256.
- The vcpkg-built DLLs carry the builder's PDB paths (including the user name); Lucent.exe does
  not. Removing them needs a triplet linker flag (`/PDBALTPATH:%_PDB%`), left to W12.
- On machines with injected modules (here a Citrix client DLL in every process) the smoke run
  reports them separately; it fails only when such a module has the name of a DLL Lucent ships.
- Revisit when MSIX or an update channel is introduced, when a worker executable (W7) or model
  assets (W11) join the folder, or when an all-users installation is requested.

## Alternatives considered

- **`qt_generate_deploy_qml_app_script`.** On Windows it drives the same `windeployqt` through Qt's
  deploy support. Calling `windeployqt` directly keeps every flag visible (`--no-patchqt`,
  `--no-compiler-runtime`, the skipped plugin types) and leaves the non-Qt DLLs to one explicit
  mechanism; the generated script was not tried against vcpkg's split layout. Not chosen.
- **Copying every vcpkg DLL** (as the build tree does). Ships GoogleTest, unused codecs and tools'
  dependencies, and hides missing ones. Rejected for the import-driven closure plus audit.
- **Keeping every Qt Quick Controls style.** Over 1,100 more files and eight more DLLs that Lucent
  never loads. Rejected.
- **App-local Visual C++ runtime DLLs or the static CRT.** The blueprint and Microsoft's guidance
  prefer the official redistributable; app-local copies are not serviced by Windows Update, and the
  static CRT conflicts with the DLL-per-dependency model (ADR 0002). Rejected.
- **A per-machine installer (Program Files).** Needs an administrator for every install and update.
  Rejected by the blueprint, which asks for a per-user installer that elevates only the runtime.
- **Taking over RAW file associations.** Would change the user's defaults. Rejected; "Open with"
  registration only, opt-in.
- **An `OpenWithProgids` value for every RAW extension.** Simpler, and Lucent would appear in the
  "Open with" menu of every type, but for types without a default program it changes what a
  double-click does (decision 6). Rejected.
- **Deleting the previous version's `plugins`, `qml`, `licenses` and root DLLs wholesale before an
  upgrade.** Deletes other programs' files when the folder is shared, and ran before Restart
  Manager closed Lucent. Replaced by the installed-file list (decision 8).
- **Compiling the runtime test hook only into a test build.** The released installer would then
  differ from the tested one; the hook is instead honoured only where the runtime is already
  present (decision 5).
- **Deleting user data on uninstall, or asking.** Losing a catalog is not recoverable; the catalog
  and settings stay and the messages say where they are.
- **MSIX, WiX/MSI, NSIS.** The blueprint chooses Inno Setup for the first release; MSIX may be
  reconsidered with an update channel (W12).

## Amendment: build-machine paths and the signing rehearsal (W12, 2026-10-09)

### Context

Decision 3 did not scan binaries for paths, because every DLL vcpkg built carried its builder's
PDB path; the consequences left the fix to W12. ADR 0002's amendment removes those paths where they
are made (the triplet's linker and compiler options, and the same options for Lucent's own release
binaries); this amendment makes packaging prove it for every byte the package ships. Decision 9's
signing hooks had never run, because no certificate existed. W12 rehearsed them with a temporary
self-signed certificate that was trusted nowhere, and fixed what the rehearsal found.

### Decision

12. **No build-machine path in a shipped file.** Once the licences, the installed-file list and the
    SBOM exist, `tools\package.ps1` scans every byte of every file of the installation folder
    (`Invoke-BuilderPathAudit`, `installer\scripts\LucentPackaging.psm1`), then Setup, then the text
    of the package manifest before writing it. It searches for
    - the user profile (recorded at configure time in `lucent-package.json`, schema 5, and that of
      the packaging process), also without its drive letter (`\Users\<name>`);
    - the repository, the source and build trees, the vcpkg installed tree and the vcpkg root,
      buildtrees, packages and downloads folders (configure time and the environment), with their
      8.3 short forms where those differ;
    - any `X:\Users\<name>\` folder other than Public, Default and All Users, which also catches
      binaries that a binary cache brought from another machine;

    as UTF-8, ANSI and UTF-16LE text, with `\` or `/` (repeated, as in JSON and C strings), in any
    letter case, after `X:`, `\\?\X:` or the MSYS form `/x/`. A match stops the packaging and names
    the file, the byte offset, the encoding, whether the bytes lie in a PE debug directory (the
    CodeView PDB path) or in the file's data, the folder and the surrounding text
    (`builder-paths.txt`, `builder-paths.json`); a Setup that fails is deleted. The manifest records
    the names of the folders searched (never their paths), the files and bytes scanned and the
    hits, and names a tool below the user profile as `%USERPROFILE%\...` (the Inno Setup compiler
    on the reference machine). Every match starts at a separator (the drive letter is checked
    behind it), so the regular expression engine skips to `\` and `/` bytes: the folder of 0.1.0,
    66 MB, takes about 2 s.

13. **Signing, verified.** Decision 9 holds, with these changes from the rehearsal:
    - The certificate is checked before the build: in `Cert:\CurrentUser\My`, where
      `signtool /sha1` looks, with its private key, valid now, and usable for code signing. A
      thumbprint pasted from the certificate dialog works (spaces and the invisible mark the dialog
      adds are dropped). Unless the signature is a test signature (below), a self-signed
      certificate or a missing `LUCENT_SIGN_TIMESTAMP_URL` is refused.
    - Every signature is checked for what it must be, not by `Get-AuthenticodeSignature`'s status
      alone: signed by the configured thumbprint, verifying offline (WinVerifyTrust,
      `WINTRUST_ACTION_GENERIC_VERIFY_V2`, no revocation check and no network retrieval: the digest
      and the chain) and timestamped when a timestamp server is set. Before, any valid signature
      passed, also one by another certificate.
    - `LUCENT_SIGN_TEST=1` makes a test signature, for rehearsals only: the one failure accepted is
      the untrusted root (`CERT_E_UNTRUSTEDROOT`), the timestamp is optional, and the manifest says
      `signing.test`. Before, a rehearsal was impossible: the hook required a timestamp server, and
      an untrusted root failed its `Valid` check.
    - A `$` in the sign tool's path or arguments is doubled for Inno Setup, whose Sign Tool command
      lines use `$` as an escape character.
    - The manifest (schema 3) records the certificate (thumbprint, subject, issuer, expiry), test or
      release, the timestamp server and each signature checked (file, signer, WinVerifyTrust result,
      timestamp); `installer.signed` now means signed by that certificate and verified.
    - Inno Setup signs the uninstaller inside Setup (`SignedUninstaller`), so packaging cannot see
      it, and a signed uninstaller keeps its messages in `unins000.msg` beside `unins000.exe`.
      `installer\tests\Test-InstalledPackage.ps1` now counts that file as Inno Setup's (the file
      comparison would have failed for any signed package) and, with `-SignerThumbprint` (and
      `-AllowUntrustedRoot` for a test certificate), checks Setup, the installed `Lucent.exe`,
      `lucent-worker.exe` and `unins000.exe`.
    - Timestamping was not rehearsed: an RFC 3161 request sends each file's digest to the
      timestamp server over the network, which an offline rehearsal does not do. A release
      signature requires it.

### Evidence

Reference machine, 2026-10-09, MSVC 19.44.35207. "Before" is the installation folder of the W12
performance baseline (`build\w12-perf`); "after" is the `release` build of this change
(`build\w12-hyg-release`), made once every port had been rebuilt with the new triplet (ADR 0002
amendment), scanned by `tools\package.ps1 -StageOnly`.

| | Before | After |
|---|---|---|
| Installation folder scanned | 308 files, 66.0 MB | 308 files, 66.0 MB, in 2.1 s |
| PDB paths in PE debug directories | 63: every DLL, plugin and QML plugin vcpkg built | 0 |
| `__FILE__` strings | 2: `tiff.dll` (libtiff's `tif_dir.c`), `Qt6Quick.dll` (`qquickdesignersupport.cpp`) | 0 |
| `Lucent.exe`, `lucent-worker.exe` | 0 | 0 |
| Licences, SBOM, `qt.conf`, QML files, assets | 0 | 0 |
| Setup, package manifest | not scanned | 0 |

All 65 earlier hits named the vcpkg buildtrees folder below the user profile. Outside the package,
the 274 DLLs and EXEs vcpkg installed hold one path, in Qt's build tool `syncqt.exe`, which is never
shipped. `installer\tests\Test-BuilderPathScan.ps1` (54 checks, no build needed) covers the
encodings, the separators, the drive forms, the exclusions and the debug-directory classification.

The signing rehearsal (2026-10-09):

1. A temporary certificate: `New-SelfSignedCertificate -Type CodeSigningCert -CertStoreLocation
   Cert:\CurrentUser\My`, RSA 3072, SHA-256, a non-exportable key in the Microsoft Software Key
   Storage Provider, valid two days. Windows also put a copy without the key into
   `CurrentUser\CA` (intermediate certification authorities); no Root or TrustedPublisher store
   changed.
2. `tools\package.ps1 -SkipBuild` with `LUCENT_SIGN_CERT_SHA1` and `LUCENT_SIGN_TEST=1`, no
   timestamp server: `Lucent.exe` and `lucent-worker.exe` signed (`signtool sign /fd sha256 /sha1`),
   and Inno Setup's sign tool ran for the uninstaller (`uninst.e32.tmp`) and for Setup. Every
   signature was verified: signed by the thumbprint, WinVerifyTrust `0x800B0109` (the untrusted
   root, accepted for a test signature), no timestamp. 73 s; Setup 39.9 MiB; the manifest records
   `signed`, `signing.test` and the three signatures.
3. Independent checks of the three files: `signtool verify /pa /v` names the certificate's SHA-1
   hash and ends with "SignTool Error: A certificate chain processed, but terminated in a root
   certificate which is not trusted by the trust provider" (exit code 1), as it must for a
   certificate that nothing trusts; `Get-AuthenticodeSignature` reports UnknownError with the same
   message and the expected signer.
4. `installer\tests\Test-InstalledPackage.ps1 -SignerThumbprint <thumbprint> -AllowUntrustedRoot
   -Tasks ''` (installed for the user into `%TEMP%` and uninstalled): 35 of 35 checks passed,
   including the signatures of Setup and of the installed `Lucent.exe`, `lucent-worker.exe` and
   `unins000.exe`. Setup logged "Writing uninstaller messages: ...\unins000.msg"; before this
   amendment the test's file comparison would have failed on that file.
5. The certificate was deleted from both stores, with its private key (`Remove-Item -DeleteKey` in
   `Cert:\CurrentUser\My`). Afterwards no store holds it (`Get-ChildItem Cert:\ -Recurse`),
   `certutil -user -store My <thumbprint>` fails with `0x80090011` (NTE_NOT_FOUND), the CNG key no
   longer exists, its file in `%APPDATA%\Microsoft\Crypto\Keys` is gone, and every certificate store
   of the user and the machine holds what it held before the rehearsal. (A first attempt stopped
   in the rehearsal's own script before packaging. Its cleanup ran too: no store holds that
   certificate, its CNG key does not exist, and the stores held as many certificates before the
   second attempt as before the first.)

`tools\package.ps1 -SkipBuild -StageOnly` then replaced the test-signed outputs with an unsigned
folder (0 hits in 309 files).

### Consequences

- A package can no longer ship a path of the machine that built it: a dependency, compiler or tool
  that brings one back stops the packaging with the file and the byte offset. The scan adds about
  2 s.
- The published manifest names the folders searched only by name, and the Inno Setup compiler as
  `%USERPROFILE%\...`.
- Release signing needs a code-signing certificate from a certification authority with its key in
  the user's store (a hardware token's certificate appears there too), and a timestamp server; the
  hooks refuse anything else unless `LUCENT_SIGN_TEST=1`. Not rehearsed: timestamping, a key on a
  hardware token, SmartScreen reputation, and the time Windows takes to check the signature of a
  40 MB Setup (Inno Setup suggests disk spanning for large signed setups; the single file is kept
  until a measurement says otherwise).
- A test-signed package is never distributed: its signatures chain to no trusted root and, without
  a timestamp, stop verifying when the certificate expires.
- The clean-machine gate (decision 11) should run `Test-InstalledPackage.ps1 -SignerThumbprint`
  with the release certificate, which checks the uninstaller that packaging cannot see.
