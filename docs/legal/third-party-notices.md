# Third-party components: development licence ledger

> **Development ledger, not a release notice bundle.** This file tracks which third-party
> components the Lucent source tree builds with or depends on, under which licence, and what each
> licence will require when Lucent is distributed. It does not reproduce the licence texts and does
> not satisfy any notice obligation by itself. The shipped notice bundle and SBOM are generated
> from the files a release actually deploys (roadmap W5 and W12; ADR 0003).

- **Lucent's own code:** MIT (`LICENSE` at the repository root).
- **Policy:** [ADR 0003: linking model and third-party licensing](../adr/0003-linking-and-licensing.md).
  In short, every third-party library is a DLL; LGPL libraries (LibRaw, Qt) are shipped
  unmodified and replaceable; LibRaw is used under LGPL-2.1; GPL applications are visual
  references only.
- **Dependency pinning:** vcpkg registry baseline `e143402b5545bab137cc465225a82da63cb2148f`,
  triplet `x64-windows-lucent` (shared, dynamic CRT, release-only). See
  [ADR 0002](../adr/0002-build-and-dependency-management.md).

## How this ledger was verified

Verified on 2026-09-24 against a `dev-core` configure at the pinned baseline:

- **Installed ports and versions** come from vcpkg's install plan and
  `vcpkg_installed\vcpkg\info\*.list`.
- **Licences** were read from each port's installed copyright file,
  `<build dir>\vcpkg_installed\x64-windows-lucent\share\<port>\copyright`, and cross-checked with
  the `license` field of the port manifest in the pinned vcpkg checkout.
- **Runtime imports** were read with `dumpbin /dependents` on the installed DLLs.
- **`ui` feature (Qt) rows** are marked *planned*. Their versions come from the install plan of a
  `tests;ui` configure at the same baseline. The copyright files of the non-Qt dependencies were
  read from that build's installed tree. The Qt ports themselves were still building, so their
  rows are not yet verified against installed copyright files.

To refresh the ledger after changing `vcpkg.json`, the baseline or the triplet, repeat the steps
above and update this file in the same change (ADR 0003, decision 7).

**Usage key:**

- **shipped DLL**: planned to be deployed with the application.
- **compiled in**: header-only code that ends up inside Lucent binaries.
- **build-only**: needed to build, not deployed.
- **test-only**: used by test executables, never packaged.
- **dev-only**: a developer tool.
- **planned**: arrives with a later feature and is not in `dev-core` builds.

## Engine dependencies (vcpkg default features)

| Component (port) | Version | Licence | Usage | Main obligations when distributed | Licence text source |
|---|---|---|---|---|---|
| LibRaw (`libraw[core,dng-lossy]`) | 0.22.2 | LGPL-2.1-only **or** CDDL-1.0; Lucent elects **LGPL-2.1**. Also carries BSD-3-Clause-style (DCB, FBDD), BSD-style (X3F) and MIT (Adobe DNG SDK parts) notices | Shipped DLL, linked by `lucent_raw`. The port installs `raw_r.dll` (thread-safe) and `raw.dll` (manual-link); only the variant Lucent links ships | See [LibRaw](#libraw) | `share/libraw/copyright` (COPYRIGHT, LICENSE.LGPL, LICENSE.CDDL) |
| JasPer (`jasper`) | 4.2.9 | JasPer License Version 2.0 (MIT-style; the port manifest's `license` field is empty) | Build-only today: required by the LibRaw port, but the installed LibRaw DLLs do not import `jasper.dll`. Ships only if a future LibRaw build imports it | Reproduce the copyright and permission notice and the disclaimer; no endorsement using the holders' names | `share/jasper/copyright` |
| libjpeg-turbo (`libjpeg-turbo[core,jpeg8]`) | 3.2.0, port 1 | IJG AND BSD-3-Clause (bundled Zlib, libpng-2.0 and BSD-2-Clause parts are subsumed, per its LICENSE.md) | Shipped DLL: `jpeg8.dll` is imported by LibRaw (lossy DNG); JPEG export is planned for W2. `turbojpeg.dll` ships only if Lucent uses the TurboJPEG API | Product documentation must state "This software is based in part on the work of the Independent JPEG Group." If the TurboJPEG API is used, include the BSD-3-Clause text. No use of IJG or libjpeg-turbo names in advertising | `share/libjpeg-turbo/copyright` (LICENSE.md only; see [gaps](#known-gaps-to-close-before-a-release)) |
| libspng (`libspng`) | 0.7.4, port 1 | BSD-2-Clause AND libpng-2.0 | Build-only today: a libjpeg-turbo dependency imported only by `turbojpeg.dll`. Ships with `turbojpeg.dll` if that DLL ships | Reproduce the BSD-2-Clause notice and the libpng-2.0 notice for code derived from libpng | `share/libspng/copyright` (LICENSE plus `spng.c`, whose header carries the libpng notice) |
| Little CMS (`lcms`) | 2.19.1, port 1 | MIT | Shipped DLL `lcms2-2.dll`: imported by LibRaw; direct use planned in `engine`/`export` (W2) | Reproduce the MIT notice. **Never enable** the `fastfloat` or `threaded` features (GPL-3.0-or-later) | `share/lcms/copyright` |
| libtiff (`tiff[core,zip]`, default features off) | 4.7.2 | libtiff licence, plus the Regents of the University of California notice for `tif_lzw.c` | Shipped DLL `tiff.dll`, planned for 16-bit TIFF export (W2) | Include the copyright and permission notices in the software and related documentation; no use of Sam Leffler, SGI or UC names in advertising; materials must acknowledge that the LZW code "was developed by the University of California, Berkeley" | `share/tiff/copyright` |
| SQLite (`sqlite3[core,json1]`) | 3.53.4, port 1 | Public domain (vcpkg SPDX id `blessing`) | Shipped DLL `sqlite3.dll`, planned for the catalog (W4) | None. Attribution is customary | `share/sqlite3/copyright` (public-domain statement; upstream https://www.sqlite.org/copyright.html) |
| zlib (`zlib`) | 1.3.2, port 2 | Zlib | Shipped DLL `z.dll`: imported by LibRaw, libtiff and libspng | Do not misrepresent the origin; mark altered sources; keep the notice in source distributions | `share/zlib/copyright` |
| nlohmann-json (`nlohmann-json`) | 3.12.0, port 2 | MIT | Compiled in (header-only), for JSON output, recipes and manifests | Reproduce the MIT notice | `share/nlohmann-json/copyright` |
| tl-expected (`tl-expected`) | 1.3.1 | CC0-1.0 | Compiled in (header-only); part of the public error contract (ADR 0004) | None (public-domain dedication) | `share/tl-expected/copyright` |

### LibRaw

Lucent uses LibRaw under **LGPL-2.1** (ADR 0003, decision 2). The installed LibRaw DLLs import
`jpeg8.dll`, `lcms2-2.dll` and `z.dll`. A distribution must:

- ship LibRaw as a separate DLL that the user can replace with a compatible modified version;
- include the LGPL-2.1 text and LibRaw's COPYRIGHT notices, which cover dcraw (Dave Coffin), DCB
  and FBDD (Jacek Gozdz, BSD-style 3-clause), the X3F tools (Roland Karlsson, BSD-style) and the
  Adobe DNG SDK 1.4 code (MIT);
- provide the corresponding source: LibRaw 0.22.2 (tag `0.22.2`, pinned by SHA-512 in the port),
  the LibRaw-cmake build files (commit `eb98e4325aef2ce85d2eb031c2ff18640ca616d3`) and the vcpkg
  port patches (`dependencies.patch`, `fix-install.patch`, and the `LIBRAW_NODLL` edit to
  `libraw_types.h`). Lucent itself does not modify LibRaw;
- not forbid, in Lucent's end-user terms, modification of the library for the user's own use or
  reverse engineering for debugging such modifications.

The port's copyright file lists no GPL demosaic packs. Enabling the `openmp` feature does not
change the licence, but it does change the build and would be recorded here.

## Test dependencies (vcpkg `tests` feature)

| Component (port) | Version | Licence | Usage | Main obligations | Licence text source |
|---|---|---|---|---|---|
| GoogleTest and GoogleMock (`gtest`) | 1.18.0 | BSD-3-Clause | Test-only: `gtest*.dll` and `gmock*.dll` sit beside the test executables in `<build dir>\bin` and are never packaged | None while undistributed | `share/gtest/copyright` |

## Build helper ports (host, never shipped)

| Component (port) | Version | Licence | Usage | Notes |
|---|---|---|---|---|
| `vcpkg-cmake` | 2025-08-07 | MIT (vcpkg helper scripts) | Build-only | Microsoft copyright |
| `vcpkg-cmake-config` | 2026-07-21 | MIT | Build-only | |
| `vcpkg-cmake-get-vars` | 2025-05-29 | MIT | Build-only | |
| `vcpkg-tool-meson` | 1.9.0, port 10 | Port scripts MIT; Meson itself is Apache-2.0 upstream | Build-only (Little CMS builds with Meson) | The installed copyright file covers only vcpkg's port scripts |

## Application dependencies (vcpkg `ui` feature, planned)

Not part of `dev-core` builds. These rows arrive with the Qt Quick application (W3) and must be
re-verified against the installed copyright files when the `dev` preset first builds.

| Component (port) | Version | Licence | Usage | Main obligations | Status |
|---|---|---|---|---|---|
| Qt Base (`qtbase`, default features off; features `concurrent`, `doubleconversion`, `freetype`, `gui`, `harfbuzz`, `jpeg`, `network`, `opengl`, `pcre2`, `png`, `testlib`, `thread`, `windeployqt`) | 6.11.1, port 3 | Lucent uses **LGPL-3.0-only**; Qt also offers GPL and commercial terms. The port manifest's `license` field is empty | Planned shipped DLLs (Qt Core, Gui, Network, Concurrent) and plugins; Qt Test is test-only; `windeployqt` is a deployment tool | LGPL-3.0 obligations as for LibRaw (replaceable DLLs, licence text, corresponding source including the vcpkg `qtbase` patches, and end-user terms that permit modification and reverse engineering for debugging) | Not verified: Qt is still building |
| Qt Declarative (`qtdeclarative`) | 6.11.1 | LGPL-3.0-only (as above) | Planned shipped DLLs (Qt QML, Qt Quick) and QML imports | As above | Not verified |
| Qt SVG (`qtsvg`) | 6.11.1 | LGPL-3.0-only (as above) | Planned shipped DLL and image plugin | As above | Not verified |
| Qt Shader Tools (`qtshadertools`) | 6.11.1 | Qt tools: GPL-3.0 with the Qt GPL Exception 1.0, or commercial (https://doc.qt.io/qt-6/licensing.html) | Build-only: `qsb` compiles shaders at build time. The runtime library is not planned to ship | None while not shipped | Not verified |
| Qt Language Server (`qtlanguageserver`) | 6.11.1 | As Qt tools | Build-only (a dependency of `qtdeclarative`'s tooling) | None while not shipped | Not verified |
| double-conversion | 3.4.0 | BSD-3-Clause (V8 project authors) | Planned shipped DLL (Qt Core) | Reproduce the notice and disclaimer in documentation | Copyright file read |
| PCRE2 (`pcre2[core,jit,platform-default-features]`) | 10.47, port 1 | BSD-3-Clause (port manifest) | Planned shipped DLL (Qt Core regular expressions) | Reproduce the upstream LICENCE notices | The installed copyright file only points to upstream LICENCE (see [gaps](#known-gaps-to-close-before-a-release)) |
| HarfBuzz (`harfbuzz[core,freetype]`) | 14.5.0 | MIT-Modern-Variant AND MIT ("Old MIT") | Planned shipped DLL (text shaping) | Reproduce the notices | Copyright file read |
| FreeType (`freetype[brotli,bzip2,core,png,zlib]`) | 2.14.3 | FTL **or** GPL-2.0-or-later; Lucent elects **FTL** | Planned shipped DLL (font rasterisation) | Credit in product documentation, e.g. "Portions of this software are copyright (c) <year> The FreeType Project (https://freetype.org). All rights reserved." | Copyright file read |
| libpng (`libpng`) | 1.6.58 | libpng-2.0 | Planned shipped DLL (FreeType, Qt PNG) | Keep the notice in source distributions | Copyright file read |
| Brotli (`brotli`) | 1.2.0 | MIT | Planned shipped DLL (FreeType WOFF2) | Reproduce the MIT notice | Copyright file read |
| bzip2 (`bzip2[core,tool]`) | 1.0.8, port 6 | bzip2-1.0.6 (BSD-style) | Planned shipped DLL (FreeType); the `tool` executable is not shipped | Reproduce the notice and disclaimer in documentation | Copyright file read |
| MD4C (`md4c`) | 0.6.0 | MIT | Planned shipped DLL (Qt Markdown support) | Reproduce the MIT notice | Copyright file read |
| EGL Registry (`egl-registry`) | 2025-05-27 | Apache-2.0 and Khronos MIT-style (per file) | Build-only headers | None while not shipped | Copyright file read |
| OpenGL Registry (`opengl-registry`) | 2026-08-03 | Per-file licences (each header states its own) | Build-only headers | None while not shipped | Copyright file read |
| OpenGL (`opengl`) | 2022-12-04, port 3 | Windows SDK licence | Build-only: links the OS `opengl32.dll`, which is part of Windows | None | Copyright file read |

## Runtime prerequisite (not a vcpkg port)

| Component | Version | Licence | Usage | Obligations |
|---|---|---|---|---|
| Microsoft Visual C++ Redistributable (x64, 14.x) | Matches MSVC 14.44 or newer | Microsoft Software License Terms and redistribution guidance | Required at runtime by the dynamic CRT (`/MD`) that Lucent and every vcpkg DLL use. For example, the LibRaw DLLs import `MSVCP140.dll`, `VCRUNTIME140.dll` and `VCRUNTIME140_1.dll` | Install through Microsoft's official redistributable installer from the Lucent installer (blueprint section 9, W5). Never copy CRT DLLs from a developer machine |

## Development-only tools and references (never shipped)

| Component | Version on the reference machine | Licence | Usage |
|---|---|---|---|
| Visual Studio 2022 Build Tools (MSVC, Windows SDK 10.0.26100) | 17.14.41, MSVC 14.44.35207 | Microsoft Software License Terms for Visual Studio 2022 Build Tools and the Windows SDK. Each developer must confirm their use is covered; the terms were not reviewed for this ledger | Compiler, linker, SDK |
| CMake (pip package in the tools venv) | 4.4.3 | BSD-3-Clause | Build system |
| Ninja | 1.13.2 (winget); Visual Studio's bundled 1.12.1 is what builds use today (see setup) | Apache-2.0 | Build tool |
| vcpkg tool and registry | tool 2026-07-27, registry `e143402b` | MIT | Dependency manager |
| Python | 3.12.10 | PSF-2.0 | Hosts the pinned CMake and the research environment |
| Git for Windows | 2.55.0 | GPL-2.0 | Version control; not linked |
| Research environment (Python packages such as rawpy, which bundles LibRaw) | See `research/README.md` | Per package (rawpy: MIT wrapper; the bundled LibRaw keeps its LGPL-2.1/CDDL-1.0 terms) | Numerical references and experiments; never a runtime dependency |
| Camera test fixtures | `fixtures/manifest.json` (fetched by `tools/fetch-fixtures.ps1`) | Per fixture, recorded in the manifest. The W1 set comes from raw.pixls.us and each file is marked CC0-1.0 | Downloaded on demand into `fixtures/cache/`, never committed or shipped |
| RawTherapee, darktable, ART | Any | GPL-family (blueprint Appendix C) | Visual comparison references only: run as separate programs, never linked, bundled, invoked by Lucent or used as a source of code (ADR 0003, decision 6) |

## Known gaps to close before a release

1. **libjpeg-turbo:** the installed copyright file contains only `LICENSE.md`. The IJG licence text
   it refers to (`README.ijg`) is not installed. Take it from the pinned upstream source for the
   notice bundle.
2. **PCRE2:** the installed copyright file only points to the `LICENCE` file of the PCRE2
   distribution. Take the full text, including the JIT compiler's terms, from the pinned upstream
   source.
3. **Qt ports:** not yet installed at the time of writing. Verify the licences and the exact module
   set against the installed copyright files and the `windeployqt` output (W3/W5).
4. **Deployed set:** usages marked "planned" and the choice between `raw_r.dll` and `raw.dll`,
   `jpeg8.dll` and `turbojpeg.dll` must be confirmed from the files the deployment step actually
   ships.
5. **Corresponding source for LGPL components:** decide how it is provided (bundled archive or
   written offer) before the first public release (W12).
