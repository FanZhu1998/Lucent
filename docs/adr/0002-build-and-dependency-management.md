# ADR 0002: Build system and dependency management

- **Status:** Accepted
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
