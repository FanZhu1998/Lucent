# ADR 0002: Build system and dependency management

- **Status:** Accepted (amended 2026-10-09: "Amendment: no build-machine paths in the binaries" below)
- **Date:** 2026-09-24
- **Workflow:** W0 (toolchain), recorded in W1

## Context

Lucent is a native C++20 Windows application (blueprint sections 1 and 9). Its engine depends on
about a dozen native libraries (LibRaw, Little CMS, libjpeg-turbo, libtiff, SQLite, zlib and their
transitive dependencies), and the application will add Qt 6. The blueprint requires "CMake and a
pinned dependency manifest/lockfile" (section 9) and reproducibility within documented tolerances
(section 8), which requires knowing exactly which library versions and build flags produced a
binary.

Constraints on the reference machine (`docs/development/roadmap.md`): Windows 11 x64, 6 cores / 12
threads, 12 GB RAM, `LongPathsEnabled=0`. Several developers or agents build in parallel, sometimes
alongside a multi-hour Qt build, so dependency builds must be cached and shared, and parallel
configures must not corrupt each other.

Facts established while setting up the toolchain (W0, verified 2026-09-24):

- `Enter-VsDevShell` sets `VCPKG_ROOT` to the vcpkg copy bundled with Visual Studio. Using that copy
  changed every package ABI hash (its helper scripts differ from the pinned checkout) and defeated
  the binary cache.
- Prebuilt Qt 6.11.3 through aqtinstall could not be verified: Qt's online repository publishes
  only SHA-1 checksums for 6.11 archives, aqtinstall expects SHA-256 from a trusted mirror, and
  downgrading verification to SHA-1 was rejected as a security weakening.

## Decision

1. **Toolchain.** MSVC from Visual Studio 2022 Build Tools (reference: 17.14.41, toolset
   14.44.35207, Windows SDK 10.0.26100), C++20 without extensions, 64-bit only. CMake 3.28 or newer
   (reference 4.4.3, pinned with pip in a Python virtual environment) with the single-configuration
   Ninja generator.
2. **Presets.** `CMakePresets.json` defines `dev-core` (engine, tools and tests; no Qt), `dev` (adds
   the Qt Quick application) and `release`. Each preset selects vcpkg manifest features:

   | Preset | Build type | `VCPKG_MANIFEST_FEATURES` | `LUCENT_BUILD_UI` |
   |---|---|---|---|
   | `dev-core` | RelWithDebInfo | `tests` | OFF |
   | `dev` | RelWithDebInfo | `tests;ui` | ON |
   | `release` | Release | `tests;ui` | ON |

3. **vcpkg manifest mode.** `vcpkg.json` declares dependencies: engine libraries by default,
   GoogleTest in the `tests` feature and Qt in the `ui` feature. `vcpkg-configuration.json` pins
   the built-in registry to one baseline commit (`e143402b5545bab137cc465225a82da63cb2148f`), so
   every dependency version follows from that commit. There are no per-port version overrides.
   The resolved versions are recorded in `docs/legal/third-party-notices.md`.
4. **One pinned vcpkg checkout.** Developers clone vcpkg to `%USERPROFILE%\dev\vcpkg` (overridable
   with `LUCENT_VCPKG_ROOT`), check out exactly the baseline commit and bootstrap the vcpkg tool
   release that commit pins (2026-07-27). The checkout's helper scripts are part of every package
   ABI hash, so identical checkouts produce identical hashes on every machine, which makes binary
   caching and reproducibility work. `tools/dev-env.ps1` ignores any pre-existing `VCPKG_ROOT` and
   sets it again *after* `Enter-VsDevShell`. An optional second checkout at the same commit
   (`%USERPROFILE%\dev\vcpkg-qt`) runs the long Qt build, so its vcpkg filesystem lock does not
   block everyday configures. Because the commit is the same, packages it builds have identical ABI
   hashes and are restored from the shared binary cache by the main checkout.
5. **Overlay triplet `x64-windows-lucent`** (`cmake/triplets/`): shared libraries, dynamic CRT,
   release-only (`VCPKG_BUILD_TYPE release`). It is used as both the target and the host triplet.
   - Shared libraries keep LGPL components (LibRaw, Qt) replaceable (ADR 0003).
   - Release-only builds take roughly half the dependency build time of the default
     debug-plus-release triplet.
   - Using the same triplet for the host means Qt's host tools are built once, not twice.
6. **Binary caching.** vcpkg's default local binary cache (`%LOCALAPPDATA%\vcpkg\archives`) is
   shared by all build directories on a machine. A new build directory restores its packages in
   about 30 seconds instead of rebuilding them.
7. **Qt from source through vcpkg.** The `ui` feature builds Qt 6.11.1 (the version at the
   baseline) from sources pinned by SHA-512 in the port files. `qtbase` is built with default
   features disabled and an explicit feature list (see `vcpkg.json`), plus `qtdeclarative`,
   `qtshadertools` and `qtsvg`. The `dev-core` preset does not need Qt, so engine work is never
   blocked by the Qt build.
8. **Helper scripts.** `tools/bootstrap-dev.ps1` checks and installs prerequisites,
   `tools/dev-env.ps1` loads the MSVC environment and pinned tools, and `tools/build.ps1`
   configures, builds and tests a preset. It takes `-BuildDir` for parallel build trees and `-Jobs`
   to limit parallelism. The definition-of-done command is
   `powershell -NoProfile -File tools\build.ps1 -Preset dev-core -Test`.
9. **Warnings.** Lucent targets compile with `/W4 /permissive- /WX` and selected extra warnings
   through `Lucent::compiler_options`. Third-party headers are external (`/external:W0`). Warnings in
   Lucent code are fixed, never disabled globally.
10. **Changing versions.** A dependency upgrade is a deliberate change. It moves the baseline in
    `vcpkg-configuration.json` and every pinned checkout to the new commit, re-bootstraps the tool,
    rebuilds, passes the definition-of-done command, and updates the licence ledger in the same
    change. An MSVC upgrade is treated the same way, because the compiler identity is also part of
    every package ABI hash.

## Consequences

- Every machine resolves the same dependency versions and build flags from two committed files and
  one commit hash. The ledger and a future SBOM (vcpkg writes an SPDX document per port) can be
  derived from the installed tree.
- A fresh clone builds with one command once the prerequisites exist. Repeat configures restore
  from the binary cache instead of rebuilding.
- The first dependency build on a machine is slow, and the first Qt build takes hours.
- Release-only dependencies mean there is no Debug-CRT configuration: developer builds are
  RelWithDebInfo, the MSVC debug heap and iterator checks are unavailable, and stepping into
  optimised third-party code is less convenient (vcpkg installs PDBs, so symbols are available).
  A debug triplet can be added later if a defect requires it.
- Updating MSVC or moving the baseline invalidates the whole binary cache.
- Builds must run inside the environment set up by `tools/dev-env.ps1`. IDEs that bring their own
  vcpkg or environment must be pointed at the pinned checkout (`docs/development/setup.md`).
- With `LongPathsEnabled=0`, vcpkg roots and build directories must stay short.
- Every third-party library is a DLL that must be deployed next to Lucent's executables. Build
  outputs are gathered in `<build dir>\bin` for that reason.
- Revisit this decision when continuous integration is added (it will need a shared binary cache),
  when the toolchain moves to C++23, when a second platform is added, or when Qt is upgraded.

## Alternatives considered

- **Conan 2.** A capable package manager, but it would need its own profiles and recipes for Qt
  and LibRaw on MSVC. vcpkg's manifest mode, integration with MSVC and CMake, and Qt ports cover
  Lucent's needs with less machinery. Not chosen.
- **Git submodules or CMake `FetchContent`.** Dependencies would rebuild in every build tree, their
  warnings would mix with Lucent's, there would be no binary cache, and producing replaceable LGPL
  DLLs would be manual work. Rejected.
- **Manually installed prebuilt SDKs.** Not reproducible or auditable. Rejected.
- **vcpkg classic mode.** Global, per-machine state that is not declared by the repository.
  Rejected.
- **The vcpkg copy bundled with Visual Studio.** It follows Visual Studio updates rather than the
  project baseline, and its helper scripts produced different ABI hashes (observed), which defeated
  the binary cache. Rejected.
- **The default `x64-windows` triplet.** It builds debug and release (about twice the time) and
  would need a separate host triplet for Qt tools. Rejected for build time. A debug triplet remains
  possible later.
- **A static triplet (`x64-windows-static-md`).** Static linking of LGPL libraries requires
  distributing relinkable object files. Rejected (ADR 0003).
- **Prebuilt Qt via aqtinstall.** Could not be verified with SHA-256 (see Context). Rejected on
  security grounds.
- **Qt Online Installer.** Official and verified, but it requires a Qt account and interactive
  installation, and it is neither pinned nor automatable. Developers may use it privately, but it
  is not the supported path and the presets do not target it.
- **Visual Studio (MSBuild) generator.** A multi-configuration generator is slower for incremental
  builds and does not produce `compile_commands.json` for tooling. Rejected in favour of Ninja.

## Amendment: no build-machine paths in the binaries (W12, 2026-10-09)

### Context

ADR 0009 found that the DLLs vcpkg builds carry the builder's paths and left the fix to W12. A scan
of the installation folder of the W12 performance baseline (`build\w12-perf\package\stage`, 308
files, 66 MB; ADR 0009, decision 12 describes the scan) found 65 places in 63 files, all naming the
vcpkg buildtrees folder below the builder's user profile (`C:\Users\<user>\dev\vb\...`):

- 63 in the CodeView record of the PE debug directory of every DLL vcpkg built (every Qt module,
  plugin and QML plugin; zlib, bzip2, Brotli, double-conversion, FreeType, HarfBuzz, libjpeg-turbo,
  Little CMS, libpng, md4c, PCRE2, LibRaw, SQLite, libtiff): the full path of the DLL's PDB, because
  vcpkg's toolchain (`scripts/toolchains/windows.cmake`) links release binaries with `/DEBUG`;
- 2 `__FILE__` strings: libtiff's `tif_dir.c` puts `__FILE__` into error messages, and Qt Quick's
  `qquickdesignersupport.cpp` into debug output. Neither goes away with `NDEBUG`.

`Lucent.exe` and `lucent-worker.exe` named nothing: the release preset links without `/DEBUG`, and
Lucent's code uses neither `__FILE__` nor `std::source_location` (tests get the source folder as the
string macro `LUCENT_SOURCE_DIR`, which no shipped program uses).

What MSVC offers (19.44.35207, Visual Studio 2022 17.14; checked 2026-10-09 against the compiler
and linker references and by compiling test programs):

- `/PDBALTPATH:%_PDB%`, a documented linker option, writes only the PDB's file name into the debug
  directory. The linker expands `%_PDB%` itself. Without `/DEBUG` it is accepted silently and does
  nothing.
- MSVC documents no option that maps or trims source paths. `/FC` only decides whether a source
  named by a relative path appears in full; CMake names every source by its full path anyway.
- Two undocumented compiler options exist. `/d1trimfile:<prefix>` removes the prefix from
  `__FILE__`, `__builtin_FILE()` and `std::source_location::file_name()`, in sources and headers,
  in C and C++, ignoring letter case and the separator style; several may be given, and the first
  that matches is used; diagnostics, `/showIncludes` (which Ninja reads for dependencies), the
  `#line` directives of `/P` and the debug information keep the full paths, and the option causes
  no warning. `/pathmap:<from>=<to>` is ignored with warning D9007 unless
  `/experimental:deterministic` is also given, which implies `/Brepro` and warns about
  `__DATE__` and `__TIME__`, a risk for ports built with `/WX`.
- `%_PDB%` reaches the linker unchanged through CMake, Ninja and `cmd.exe /C` (which expands only
  defined variables, and nothing defines `_PDB`), and through meson, whose machine files are read
  without interpolation (`mesonbuild/machinefile.py`).
- vcpkg installs each port's PDBs with `vcpkg_copy_pdbs`, which asks `dumpbin /PDBPATH` where a
  DLL's PDB is: with only a file name recorded, dumpbin finds nothing, every port's log warns
  "Could not find a matching pdb file", and no PDB is installed (the previous build installed 250,
  2.0 GB: every Qt module, plugin and tool, and the other ports' DLLs). `dumpbin /PDBPATH` also
  searches `_NT_SYMBOL_PATH`, and reports a PDB only when it matches the binary.

### Decision

11. **No build-machine path in a shipped binary.** The triplet (`cmake/triplets/x64-windows-lucent.cmake`)
    sets `VCPKG_LINKER_FLAGS` to `/PDBALTPATH:%_PDB%`, and `VCPKG_C_FLAGS` and `VCPKG_CXX_FLAGS` to
    `/d1trimfile:<folder>/` for the buildtrees folder, the installed tree and the vcpkg root, taken
    from the variables `scripts/ports.cmake` defines before it includes the triplet (when vcpkg only
    reads triplet settings they are absent, and so are the options; a folder containing a space is
    skipped, because the flags are one space-separated string). Lucent's own release builds get the
    same two options (`cmake/LucentCompilerOptions.cmake`: Release only, C and C++ only, for the
    vcpkg installed tree, the build tree and the source tree, most specific first), project-wide
    rather than through `Lucent::compiler_options`, because the helper targets Qt generates for
    `Lucent.exe` (QML cache and resource object libraries) do not link it. Developer builds
    (RelWithDebInfo) keep full paths for debuggers and test failure messages.

    The PDBs stay installed for developers (the consequence "vcpkg installs PDBs, so symbols are
    available" above): the triplet's documented `VCPKG_POST_PORTFILE_INCLUDES` adds
    `cmake/triplets/post-portfile/install-pdbs.cmake` after every portfile. It puts the folders of
    the port's release build tree that hold PDBs on `_NT_SYMBOL_PATH`, asks `dumpbin /PDBPATH` for
    every DLL and EXE of the package, and copies the matching PDB it names beside the binary. The
    portfiles' own `vcpkg_copy_pdbs` warnings remain in the logs and are expected.

    An undocumented option is used because no documented one exists, and only behind a check that
    does not trust it: the packaging audit (ADR 0009, decision 12) scans every shipped byte for the
    build machine's folders and fails on any hit, so a compiler that stopped honouring
    `/d1trimfile` would stop the packaging, never ship a path. `/pathmap` with
    `/experimental:deterministic` was not chosen: it changes more than paths.

    Changing the triplet changes the ABI hash of every port: all 31 packages were rebuilt once
    from source, in 1.2 hours on the reference machine with `VCPKG_MAX_CONCURRENCY=6`
    (qtdeclarative 38 minutes, qtbase 16, HarfBuzz 3.2, every other port under 2.5), during the
    first configure of a `dev` build tree; later build trees restore the packages from the binary
    cache.

    Result (2026-10-09): of the 274 DLLs and EXEs vcpkg installed, only Qt's build tool
    `tools\Qt6\bin\syncqt.exe` still names its PDB's full path (qtbase builds it in its configure
    step, outside the toolchain's linker flags; it is never shipped); `tiff.dll` now says
    `tiff\src\v4.7.2-910d586e47.clean\libtiff\tif_dir.c`, and the installed tree holds 259 PDBs
    (2.0 GB): the 250 of the previous build, none lost, and those of Little CMS, md4c and the Brotli
    and bzip2 tools, which `vcpkg_copy_pdbs` had missed. The package's scan is in ADR 0009's
    amendment.

### Consequences

- `__FILE__` in the ports' code is now relative to the buildtrees folder
  (`tiff/src/v4.7.2-910d586e47.clean/libtiff/tif_dir.c`), and each DLL names only its PDB's file
  name. Debuggers find a port's PDB next to its DLL (vcpkg installs them side by side) or through a
  symbol path. The PDBs themselves still hold full paths; they are never shipped (ADR 0009).
- Every binary cache entry from before the change is unused, and the first configure on a machine
  rebuilds Qt from source.
- vcpkg hashes the bytes of the triplet file and of its post-portfile step, line endings
  included. On the reference machine (`core.autocrlf=true`, `text=auto`) checkouts write them with
  CRLF, and the rebuilt cache is keyed by that form; an editor or tool that saves either with LF
  endings changes every ABI hash and rebuilds every port (observed during this change: the
  rebuild was restarted with the CRLF files). Pinning `cmake/triplets/**` to one line ending in
  `.gitattributes` would remove the trap.
- The vcpkg folders must not contain spaces (vcpkg advises against them anyway); the audit would
  catch the paths that a skipped folder leaves.
- Revisit when MSVC documents a source-path mapping, when vcpkg's toolchain adds these options
  itself, or with clang-cl (`/pathmap` without `/experimental:deterministic`).
