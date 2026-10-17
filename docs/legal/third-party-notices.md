# Third-party components: licence ledger

> **Ledger, not a release notice bundle.** This file tracks which third-party components the
> Lucent source tree builds with or depends on, under which licence, what each licence requires when
> Lucent is distributed, and how a release meets it ([Release status](#release-status)). It does not
> reproduce the licence texts and does not satisfy any notice obligation by itself. The shipped
> notice bundle and SBOM are generated from the files a release actually deploys by
> `tools\package.ps1` into the installation's `licenses\` folder (W5,
> [ADR 0009](../adr/0009-windows-packaging.md), decision 10), and the corresponding source of the LGPL
> libraries into `Lucent-<version>-corresponding-source.zip` beside the installer (W12,
> [ADR 0024](../adr/0024-release-and-licensing.md)).

- **Lucent's own code:** MIT (`LICENSE` at the repository root).
- **Lucent's logo** (`assets/branding`, built into the programs and Setup): not covered by the MIT
  License. Copyright (c) 2026 Fan Zhu, all rights reserved; others may show it, unmodified, only to
  refer to Lucent (the owner's decision of 2026-10-15, `LicenseRef-Lucent-Logo`;
  [the asset's README](../../assets/branding/README.md#licence), [ADR 0025](../adr/0025-branding.md)).
- **Policy:** [ADR 0003: linking model and third-party licensing](../adr/0003-linking-and-licensing.md).
  In short, every third-party library is a DLL; LGPL libraries (LibRaw, Qt, and since W13 Lensfun
  with GLib, libintl and libiconv) are shipped as replaceable DLLs. Their vcpkg recipes patch the
  upstream source of several of them (W12 review LIC-1): vcpkg's own patches (for example GLib's
  `use-libiconv-on-windows.patch`, which makes `glib/gconvert.c` convert with GNU libiconv, libintl's
  `0003-Fix-win-unicode-paths.patch` in `intl/loadmsgcat.c`, Qt's `md4c.diff`) and, in the overlay
  ports of GLib and libintl, one Lucent patch each that keeps the build machine's folders out of the
  DLL (the overlay recipes ship in `licenses\<port>\recipe`, with vcpkg's licence beside them,
  `licenses\vcpkg-LICENSE.txt`). `LGPL-NOTICE.txt` and the
  corresponding-source archive list every patch with the files it changes and its date ("Modified").
  LibRaw is used under LGPL-2.1; GPL applications are visual references only. Third-party data (the
  Lensfun lens database, CC BY-SA 3.0) ships unmodified as a verified asset with its licence text
  ([ADR 0023](../adr/0023-lens-profile-corrections.md)).
- **Dependency pinning:** vcpkg registry baseline `e143402b5545bab137cc465225a82da63cb2148f`,
  triplet `x64-windows-lucent` (shared, dynamic CRT, release-only). See
  [ADR 0002](../adr/0002-build-and-dependency-management.md).

## Distribution decision (2026-10-10)

The user decided, before the first public release:

- **Lucent is free and open-source software under its MIT licence and is not sold.** Lucent's
  terms (the MIT License) do not restrict modifying the LGPL libraries or reverse engineering Lucent
  to debug such modifications.
- **Corresponding source:** every installer is published with
  `Lucent-<version>-corresponding-source.zip`, from the same place and for as long as the installer
  is offered (LGPL-2.1 section 4; for the LGPL-3.0 libraries GPL-3.0 section 6(d), which LGPL-3.0
  incorporates). It holds, for each shipped LGPL library, the upstream source archives (SHA-512 as
  vcpkg verified them and as the recipe pins them at the registry baseline), the vcpkg recipe as built
  (registry port or Lucent's overlay port, every file as vcpkg hashed it), vcpkg's build record, the
  triplet, Lucent's vcpkg manifest and a README on rebuilding each library and replacing its DLL.
  `licenses\LGPL-NOTICE.txt` names the archive with its size and SHA-256. Packaging fails when a
  shipped component's licence leaves no choice but a copyleft licence and
  `installer\license-policy.json` provides no source for it (a `LicenseRef-` alternative, such as a
  vendor's commercial licence, is no choice Lucent has), when a provided library is missing from the
  archive, and, whatever the policy provides, for GPL or AGPL code without an exception (unless an ADR
  accepted it) and for copyleft code compiled into the programs (W12 review LIC-4).
- **Inno Setup** (Setup and its uninstaller) is used under its non-commercial terms, which cover a
  free distribution. The compiler on the reference machine (6.7.3) reports "Non-commercial use only";
  packaging records that line in the package manifest, and if the policy ever says
  `commercial: true` it refuses any compiler whose banner does not state a commercial licence (a
  non-commercial one, or a banner it does not recognise; W12 review LIC-6). `THIRD-PARTY-NOTICES.txt`
  credits Inno Setup, whose own notices stay in its binaries.
- **Releases are unsigned** until a code-signing certificate exists (the signing pipeline stays
  ready, ADR 0009 decision 13). Unsigned packages are not offered on an update channel.

The decision is recorded in `installer\license-policy.json` ("distribution") and in every package
manifest (schema 5, "distribution").

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

Re-verified on 2026-10-03 (W5) against the installation folder of the 0.1.0 package built from the
`release` preset at the same baseline:

- **Shipped set** from the packaging audit (`tools\package.ps1`, `audit.txt`): every file of the
  installation folder mapped to its vcpkg port through `vcpkg_installed\vcpkg\info\*.list` and
  compared byte for byte with the vcpkg installation; every DLL import resolved with
  `dumpbin /dependents`.
- **Qt ports** checked against their installed copyright files, which concatenate Qt's `LICENSES`
  texts (including `LGPL-3.0-only.txt` and `GPL-3.0-only.txt`); the vcpkg port manifests declare no
  licence.
- **Licence elections and credit lines** that packaging applies are recorded, per reviewed port
  version, in `installer\license-policy.json`; packaging stops when another version ships.

Re-verified on 2026-10-10 (W12 licence and release phase) against the installation folder of the
0.1.0 `-StageOnly` package of the `release` preset (`build\rel-lic`, commit `f2faf0e` with the
phase's changes): 23 shipped components (21 with DLLs, 2 compiled in), 8 of them LGPL, the two data
assets, and the corresponding-source archive written and read back for the 8 LGPL libraries (the
[Release status](#release-status) table).

Re-verified on 2026-10-15 (W12 release review) against the `-StageOnly` package of the `release`
preset (`build\wl-fix-scripts-rel`, commit `334d0bb` with the review's changes): the same 23
components and 8 LGPL libraries; `LGPL-NOTICE.txt` and the archive list every patch of each LGPL
library's recipe with the files it changes and its date ("modifications listed" in the table below),
and the archive's README names `Qt6EntryPoint.lib` as the only library code linked into a program
(`Lucent.exe`).

Re-verified on 2026-10-15, after the owner's decision on the logo's terms, against the `-StageOnly`
package of the `release` preset (`build\lic-logo`, commit `973c11d` with the change recording them,
uncommitted): the same 23 components, 8 LGPL libraries and two data assets; `THIRD-PARTY-NOTICES.txt`
states the logo's terms right after Lucent's own notice, and the SBOM gives `Lucent.exe`, `lucent-worker.exe` and Lucent's
package `MIT AND LicenseRef-Lucent-Logo`, with the terms as an extracted licensing info, and passed
`pyspdxtools` (spdx-tools 0.8.5) (the row "Lucent's logo" below).

Re-verified on 2026-10-16 (the release checklist's gate 8 read on the 0.1.0 package found eleven
discrepancies, all fixed: [ADR 0024](../adr/0024-release-and-licensing.md), amendment of 2026-10-16)
against the `-StageOnly` package of a fresh `release` build (`build\rel-0.1.0-lic`, commit `972f349`
with the fixes uncommitted): LibRaw's parts and their licences were read from its source archive
(`LibRaw-LibRaw-0.22.2.tar.gz`, SHA-512 as the port pins it at the baseline), and LibRaw is
`LGPL-2.1-only AND BSD-3-Clause AND MIT` with its parts' terms in `licenses\libraw`; vcpkg's licence
ships beside its recipes (the installation and the archive); the SBOM concludes a licence for the 14
recipe files, `qt.conf` and `Lucent-files.txt`; `THIRD-PARTY-NOTICES.txt` names the Visual C++
redistributable's and Inno Setup's versions and the camera support database; and the rows of Qt
Base, LibRaw, GNU libiconv, Lucent and nlohmann-json below were corrected.

Re-verified on 2026-10-16 (the gate-8 review, round 1, of that reading found fourteen more
discrepancies, R1 to R14, and asked for a sweep of every shipped component, which found more parts
of the same kind; all fixed: [ADR 0024](../adr/0024-release-and-licensing.md), amendment "the gate-8
review, round 1") against the `-StageOnly` package of the same build (`build\rel-0.1.0-lic`, commit
`972f349` with the fixes uncommitted). For each of the 23 shipped components, the source archive the
port pins (SHA-512 checked against the port at the baseline and vcpkg's download) was searched for
files under licences or notices of their own (copyright lines, licence statements, SPDX headers, the
licence files a component's own licence points to), and each such file was checked against what this
build compiles: vcpkg's build logs of the ports (their ABI hashes are this build's), the configure
summaries and the shipped DLLs. Qt's third-party code was read from Qt's own SBOMs and checked
against Qt's configure summary. The C++ standard library code compiled into the two programs was read
in the MSVC toolset that built them (14.44.35207) and its tables found in the binaries; Inno Setup's
licence file was read in the compiler that made Setup. The result is the section
[Code under licences of its own inside the shipped files](#code-under-licences-of-its-own-inside-the-shipped-files)
and the corrected rows below; the notices, the SBOM and `installer\license-policy.json` state the
same, and `installer\tests\Test-SourcePackage.ps1 -PackageManifest` checks a package for it.

Re-verified on 2026-10-16 (the gate-8 review, round 2, of round 1's fixes found twenty-one more
discrepancies, F1 to F21; each is fixed at its source or, where it needs the owner, recorded for gate
8: [ADR 0024](../adr/0024-release-and-licensing.md), amendment "the gate-8 review, round 2") against
the `-StageOnly` package of the same build (`build\rel-0.1.0-lic`, commit `972f349` with the fixes
uncommitted). Every text the round added was taken from a verified source, never written from
memory: the source archives in vcpkg's downloads folder (SHA-512 as the ports pin them at the
baseline), files of the published sources of Inno Setup 6.7.3 (tag `is-6_7_3`, signed) and of Apache
Tika at the commit Qt's SBOM records, which Lucent keeps in `installer\third-party` and packaging
checks by their git blob ids, the Windows SDK's `cppwinrt\LICENSE.txt` (pinned by SHA-256) and the
licence that the bundled `vc_redist.x64.exe` itself carries. What the shipped binaries contain was
read in them: C++/WinRT's version string in `Qt6Core.dll` and `qwindows.dll`, the strings of the
Inno Setup compiler's loader and setup engine. Each patch's licence is now derived from the files it
changes ([vcpkg port recipes](#release-status)). The corrected rows, sections and items say "(the
gate-8 review, round 2)".

To refresh the ledger after changing `vcpkg.json`, the baseline or the triplet, repeat the steps
above, update `installer\license-policy.json` and this file in the same change (ADR 0003,
decision 7).

## Release status

How the 0.1.0 package (the `-StageOnly` run of 2026-10-10 above; the modifications as the run of
2026-10-15 lists them; the logo's terms as the run for them states them; the licences, notices and
SBOM as the run of the gate-8 review, round 2, of 2026-10-16 writes them) meets each shipped
component's obligations. "Licence as used" is the expression of everything of the component inside
the shipped files: its own code and its parts under licences of their own
([below](#code-under-licences-of-its-own-inside-the-shipped-files)). "Notice" means the
component's copyright file (and any further text named) is in `licenses\<port>\` and the component
is listed in `THIRD-PARTY-NOTICES.txt`; "source" means its
upstream archives and recipe are in `Lucent-0.1.0-corresponding-source.zip` and `LGPL-NOTICE.txt`
names them; "replaceable" means a separate DLL that Lucent loads dynamically and never checks;
"modifications listed" means `LGPL-NOTICE.txt` and the archive name every patch the library's recipe
applies to its upstream source, with the files it changes, who made it and its date (W12 review LIC-1;
until that review the status "Met" below did not cover vcpkg's own patches).

| Component | Version | Licence as used | Ships as | Obligations | Status |
|---|---|---|---|---|---|
| Lucent | 0.1.0 | MIT, except the logo inside the programs (row "Lucent's logo") and the code of others compiled or linked into them (the rows of nlohmann-json, tl-expected, the C++ standard library and Qt Base's entry point) | Lucent's own files: `Lucent.exe`, `lucent-worker.exe`, `qt.conf`, `Lucent-files.txt` (the installed-file list), its data asset `assets\camera-support` (row "Camera support database"), the description files it adds to the lens database (`assets\lens-database\provenance.json` and `manifest.json`), and the notices and SBOM it writes (`licenses\THIRD-PARTY-NOTICES.txt`, `LGPL-NOTICE.txt`, `Lucent.spdx.json`) with its licence `licenses\Lucent-LICENSE.txt`. The other files of `assets\lens-database` and `licenses\` (licence texts, part files, recipes) belong to the rows of their components (until the gate-8 review, round 1, of 2026-10-16 this row named "assets" and "the licence bundle" as Lucent's) | Keep the notice | Met: `licenses\Lucent-LICENSE.txt`; source public at the release tag; the SBOM gives `qt.conf`, `Lucent-files.txt`, the camera support files and the lens database's two description files MIT (NOASSERTION for `qt.conf` and `Lucent-files.txt` until the gate-8 reading of 2026-10-16), and each program its whole licence, `MIT AND LicenseRef-Lucent-Logo` and the licences of the code compiled or linked into it: `Lucent.exe` `MIT AND LicenseRef-Lucent-Logo AND CC0-1.0 AND BSD-3-Clause AND Apache-2.0 WITH LLVM-exception AND BSL-1.0 AND MIT WITH fmt-exception AND Unicode-DFS-2016`, `lucent-worker.exe` the same without the entry point's `BSD-3-Clause` (since the gate-8 review, round 2; until then `MIT AND LicenseRef-Lucent-Logo` alone); the notices and licence texts it writes stay NOASSERTION, since they quote the components' licences. The programs' version resources and Setup's copyright line keep "Copyright (c) 2026 Fan Zhu. MIT License.", as the owner decided on 2026-10-15; the notices and the SBOM state the exceptions (the logo, the code of others) |
| Qt Base (`qtbase`) | 6.11.1 | `LGPL-3.0-only AND Apache-2.0 AND BSD-2-Clause AND BSD-3-Clause AND CC0-1.0 AND FTL AND HPND AND Imlib2 AND LicenseRef-ICC-License AND LicenseRef-Lcs-Telegraphics AND LicenseRef-SHA1-Public-Domain AND MIT AND MIT-Khronos-old AND MPL-2.0 AND OGL-UK-3.0 AND Unicode-3.0 AND X11`: Qt under LGPL-3.0-only, with the licences of the 28 third-party records of Qt's SBOM compiled into its files (FTL elected for the rasterizer from FreeType 2, CC0-1.0 for BLAKE2; [below](#code-under-licences-of-its-own-inside-the-shipped-files)), corrected where Qt's SBOM falls short: Khronos' OpenGL headers are `MIT-Khronos-old` (Qt's `REUSE.toml`; the SBOM says MIT), and Apache Tika's MIME type definitions include PRONOM's under the Open Government Licence 3.0 (Tika's NOTICE); C++/WinRT of the Windows SDK (MIT, row "C++/WinRT") is compiled into `Qt6Core.dll` and `qwindows.dll`; its entry point `Qt6EntryPoint.lib` BSD-3-Clause (until the gate-8 review, round 1, of 2026-10-16 the licence was `LGPL-3.0-only` alone; `MIT-Khronos-old` and `OGL-UK-3.0` since round 2) | 4 DLLs, 4 plugins; the entry point linked statically into `Lucent.exe` | Replaceable, texts (LGPL-3.0, GPL-3.0), source, a dated notice of the modifications (GPL-3.0 section 5(a), which LGPL-3.0 incorporates), notices of the third-party code inside, terms allowing modification and reverse engineering | Met: replaceable (`--no-patchqt`), notice and texts, source, modifications listed (vcpkg's recipe, 26 patches: 18 change only CMake build files, `use-shared-mime-info.patch` only with a feature Lucent does not use; `md4c.diff` changes `qtextmarkdownimporter.cpp` of `Qt6Gui.dll`; `silence-winrtbase-coroutine-warnings.diff` adds a `#define` silencing a deprecation warning to `src/corelib/platform/windows/qt_winrtbase_p.h`, compiled where Qt uses C++/WinRT, as this build does, the only other one that reaches the Windows binaries, and to its configure test (`src/corelib/configure.cmake`; named since the gate-8 review, round 1); `clang-cl_source_location.patch`, `clang-cl_QGADGET_fix.diff` and `QTBUG-145703.patch` change headers with an effect only with clang-cl or in static builds; `use_inotify_on_freebsd.patch` changes a FreeBSD-only condition in `src/corelib/io/qfilesystemwatcher.cpp`; `macdeployqt-symlinks.patch` changes macdeployqt, not built on Windows; `windeployqt-webengine-debug-resources.patch` changes windeployqt, which the build runs but does not ship; the four C++ patches were left out of this summary and of the policy's note until the gate-8 reading of 2026-10-16), Qt's SBOM attributions, each compiled one covered by the licence (packaging checks it since the gate-8 review, round 1), the Vulkan Memory Allocator, which Qt's SBOM records for `Qt6Gui.dll`, listed as not compiled (Vulkan is off); the MPL-2.0 Public Suffix List's Source Code Form named in the notices and the SBOM: `public_suffix_list.dat` of `publicsuffix/list` at commit `944fb533`, the version Qt's SBOM records (since the gate-8 review, round 2; until then this row said its source was inside the qtbase archive, which holds only the array Qt generated from it); the lines of Apache Tika's NOTICE that pertain to the MIME type definitions in `licenses\qtbase\Apache-Tika-NOTICE.txt` (round 2); each patch with the licence of the files it changes (round 2): `macdeployqt-symlinks.patch` and `windeployqt-webengine-debug-resources.patch` `MIT AND GPL-3.0-only WITH Qt-GPL-exception-1.0` (Qt's tools), the others `MIT AND BSD-3-Clause` or `MIT AND LGPL-3.0-only` (Lucent's election among Qt's choices), not the library's licence as before |
| Qt Declarative (`qtdeclarative`) | 6.11.1 | `LGPL-3.0-only AND BSD-2-Clause AND MIT`: Qt QML and Qt Quick under LGPL-3.0-only, the JavaScriptCore macro assembler, YARR and WTF in `Qt6Qml.dll` BSD-2-Clause, Yoga in `Qt6QuickLayouts.dll` MIT (until the gate-8 review, round 1, `LGPL-3.0-only`) | 19 DLLs, 18 QML plugin DLLs and 212 other files: 174 QML source files (`.qml`), 19 module definitions (`qmldir`) and 19 type information files (`.qmltypes`) (until the gate-8 review, round 2, the notices called all 212 module definitions and type information) | As Qt Base | Met: replaceable, notice, source; no patch ("Modified: no"); the third-party code from Qt's SBOM listed and covered by the licence; UDis86 (BSD-2-Clause) not compiled (`WTF_USE_UDIS86=0`), the Material style's shadow code (MIT) not shipped |
| Qt SVG (`qtsvg`) | 6.11.1 | `LGPL-3.0-only AND HPND-sell-variant`: the arc code derived from XSVG in `Qt6Svg.dll` (until the gate-8 review, round 1, `LGPL-3.0-only`) | `Qt6Svg.dll`, 2 plugins | As Qt Base | Met: replaceable, notice, source; no patch; XSVG listed and covered by the licence |
| LibRaw (`libraw`) | 0.22.2 | `LGPL-2.1-only AND BSD-3-Clause AND MIT`: LibRaw under LGPL-2.1-only (elected over CDDL-1.0), its DCB demosaic and FBDD denoise code under BSD-3-Clause and its code from the Adobe DNG SDK 1.4 under MIT, both in `raw_r.dll`; its X3F tools (BSD-3-Clause) are in the source only (see [LibRaw](#libraw)); its Sony YCC and lossless JPEG decoders, "partially backported from DNGLab's rust code", are under LibRaw's licence, which fits the LGPL-2.1 that DNGLab's repository states (the gate-8 review, round 2) | `raw_r.dll` | Replaceable, LGPL-2.1 text, notices, the terms of its parts with the binary (the BSD-3-Clause conditions and disclaimer, the MIT permission notice), source (LibRaw and LibRaw-cmake archives, port patches), changed files marked (LGPL-2.1 section 2) | Met: replaceable, notice, the parts' notices in `licenses\libraw` from the verified source archive (since the gate-8 reading of 2026-10-16, which found the MIT code missing from the licence and the parts' terms missing from the notices); the MIT text of the Adobe code, which LibRaw's archive names without its text, is that of nlohmann-json's licence files, from its verified archive, which the part's file names (since the gate-8 review, round 2; until then the MIT License's standard words, supplied by Lucent); the DNGLab-derived decoders' header in `libraw\DNGLab-derived-decoders.txt` (round 2); source (2 archives), modifications listed: vcpkg's `dependencies.patch` and `fix-install.patch` change LibRaw-cmake's build files, not LibRaw's (its `CMakeLists.txt`, under Gilles Caulier's GPL-2.0-or-later notice, and two `cmake/` files without a notice, which its BSD-3-Clause `LICENSE` covers), so each patch is `MIT AND BSD-3-Clause AND GPL-2.0-or-later`, and the archive's `licenses/GPL-2.0-only.txt` holds the GPL's text, which LibRaw-cmake's archive lacks (since round 2; until then the patches had LibRaw's licence) |
| Lensfun (`lensfun`) | 0.3.4 | `LGPL-3.0-only AND LGPL-2.1-or-later`: Lensfun under LGPL-3.0-only, the subset of the TRE regular expression library in `libs/regex` (Ville Laurikari) under LGPL-2.1-or-later, compiled into `lensfun.dll` as `tre_regex` (until the gate-8 review, round 1, of 2026-10-16 the licence was `LGPL-3.0-only` and TRE unnamed) | `lensfun.dll` | As Qt Base; TRE's notice | Met: replaceable, notice, TRE's notice from the verified archive in `licenses\lensfun\TRE-LGPL-2.1-or-later.txt` (the LGPL-2.1's text is GNU libintl's `copyright.txt`, the LGPL-3.0's Lensfun's own), source, modifications listed (vcpkg's `fix_build.patch`: `CMakeLists.txt` only, a file without a licence notice for which Lensfun's archive states no licence, so the SBOM makes no assertion of the patch's licence, NOASSERTION; the gate-8 review, round 2) |
| GLib (`glib`) | 2.90.0 | LGPL-2.1-or-later | `glib-2.0-0.dll` | Replaceable, text, source of the modified library, changed files marked with the date (LGPL-2.1 section 2) | Met since the W12 release review: modifications listed with their dates in `LGPL-NOTICE.txt` and the archive: vcpkg's `use-libiconv-on-windows.patch` (`glib/gconvert.c`, `meson.build`) and `libintl.patch` (`meson.build`), and Lucent's `lucent-neutral-locale-dir.patch` (`meson.build`, which it also marks); source with the overlay recipe, also in `licenses\glib\recipe`. (Before the review the notice named only Lucent's patch.) |
| GNU libintl (`gettext-libintl`) | 0.22.5 | `LGPL-2.1-or-later AND GPL-3.0-or-later WITH Bison-exception-2.2`: libintl under LGPL-2.1-or-later, GNU Bison's parser skeleton in `intl/plural.c` (generated by Bison 3.8.2, compiled into `intl-8.dll`) under GPL-3.0-or-later with the Bison exception, which lets the larger work keep its own terms (until the gate-8 review, round 1, of 2026-10-16 `LGPL-2.1-or-later`) | `intl-8.dll` | As GLib; the skeleton's notice with the exception | Met since the W12 release review: modifications listed: vcpkg's `uwp.patch` (`gettext-runtime/intl/langprefs.c`, `gettext-runtime/intl/gnulib-lib/localcharset.c`) and `0003-Fix-win-unicode-paths.patch` (`gettext-runtime/intl/loadmsgcat.c`), and Lucent's `lucent-no-builder-paths.patch` (`gettext-runtime/intl/dcigettext.c`, `gettext-runtime/intl/gnulib-lib/relocatable.c`), and since the gate-8 review, round 1, vcpkg's script `bashify.cmake`, which the portfile runs on `gettext-runtime/configure` before configuring; source with the overlay recipe, also in `licenses\gettext-libintl\recipe`; the skeleton's notice and the GPL's text (`COPYING` of the archive) in `licenses\gettext-libintl\plural-Bison-skeleton.txt`; gnulib's `alloca.h`, which the build generates under a GPL-3.0-or-later notice without an exception, gives `vasnprintf.c` only `#include <malloc.h>` and `#define alloca _alloca` for MSVC, so the licence names no GPL for it (a judgement the gate-8 review, round 2, records; [below](#code-under-licences-of-its-own-inside-the-shipped-files)) |
| GNU libiconv (`libiconv`) | 1.19 | LGPL-2.1-or-later | `iconv-2.dll` | As LibRaw | Met: replaceable, notice, source; `iconv-2.dll`'s own sources (`lib/iconv.c`, `lib/compat.c`, `libcharset/lib/localcharset.c`, `woe32dll/iconv-exports.c` and the version resource `windows/libiconv.rc`, as `lib/Makefile.in` lists them and the build log shows) are unmodified: vcpkg's three patches change the `iconv` program's helpers (`srclib/fcntl.c`, `srclib/relocatable.c`) and `libcharset.h.in`, which the DLL does not compile; they are listed all the same, each with the licence of the files it changes (`srclib` LGPL-2.1-or-later as the files state; `libcharset.h.in` LGPL-2.0-or-later, its notice; the gate-8 review, round 2); the converter tables of `lib/*.h` from unicode.org's mapping tables carry libiconv's notice and state no Unicode terms ([below](#code-under-licences-of-its-own-inside-the-shipped-files)) |
| FreeType (`freetype`) | 2.14.3 | `FTL AND MIT AND MIT-open-group`: FreeType under FTL (elected over GPL-2.0-or-later), its BDF and PCF drivers and `src/base/fthash.c` under MIT, the PCF driver's `pcfutil.c` under The Open Group's X11 notice (until the gate-8 review, round 1, of 2026-10-16 `FTL`) | `freetype.dll` | Credit in the documentation; the notices of the MIT and X11 parts with every copy | Met: notice; credit line under "Required notices"; the five parts' notices from the verified archive in `licenses\freetype` |
| libjpeg-turbo | 3.2.0 | `BSD-3-Clause AND IJG AND Zlib`: its SIMD extensions (zlib License) are compiled into `jpeg8.dll` (until the gate-8 review, round 1, of 2026-10-16 `BSD-3-Clause AND IJG`) | `jpeg8.dll` | IJG credit, its README.ijg; the SIMD code's notice | Met: notice with `README.ijg` from the verified archive; credit line; the notice of `simd/nasm/jsimdext.inc` in `licenses\libjpeg-turbo\SIMD-Zlib.txt`, listed with the holders of every compiled SIMD file (Intel in `jcolsamp.inc` and the AVX2 files, Nokia, Loongson, Google, Arm and Samsung in `simd/jsimd.c`, Linaro and the Institute of Software, Chinese Academy of Sciences in `simd/jsimdint.h`, under the same zlib License; since the gate-8 review, round 2; until then only `jsimdext.inc`'s six) |
| libtiff (`tiff`) | 4.7.2 | `libtiff AND BSD-4.3TAHOE AND MIT`: libtiff's licence, the Berkeley notice of the LZW code (`tif_lzw.c`), and the MIT hash set (`tif_hash_set.c`); the PixarLog, LogLuv and CCITT fax code carry the libtiff licence's form with their own holders (until the gate-8 review, round 1, of 2026-10-16 `libtiff`) | `tiff.dll` | Notices; LZW acknowledgement | Met: notice; credit line; the five parts' notices from the verified archive in `licenses\tiff` |
| PCRE2 (`pcre2`) | 10.47 | `BSD-3-Clause WITH PCRE2-exception AND BSD-2-Clause`: its just-in-time compiler, sljit, is BSD-2-Clause and compiled into both DLLs (until the gate-8 review, round 1, of 2026-10-16 `BSD-3-Clause WITH PCRE2-exception`) | `pcre2-16.dll`, `pcre2-8.dll` | Its LICENCE.md; sljit's notice | Met: `LICENCE.md` from the verified archive; sljit's `LICENSE`, from the port's second archive (`zherczeg/sljit` at `45f910b7`, verified by its SHA-512), in `licenses\pcre2\sljit-BSD-2-Clause.txt` |
| HarfBuzz (`harfbuzz`) | 14.5.0 | `MIT-Modern-Variant AND ISC AND MIT AND Unicode-3.0`: HarfBuzz's "Old MIT" licence, the UCDN Unicode functions (ISC), fasthash and Microsoft's Universal Shaping Engine data (MIT), and the Unicode data of its generated tables (until the gate-8 review, round 1, of 2026-10-16 `MIT AND MIT-Modern-Variant`, as the port declares) | `harfbuzz.dll` | Notices | Met: notice; the four parts' notices from the verified archive in `licenses\harfbuzz` (the Unicode License v3's text is a section of `qtbase\copyright.txt`) |
| Little CMS (`lcms`) | 2.19.1 | MIT | `lcms2-2.dll` | Notice; never the GPL plugins | Met (features `fastfloat` and `threaded` off) |
| libpng | 1.6.58 | libpng-2.0 | `libpng16.dll` | Notice | Met |
| zlib | 1.3.2 | Zlib | `z.dll` | Notice in source distributions | Met (its notice ships though its source does not) |
| Brotli (`brotli`) | 1.2.0 | MIT | 2 DLLs | Notice | Met |
| bzip2 | 1.0.8 | bzip2-1.0.6 | `bz2.dll` | Notice and disclaimer in the documentation | Met |
| double-conversion | 3.4.0 | BSD-3-Clause | `double-conversion.dll` | Notice in the documentation | Met |
| MD4C (`md4c`) | 0.6.0 | MIT | `md4c.dll` | Notice | Met |
| SQLite (`sqlite3`) | 3.53.4 | Public domain (`blessing`) | `sqlite3.dll` | None | Listed |
| nlohmann-json | 3.12.0 | `MIT AND CC0-1.0`: Bjoern Hoehrmann's UTF-8 decoder and Florian Loitsch's Grisu2, both MIT, and Hedley, Evan Nemerson's macros, CC0-1.0 (until the gate-8 review, round 1, of 2026-10-16 `MIT`); its Abseil-derived code is not compiled | Compiled into `Lucent.exe` and `lucent-worker.exe` (header-only) | Notice | Met: notice; the README's statements of the three parts and their licences' texts in `licenses\nlohmann-json` (the CC0 legal code is tl-expected's `copyright.txt`) |
| C++/WinRT (`cppwinrt`, the Windows SDK's) | 2.0.250303.1, Windows SDK 10.0.26100.0 | MIT (Copyright (c) Microsoft Corporation; the SDK's `cppwinrt\LICENSE.txt`) | Compiled into Qt Base's `Qt6Core.dll` and `qwindows.dll` (templates of `winrt/base.h`: Qt's build configures C++/WinRT, `QT_FEATURE_cpp_winrt`; both binaries carry its version string) | The MIT notice with the copies | Met since the gate-8 review, round 2 (until then unrecorded; Qt's SBOM does not record it): the SDK's `LICENSE.txt`, pinned by SHA-256 in `installer\license-policy.json` (`toolchainLibraries.cppwinrt`), in `licenses\cppwinrt\LICENSE.txt`; listed with Qt Base in `THIRD-PARTY-NOTICES.txt`; an SBOM package CONTAINED by Qt Base's binary package; packaging refuses another SDK or C++/WinRT version and probes every shipped binary for its version string |
| Microsoft C++ Standard Library (`msvc-stl`, the compiler's own) | MSVC toolset 14.44.35207 | `Apache-2.0 WITH LLVM-exception AND BSL-1.0 AND MIT WITH fmt-exception AND Unicode-DFS-2016`: its headers' licence, Ryu's (BSL-1.0) and {fmt}'s (MIT with its exception) for `std::format` and `std::to_chars`, and Unicode's data licence for `std::format`'s width tables | Compiled into `Lucent.exe` and `lucent-worker.exe` (templates of the standard headers) | The Unicode notice with the copies or in the documentation; the LLVM exception, BSL-1.0 and the {fmt} exception waive their notices for object code | Met since the gate-8 review, round 1, of 2026-10-16 (until then not listed): the three notices verbatim from the toolset's headers, pinned by SHA-256 in `installer\license-policy.json` (`toolchainLibraries`), in `licenses\msvc-stl`; the width table found in both programs. The folder holds no `copyright.txt` and no Apache License text, which the LLVM exception waives for compiled code (sections 4(a), 4(b) and 4(d)); the notices' sentence that every component's folder holds a `copyright.txt` is limited to vcpkg components since the gate-8 review, round 2 |
| tl-expected | 1.3.1 | CC0-1.0 | Compiled in | None | Listed |
| Lensfun lens database (asset `lens-database`) | 1.0.0 (data format 1, 2026-10-08) | CC-BY-SA-3.0 | `assets\lens-database`, 56 XML files | Attribution (title, authors, URI), licence text with every copy, notices intact, no technological measures restricting the licence's rights; adaptations under BY-SA | Met: credit in `THIRD-PARTY-NOTICES.txt` and Help > About; the licence text twice; files unmodified with `provenance.json`; Lucent's integrity check only decides whether Lucent uses the files, which stay plain XML |
| Camera support database (asset `camera-support`) | 3.0.1 | MIT (Lucent's own) | `assets\camera-support` | None beyond Lucent's | Met: named under "Data" in `THIRD-PARTY-NOTICES.txt` as Lucent's own data under its MIT License (since the gate-8 reading of 2026-10-16; until then only "Lucent under the MIT License" covered it) |
| Lucent's logo (asset `branding`) | 1.0.2 | Lucent's own, not MIT: copyright (c) 2026 Fan Zhu, all rights reserved; others may show it, unmodified, only to refer to Lucent; the terms do not limit the GNU LGPL's rights to modify Lucent and its LGPL libraries for one's own use and to reverse engineer Lucent for debugging such modifications (`LicenseRef-Lucent-Logo`, decided by the owner on 2026-10-15) | Built in, not installed as a file: `Lucent.exe` and `lucent-worker.exe` (icon resource; `Lucent.exe`'s window icon and Help > About), Setup and its uninstaller (icon), Setup's wizard images | State its terms, since the MIT License does not cover it | Met (the `-StageOnly` package for the logo's terms, 2026-10-15, above): the terms at the top of `THIRD-PARTY-NOTICES.txt`, and in the SBOM (`Lucent.exe`, `lucent-worker.exe` and Lucent's package `MIT AND LicenseRef-Lucent-Logo`, with the text), from `assets\branding\provenance.json` |
| Microsoft Visual C++ Redistributable | 14.44.35211.0 | MICROSOFT SOFTWARE LICENSE TERMS - MICROSOFT VISUAL C++ 2015 - 2022 RUNTIME (EULA ID `Cpp_2015-2022_ENU.1033`), the licence the package carries; Lucent redistributes it as Distributable Code under the licence of the Visual Studio 2022 Build Tools it came with (<https://go.microsoft.com/fwlink/?LinkId=2179911>; the list of Distributable Code, <https://aka.ms/vs/17/redist.txt>) | Inside Setup only | Unmodified, official package; the Build Tools licence's conditions for Distributable Code | Notices met; **open: the owner's review of the Distributable Code conditions** (`vcRedistributable.termsReview`: pending; packaging warns and gate 8 stays open until it is recorded). Authenticode-checked when packaging, run by Setup only when needed and silently (`/install /quiet /norestart`), so its window and licence are not shown; `THIRD-PARTY-NOTICES.txt` names it with the version the package manifest records (since the gate-8 reading of 2026-10-16) and, since the gate-8 review, round 2, names its licence, which that installer shows when run on its own; the SBOM's text, which said the redistributable installer shows its terms when Setup runs it, is corrected; packaging stops unless the redistributable is the one whose licence was read (`vcRedistributable.reviewed`: version, SHA-256, the licence's name and EULA ID from its `license.rtf`) |
| Inno Setup (Setup and `unins000.exe`) | 6.7.3 | Inno Setup License (`InnoSetup`), non-commercial use | Setup's loader and the uninstaller | Keep its copyright notices; non-commercial use | Met: unmodified; free distribution; credited in `THIRD-PARTY-NOTICES.txt` with the engine version the compiler reports, which the package manifest records (`toolchain.innoSetupVersion`; since the gate-8 reading of 2026-10-16); since the gate-8 review, round 1, its `license.txt` ships as `licenses\inno-setup\license.txt` (the compiler's, SHA-256 as the policy reviewed it for 6.7.3) and the SBOM has an Inno Setup package. Since the gate-8 review, round 2: `license.txt` is also checked against Inno Setup's published source (its git blob at the signed tag `is-6_7_3`), and the code of others inside its loader, setup engine and uninstaller ships with its licences in `licenses\inno-setup` (until then named only, [gap 13](#known-gaps-to-close-before-a-release)): RemObjects Pascal Script (its licence, `Components/UniPs/LICENSE.md` of the published source, whose third condition the credit line under "Required notices" meets), zlib 1.3.1 (Zlib; the text in zlib's `copyright.txt`), the LZMA SDK 25.00 decoders (public domain, as they state) and, in Setup's loader, the LZMA SDK 4.40 decoder, offered under the GNU LGPL or the CPL and redistributed under the LGPL-2.1 (Lucent's election; its text in GNU libintl's `copyright.txt`) with its five source files as published; each is an SBOM package that Inno Setup's package contains |
| vcpkg port recipes (registry `e143402b`) | The ports at the registry baseline | MIT (Copyright (c) Microsoft Corporation); the Lucent project's changes in its overlay ports MIT (Copyright (c) 2026 Fan Zhu); the lines a patch quotes and changes stay under the licences of the files they come from (since the gate-8 review, round 2, derived per file; until then the library's licence) | The overlay recipes of GLib and GNU libintl in `licenses\<port>\recipe` (14 files); every LGPL library's recipe in the corresponding-source archive | Keep the copyright and permission notice with the copies | Met since the gate-8 reading of 2026-10-16 (until then vcpkg's licence was in neither): `licenses\vcpkg-LICENSE.txt` and the archive's `licenses\vcpkg-LICENSE.txt`, LICENSE.txt of the registry at the baseline, checked by its git blob, beside the archive's `licenses\Lucent-LICENSE.txt`; the SBOM gives vcpkg's 6 files that are no patches MIT, the 2 portfiles Lucent changed MIT with both holders, and the 6 patches (vcpkg's 4, Lucent's 2) `MIT AND LGPL-2.1-or-later` (instead of NOASSERTION); since the gate-8 review, round 1, also the 155 recipe files of the port packages, which were NOASSERTION (MIT, a patch also under its library's own licence; an overlay port's files with the licences of their shipped copies). Since the gate-8 review, round 2, a patch's licence is `MIT AND` the licences of the files it changes (the file's SPDX line with Lucent's election among its choices, else the statement or evidence that `installer\license-policy.json` names in the port's `fileLicences`), not its library's: of the 63 patches, for example, Qt's build-file patches `MIT AND BSD-3-Clause` and its library-code patches `MIT AND LGPL-3.0-only`, its two deployment-tool patches `MIT AND GPL-3.0-only WITH Qt-GPL-exception-1.0`, LibRaw's two `MIT AND BSD-3-Clause AND GPL-2.0-or-later`, libiconv's `libcharset.h.in` patch `MIT AND LGPL-2.0-or-later`; Lensfun's `fix_build.patch` has NOASSERTION, since Lensfun's archive states no licence for the file it changes. The archive's `MANIFEST.json` records the same for each patch (`modifications[].licence`, `quotes`) |

Never shipped, enforced by the packaging audit (`neverShipped`): the GNU gettext programs
(GPL-3.0; GLib's build runs them, and Lucent's overlay of their recipe is in the corresponding-source
archive for completeness), Qt's tools (`qtshadertools`, `qtlanguageserver`, `windeployqt`), GoogleTest
and the vcpkg helper ports. libffi (MIT) is reviewed but not imported. No model weights or datasets
ship (ADR 0019).

Not done for the packages of 2026-10-10 and of the release review: their SBOMs were checked
structurally by packaging, but not by `pyspdxtools` (optional, `-SpdxValidator`; the W5 SBOM passed
spdx-tools 0.8.5): spdx-tools was not installed on the reference machine then. It now is, pinned in
the tools environment (`tools\requirements-spdx-tools.txt`), and the SBOM of the package for the
logo's terms (above) passed it. The release checklist's step 6 runs it.

## Code under licences of its own inside the shipped files

The gate-8 review, round 1 (2026-10-16), asked for every shipped component (the 23 of the release
table, the Visual C++ runtime and Inno Setup's loader and uninstaller) to be swept for code compiled
into what ships under a licence or notice of its own ([How this ledger was verified](#how-this-ledger-was-verified)).
The gate-8 review, round 2, found more that round 1 had missed: C++/WinRT of the Windows SDK in Qt
Base's files, LibRaw's DNGLab-derived decoders, and the licences of the code inside Inno Setup.
Each part found is recorded in `installer\license-policy.json` (a port's `parts`; the compiler's
library and the Windows SDK's in `toolchainLibraries`; Inno Setup's in
`distribution.installerTool.parts`; Qt's from Qt's own SBOMs, with the policy's corrections in the
port's `attributionLicences`, `attributionNotices` and `attributionSources`), its licence is in its component's
expression, its notice ships in `licenses\<component>\` with its licence's text or the file that
holds it, `THIRD-PARTY-NOTICES.txt` lists it with its component, and the SBOM has a package for it.
A notice is taken verbatim from the component's source archive, verified by the SHA-512 its port
pins (or, for the compiler's library, from the toolset's header, verified by the SHA-256 the policy
pins; since the gate-8 review, round 2, for C++/WinRT the Windows SDK's licence file, pinned the same
way, and for Inno Setup and Apache Tika files of their published sources at a commit, kept in
`installer\third-party` and checked by their git blob ids); each part's file says where each of its
texts comes from. No licence text is written by Lucent: where an archive names a licence without
its text, the part's file names another component's file from a verified source that holds it
(since round 2; until then the Adobe code's MIT text was the licence's standard words).

Compiled into the shipped files:

| Component | Part | Licence | In | Notice (`licenses\...`) | Licence text |
|---|---|---|---|---|---|
| FreeType 2.14.3 | BDF font driver (`src/bdf`) | MIT | `freetype.dll` | `freetype\BDF-driver-MIT.txt` (`src/bdf/README`) | In the notice |
| FreeType | BDF library from Mark Leisher's xmbdfed (`bdflib.c`, `bdf.h`) | MIT | `freetype.dll` | `freetype\BDF-library-MIT.txt` (`src/bdf/README`) | In the notice |
| FreeType | PCF font driver (`src/pcf`) | MIT | `freetype.dll` | `freetype\PCF-driver-MIT.txt` (`src/pcf/README`) | In the notice |
| FreeType | PCF utilities from X11 (`pcfutil.c`) | MIT-open-group | `freetype.dll` | `freetype\PCF-utilities-MIT-open-group.txt` | In the notice |
| FreeType | Hash functions (`src/base/fthash.c`) | MIT | `freetype.dll` | `freetype\fthash-MIT.txt` | In the notice |
| GNU libintl 0.22.5 | GNU Bison's parser skeleton in `intl/plural.c` (Bison 3.8.2) | GPL-3.0-or-later WITH Bison-exception-2.2 | `intl-8.dll` | `gettext-libintl\plural-Bison-skeleton.txt` | The GPL-3.0 (`COPYING` of the archive) in the same file; the exception in the notice |
| HarfBuzz 14.5.0 | UCDN Unicode functions (`hb-ucd.cc`) | ISC | `harfbuzz.dll` | `harfbuzz\UCDN-ISC.txt` | In the notice |
| HarfBuzz | fasthash (`hb-algs.hh`) | MIT | `harfbuzz.dll` | `harfbuzz\fasthash-MIT.txt` | In the notice |
| HarfBuzz | Universal Shaping Engine data from Microsoft (`src/ms-use`), from which shaper tables are generated | MIT | `harfbuzz.dll` | `harfbuzz\ms-use-MIT.txt` | In the notice |
| HarfBuzz | Unicode data in its generated tables | Unicode-3.0 | `harfbuzz.dll` | `harfbuzz\Unicode-data-Unicode-3.0.txt` (`hb-unicode-emoji-table.hh`) | The Unicode License v3 section of `qtbase\copyright.txt` |
| Lensfun 0.3.4 | A subset of the TRE regular expression library 0.8.0 (`libs/regex`, built as `tre_regex`) | LGPL-2.1-or-later | `lensfun.dll` | `lensfun\TRE-LGPL-2.1-or-later.txt` (`libs/regex/regcomp.c`) | The LGPL-2.1 in `gettext-libintl\copyright.txt`; the LGPL-3.0, a later version the notice permits, in `lensfun\copyright.txt` |
| libjpeg-turbo 3.2.0 | x86-64 SIMD extensions (`simd/`) | Zlib | `jpeg8.dll` | `libjpeg-turbo\SIMD-Zlib.txt` (`simd/nasm/jsimdext.inc`), listed with the holders of every compiled SIMD file (since the gate-8 review, round 2) | In the notice |
| LibRaw 0.22.2 | DCB demosaic and FBDD denoise | BSD-3-Clause | `raw_r.dll` | `libraw\DCB-FBDD-BSD-3-Clause.txt` | In the notice |
| LibRaw | Code from the Adobe DNG SDK 1.4 | MIT | `raw_r.dll` | `libraw\Adobe-DNG-SDK-MIT.txt` (COPYRIGHT's statement) | The archive names the licence without its text; the file names the MIT text of nlohmann-json's `copyright.txt` and `UTF-8-decoder-MIT.txt`, from its verified archive (since the gate-8 review, round 2; until then the MIT License's standard words, supplied by Lucent) |
| LibRaw | The Sony YCC and lossless JPEG decoders, "partially backported from DNGLab's rust code" (`src/decoders/sonycc.cpp`, `src/decompressors/losslessjpeg.cpp`, `internal/losslessjpeg.h`) | LGPL-2.1-only (LibRaw's choice of LGPL-2.1 or CDDL-1.0 in their headers; DNGLab's repository states LGPL-2.1, which Lucent's election fits) | `raw_r.dll` | `libraw\DNGLab-derived-decoders.txt` (`sonycc.cpp`'s header; since the gate-8 review, round 2) | LibRaw's `LICENSE.LGPL` in `libraw\copyright.txt` |
| nlohmann-json 3.12.0 | Bjoern Hoehrmann's UTF-8 decoder | MIT | both programs | `nlohmann-json\UTF-8-decoder-MIT.txt` (`README.md`) | `LICENSES/MIT.txt` of the archive, in the same file |
| nlohmann-json | Florian Loitsch's Grisu2 | MIT | both programs | `nlohmann-json\Grisu2-MIT.txt` (`README.md`) | As above |
| nlohmann-json | Hedley 15, Evan Nemerson's macros | CC0-1.0 | both programs | `nlohmann-json\Hedley-CC0-1.0.txt` (`README.md`, and Hedley's own header from the archive's copy of an earlier nlohmann-json header) | The CC0 1.0 legal code in `tl-expected\copyright.txt` |
| PCRE2 10.47 | sljit, its just-in-time compiler (`zherczeg/sljit` at `45f910b7`, the port's second archive) | BSD-2-Clause | `pcre2-8.dll`, `pcre2-16.dll` | `pcre2\sljit-BSD-2-Clause.txt` (sljit's `LICENSE`) | In the notice |
| libtiff 4.7.2 | LZW compression (`tif_lzw.c`), from Berkeley's compress | BSD-4.3TAHOE | `tiff.dll` | `tiff\LZW-BSD-4.3TAHOE.txt` (`LICENSE.md`) | In the notice; its acknowledgement is a credit line under "Required notices" |
| libtiff | PixarLog compression (`tif_pixarlog.c`) | libtiff (its form, with Pixar's name) | `tiff.dll` | `tiff\PixarLog-libtiff.txt` | In the notice |
| libtiff | LogLuv encoding (`tif_luv.c`) | libtiff (its form, with Greg Ward Larson's name) | `tiff.dll` | `tiff\LogLuv-libtiff.txt` | In the notice |
| libtiff | CCITT fax decoder (`tif_fax3.c`), derived with permission from Frank Cringle's viewfax | libtiff | `tiff.dll` | `tiff\CCITT-fax3-libtiff.txt` (with Frank Cringle's copyright) | In the notice |
| libtiff | Hash set (`tif_hash_set.c`) | MIT | `tiff.dll` | `tiff\hash-set-MIT.txt` | In the notice |
| Qt Base 6.11.1 | 28 records of Qt's SBOM (Unicode data, hash functions, TinyCBOR, the Public Suffix List and more) | Each as Qt's SBOM records it, corrected since the gate-8 review, round 2, where the record falls short: Khronos' OpenGL and OpenGL ES headers `MIT-Khronos-old` (Qt's `REUSE.toml`), Apache Tika's MIME type definitions `Apache-2.0 AND OGL-UK-3.0` (PRONOM's definitions, Tika's NOTICE); covered by the component's licence | `Qt6Core.dll`, `Qt6Gui.dll`, `Qt6Network.dll`, `qwindows.dll` | Listed with holders and sources in `THIRD-PARTY-NOTICES.txt`, with the Public Suffix List's Source Code Form at its commit and Tika's NOTICE lines in `qtbase\Apache-Tika-NOTICE.txt` (round 2) | Sections of `qtbase\copyright.txt` |
| Qt Base 6.11.1 | C++/WinRT 2.0.250303.1 of the Windows SDK 10.0.26100.0 (templates of `winrt/base.h`; not in Qt's SBOM) | MIT | `Qt6Core.dll`, `qwindows.dll` (each carries its version string) | Listed with Qt Base in `THIRD-PARTY-NOTICES.txt` (since the gate-8 review, round 2) | `cppwinrt\LICENSE.txt`, the SDK's file |
| Qt Declarative 6.11.1 | JavaScriptCore macro assembler, YARR and WTF; Yoga 2.0.1 | BSD-2-Clause; MIT | `Qt6Qml.dll`; `Qt6QuickLayouts.dll` | As above | Sections of the Qt copyright files |
| Qt SVG 6.11.1 | XSVG (the arc code) | HPND-sell-variant | `Qt6Svg.dll` | As above | As above |
| Microsoft C++ Standard Library (MSVC 14.44.35207) | `std::format`'s Unicode tables (`__msvc_format_ucd_tables.hpp`) | Unicode-DFS-2016 | both programs (its width table found in each) | `msvc-stl\Unicode-DFS-2016.txt` | In the notice |
| Microsoft C++ Standard Library | `std::format` code derived from {fmt} (`__msvc_formatter.hpp`, `format`) | MIT WITH fmt-exception | both programs | `msvc-stl\fmt-MIT.txt` | In the notice; its exception waives the notice for object code |
| Microsoft C++ Standard Library | Ryu floating-point formatting (`xcharconv_ryu.h`) | BSL-1.0 | both programs | `msvc-stl\Ryu-BSL-1.0.txt` | In the notice; BSL-1.0 waives the notice for object code |
| Inno Setup 6.7.3 | RemObjects Pascal Script, the setup engine's scripting engine | `LicenseRef-RemObjects-Pascal-Script` (its own licence, no SPDX identifier) | Setup's engine (`Setup.e32`), `unins000.exe` | `inno-setup\RemObjects-Pascal-Script-LICENSE.txt` (`Components/UniPs/LICENSE.md` of Inno Setup's published source at the signed tag `is-6_7_3`) | In the file; its third condition, a line naming it and where it can be found, is the credit under "Required notices" |
| Inno Setup | zlib 1.3.1 (deflate and inflate, in the engine's runtime library) | Zlib | Setup's engine, `unins000.exe` | `inno-setup\zlib-Zlib.txt` (the copyright string the engine carries) | zlib's `copyright.txt` (the zlib License, which asks for no notice with binaries) |
| Inno Setup | The LZMA SDK 25.00 decoders (LZMA, LZMA2 and 7z, Igor Pavlov) | Public domain (`LicenseRef-LZMA-SDK-Public-Domain`) | Setup's engine, `unins000.exe` | `inno-setup\LZMA-SDK-decoders-public-domain.txt` (the statement the engine carries) | None applies |
| Inno Setup | The LZMA SDK 4.40 size-optimized LZMA decoder (Igor Pavlov) | LGPL-2.1-only: offered under the GNU LGPL (no version named) or the CPL, with an exception for linked code; Lucent elects the LGPL in its version 2.1 | Setup's loader (`SetupLdr.e32`) | `inno-setup\LZMA-SDK-4.40-decoder.txt` (`LzmaDecode.h` of the published source), its five source files as published in `inno-setup\LzmaDecode\` | GNU libintl's `copyright.txt` (the LGPL-2.1) |

Compiled code with no notice of its own: GLib's dirent emulation (`glib/dirent`, placed in the public
domain, named in GLib's note; it has no licence identifier), the gnulib and libcharset copies of GLib
and GNU libintl (their own LGPL-2.1-or-later), and the Unicode-derived tables of PCRE2 (`pcre2_ucd.c`,
PCRE2's own notice), GLib and MD4C, whose archives state no Unicode terms for them (recorded, not
added); since the gate-8 review, round 2, also GNU libiconv's converter tables (`lib/*.h`, such as
`cp932.h`, `cp950.h` and `big5_2003.h`), which say they come from unicode.org's mapping tables but
carry libiconv's LGPL notice and state no Unicode terms. The Adobe Glyph List table of FreeType's
`src/psnames/pstables.h` carries FreeType's notice.

Judgements recorded by the gate-8 review, round 2, where a notice's file reaches a shipped binary but
gives it no code to which the notice's terms would attach:

- GNU libintl's build generates gnulib's `alloca.h` from `gnulib-lib/alloca.in.h`, whose notice is
  GPL-3.0-or-later without an exception (configure finds no `alloca.h` for MSVC), and
  `gnulib-lib/vasnprintf.c`, compiled into `intl-8.dll`, includes it. For MSVC it gives only
  `#include <malloc.h>` and `#define alloca _alloca`, so no expressive code of it is compiled, and
  libintl's licence names no GPL for it (its note says so).
- Qt Gui's Direct3D 12 backend (`Qt6Gui.dll`) includes the Windows SDK's `d3d12.h` and
  `d3d12sdklayers.h` (MIT) and `dxcapi.h` (University of Illinois/NCSA), which give it declarations
  and GUID constants only, no code, as the C++ standard library's thread headers do for Lucent's
  programs (Qt Base's note says so).
- The MSVC toolset's runtime startup code (`msvcrt.lib`'s objects, such as `mainCRTStartup` and the
  DLL entry point) is linked into both programs and every vcpkg DLL. It is Microsoft's code under the
  Visual Studio 2022 Build Tools' licence, whose conditions for Distributable Code the owner reviews
  with the redistributable's (`vcRedistributable.termsReview`; [gap 15](#known-gaps-to-close-before-a-release)).

In the source archives, but not compiled into what ships:

| Component | Code | Licence | Why it is not in the shipped files |
|---|---|---|---|
| FreeType | HarfBuzz code of `src/autofit` (`ft-hb-ft.c`, `ft-hb-types.h`, `ft-hb-decls.h`, `hb-script-list.h`) | MIT-Modern-Variant ("Old MIT") | Used only with `FT_CONFIG_OPTION_USE_HARFBUZZ`, off in this build (the port's `harfbuzz` feature is not used) |
| FreeType | The zlib copy in `src/gzip` | Zlib | `FT_CONFIG_OPTION_SYSTEM_ZLIB`: `freetype.dll` imports `z.dll` |
| FreeType | MD5 (`src/base/md5.c`) | Public domain | Only with `FT_DEBUG_LEVEL_TRACE` |
| FreeType | The dlg logging library (not in the archive; `src/dlg/dlgwrap.c` includes it) | Not reviewed | Only with `FT_DEBUG_LOGGING` |
| GLib | `glib/gutilsprivate.c` (from xdg-user-dir-lookup.c, Red Hat) | MIT | Included only on Unix (`gutils.c`) |
| GLib | `glib/valgrind.h` (Julian Seward) | bzip2-style BSD | Included only when not compiled with MSVC (`glib/gvalgrind.h`) |
| GLib | The bundled `win_iconv.c` | Public domain | `use-libiconv-on-windows.patch` makes `gconvert.c` use GNU libiconv |
| GLib | girepository's CMPH | LGPL-2.1-only OR MPL-1.1 | girepository does not ship (only `glib-2.0-0.dll` does) |
| libjpeg-turbo | Its copies of libspng and zlib (`src/spng`) | BSD-2-Clause AND libpng-2.0; Zlib | Only `cjpeg` and `djpeg` use them, which do not ship |
| libjpeg-turbo | MD5 (`src/md5`) | Public domain | Only its test programs use it |
| libjpeg-turbo | The TurboJPEG API | BSD-3-Clause | `turbojpeg.dll` does not ship |
| LibRaw | X3F tools (`internal/x3f_tools.h`, `src/x3f`) | BSD-3-Clause | LibRaw-cmake's `ENABLE_X3FTOOLS` is off; its terms ship all the same (`libraw\X3F-BSD-3-Clause.txt`), as the archive Lucent publishes holds the code |
| LibRaw | The RawSpeed glue with RawSpeed's camera data (`RawSpeed/rawspeed_xmldata.cpp`) | CC BY-SA 3.0 (the data) | Only with `USE_RAWSPEED`; the port defines only `USE_JPEG`, `USE_JPEG8`, `USE_LCMS2`, `USE_ZLIB` |
| nlohmann-json | Parts of Google Abseil (`detail/meta/cpp_future.hpp`) | Apache-2.0 per the README, MIT per the file | The `#else` branch of `JSON_HAS_CPP_14`, which Lucent's C++20 (`/std:c++20`) defines |
| libtiff | JPEG and old-JPEG (`tif_ojpeg.c`, Joris Van Damme, AWare Systems), JBIG, LERC, LZMA (Andrey Kiselev), WebP (Mapbox) and ZSTD (Planet Labs) codecs | libtiff's form with their holders | Their codecs are off (the configure summary), so their files compile to nothing |
| libtiff | `port/getopt.c` (the University of California) | BSD-3-Clause | Only the tools use it, which do not ship |
| Qt Base | Vulkan Memory Allocator 3.2.1 | MIT | Qt's SBOM records it for `Qt6Gui.dll`, but only `qrhivulkan.cpp` includes it, compiled only with Vulkan, which this build's configure summary reports off (`attributionsNotCompiled`) |
| Qt Base | Qt's bundled zlib, PCRE2, FreeType, HarfBuzz, libjpeg, libpng, MD4C and double-conversion | Their own | This build uses the system libraries, which ship as their own components |
| Qt Declarative | UDis86, the disassembler | BSD-2-Clause | `WTF_USE_UDIS86=0`: the disassembler feature is off |
| Qt Declarative | The Material style's shadow code | MIT | Its module does not ship |
| Lensfun | Its applications (`apps/`: `lenstool`, the other programs, the Python module) | GPL-3.0 | `BUILD_LENSTOOL=OFF`, the module is not built |
| Lensfun | Its build system (`CMakeLists.txt`, `cmake/`) | Not stated: the files carry no notice, and the README's GPL-3.0 sentence names "the build system (the contents of the build/ subdirectory plus the ac.py file)", which 0.3.4 no longer holds (its ChangeLog: "Removed old non-CMake build system") | Not part of the DLL; vcpkg's `fix_build.patch` changes `CMakeLists.txt`, so the SBOM gives the patch NOASSERTION (the gate-8 review, round 2; until then this row gave the build system GPL-3.0) |
| Lensfun | GNU getopt (`libs/getopt`) | GPL-2.0-or-later | Its build adds it only with `BUILD_TESTS`, which is off (the gate-8 review, round 2) |
| LibRaw | LibRaw-cmake's build files, the port's second archive: `CMakeLists.txt` (Gilles Caulier's notice) and `cmake/` (KDE's BSD scripts and files without a notice, which its `LICENSE` covers) | GPL-2.0-or-later; BSD-3-Clause | Build files: only `libraw_config.h`, which LibRaw LLC's template `cmake/data/libraw_config.h.cmake` makes (LibRaw's licence), reaches the compiled code. vcpkg's two patches change `CMakeLists.txt` and two `cmake/` files, and the archive Lucent publishes holds the GPL-2.0's text for them (the gate-8 review, round 2) |
| Little CMS | The `fastfloat` and `threaded` plugins | GPL-3.0-or-later | Features off |
| GNU libiconv | The `iconv` program and its helpers (`srclib`) | GPL-3.0 (the program); its gnulib helpers under their own notices (`srclib/fcntl.c` and `srclib/relocatable.c`, which vcpkg's patches change, LGPL-2.1-or-later; the gate-8 review, round 2) | Not part of `iconv-2.dll`, not shipped |
| HarfBuzz | The subset, GPU, raster and vector libraries | MIT-Modern-Variant | Their DLLs do not ship |
| Microsoft C++ Standard Library | The thread headers derived from William E. Kempf's Boost.Thread (`xthreads.h`, `xtimec.h`, `__msvc_threads_core.hpp`) | Kempf's permissive notice (HPND-style) | Declarations, types and constants only; the code is in the runtime's `msvcp140.dll`, which Microsoft's redistributable installs under its terms |

**Usage key:**

- **shipped DLL**: deployed with the application; the file names are those the W5 packaging audit
  found in the installation folder.
- **compiled in**: header-only code that ends up inside Lucent binaries.
- **build-only**: needed to build, not deployed.
- **test-only**: used by test executables, never packaged.
- **dev-only**: a developer tool.
- **planned**: arrives with a later feature and is not in `dev-core` builds.

## Engine dependencies (vcpkg default features)

| Component (port) | Version | Licence | Usage | Main obligations when distributed | Licence text source |
|---|---|---|---|---|---|
| LibRaw (`libraw[core,dng-lossy]`) | 0.22.2 | LGPL-2.1-only **or** CDDL-1.0; Lucent elects **LGPL-2.1**. Its parts keep their own licences: DCB and FBDD BSD-3-Clause, the Adobe DNG SDK code MIT, X3F BSD-3-Clause (not compiled); its DNGLab-derived decoders are under LibRaw's licence; used as `LGPL-2.1-only AND BSD-3-Clause AND MIT` | Shipped DLL `raw_r.dll` (thread-safe), imported by `Lucent.exe` through `lucent_raw`. The port's `raw.dll` (manual-link) is not shipped | See [LibRaw](#libraw) | `share/libraw/copyright` (COPYRIGHT, LICENSE.LGPL, LICENSE.CDDL); the parts' notices from the verified source archive (the Adobe code's MIT text, which the archive lacks, is that of nlohmann-json's licence files since the gate-8 review, round 2; until then supplied by Lucent) |
| JasPer (`jasper`) | 4.2.9 | JasPer License Version 2.0 (MIT-style; the port manifest's `license` field is empty) | Build-only: required by the LibRaw port, but `raw_r.dll` does not import `jasper.dll` (W5 audit), so it is not shipped. Ships only if a future LibRaw build imports it | Reproduce the copyright and permission notice and the disclaimer; no endorsement using the holders' names | `share/jasper/copyright` |
| libjpeg-turbo (`libjpeg-turbo[core,jpeg8]`) | 3.2.0, port 1 | IJG AND BSD-3-Clause AND Zlib: its LICENSE.md calls the SIMD extensions' zlib License subsumed by the IJG License in the libjpeg API library; the SIMD code is compiled into `jpeg8.dll`, so Lucent names its licence and ships its notice (`simd/nasm/jsimdext.inc`) with the DLL, although the zlib License requires the notice only in source distributions ([below](#code-under-licences-of-its-own-inside-the-shipped-files)); libspng's and zlib's copies (libpng-2.0, Zlib) and the TurboJPEG API (BSD-3-Clause) are not in `jpeg8.dll` | Shipped DLL `jpeg8.dll`, imported by `Lucent.exe` (JPEG export), `raw_r.dll` (lossy DNG) and Qt's `qjpeg.dll`. `turbojpeg.dll` is not shipped | Product documentation must state "This software is based in part on the work of the Independent JPEG Group." If the TurboJPEG API is used, include the BSD-3-Clause text. No use of IJG or libjpeg-turbo names in advertising | `share/libjpeg-turbo/copyright` (LICENSE.md only; see [gaps](#known-gaps-to-close-before-a-release)) |
| libspng (`libspng`) | 0.7.4, port 1 | BSD-2-Clause AND libpng-2.0 | Build-only: a libjpeg-turbo dependency imported only by `turbojpeg.dll`, which is not shipped (W5 audit) | Reproduce the BSD-2-Clause notice and the libpng-2.0 notice for code derived from libpng | `share/libspng/copyright` (LICENSE plus `spng.c`, whose header carries the libpng notice) |
| Little CMS (`lcms`) | 2.19.1, port 1 | MIT | Shipped DLL `lcms2-2.dll`, imported by `Lucent.exe` (colour management in `engine`/`export`) and `raw_r.dll` | Reproduce the MIT notice. **Never enable** the `fastfloat` or `threaded` features (GPL-3.0-or-later) | `share/lcms/copyright` |
| libtiff (`tiff[core,zip]`, default features off) | 4.7.2 | libtiff licence, plus the Regents of the University of California notice for `tif_lzw.c` (BSD-4.3TAHOE), the libtiff licence's form with other holders for the PixarLog, LogLuv and CCITT fax code, and MIT for `tif_hash_set.c`: `libtiff AND BSD-4.3TAHOE AND MIT` | Shipped DLL `tiff.dll`, imported by `Lucent.exe` (16-bit TIFF export) | Include the copyright and permission notices in the software and related documentation; no use of Sam Leffler, SGI or UC names in advertising; materials must acknowledge that the LZW code "was developed by the University of California, Berkeley" | `share/tiff/copyright` |
| SQLite (`sqlite3[core,json1]`) | 3.53.4, port 1 | Public domain (vcpkg SPDX id `blessing`) | Shipped DLL `sqlite3.dll`, imported by `Lucent.exe` (catalog) | None. Attribution is customary | `share/sqlite3/copyright` (public-domain statement; upstream https://www.sqlite.org/copyright.html) |
| zlib (`zlib`) | 1.3.2, port 2 | Zlib | Shipped DLL `z.dll`, imported by `raw_r.dll`, `tiff.dll`, Qt Core/Gui/Network/SVG, FreeType and libpng | Do not misrepresent the origin; mark altered sources; keep the notice in source distributions | `share/zlib/copyright` |
| nlohmann-json (`nlohmann-json`) | 3.12.0, port 2 | MIT, with code of others its README names: Bjoern Hoehrmann's UTF-8 decoder and Florian Loitsch's Grisu2 (MIT), Hedley (CC0-1.0; its copy in 3.12.0 says MIT with Evan Nemerson's copyright, so both are named), and parts of Google Abseil (Apache-2.0 per the README, MIT per the file) that are not compiled: they are the `#else` branch of `JSON_HAS_CPP_14`, which Lucent's C++20 defines. Used as `MIT AND CC0-1.0` | Compiled into `Lucent.exe` and `lucent-worker.exe` (header-only; the catalog's and recipes' JSON, the worker protocol), and into the developer tools; its notice ships in `licenses\nlohmann-json` | Reproduce the MIT notices; name Hedley's dedication | `share/nlohmann-json/copyright`; the parts' statements and texts from the verified source archive |
| tl-expected (`tl-expected`) | 1.3.1 | CC0-1.0 | Compiled in (header-only); part of the public error contract (ADR 0004). Its notice ships in `licenses\tl-expected` | None (public-domain dedication) | `share/tl-expected/copyright` |

### LibRaw

Lucent uses LibRaw under **LGPL-2.1** (ADR 0003, decision 2). The installed LibRaw DLLs import
`jpeg8.dll`, `lcms2-2.dll` and `z.dll`. A distribution must:

- ship LibRaw as a separate DLL that the user can replace with a compatible modified version;
- include the LGPL-2.1 text and LibRaw's COPYRIGHT notices, which name dcraw (Dave Coffin; LibRaw
  uses none of its RESTRICTED code), DCB and FBDD, the X3F tools and the Adobe DNG SDK 1.4 code;
- include the terms of the parts under their own licences (established from the verified source
  archive, SHA-512 as the port pins it, in the gate-8 reading of 2026-10-16), which binary
  redistribution needs: the DCB demosaic and FBDD denoise code (`src/demosaic/dcb_demosaic.cpp`,
  Jacek Gozdz, FBDD with Luis Sanz Rodriguez; "BSD-like 3-clause" in COPYRIGHT, BSD-3-Clause in the
  file) and the Adobe DNG SDK 1.4 code (Copyright (c) 2005 Adobe Systems Incorporated, MIT; LibRaw
  does not say which code, and its archive holds no MIT text, so the part's file names the MIT text
  of nlohmann-json's licence files, from that component's verified archive, which ship in the same
  folder; until the gate-8 review, round 2, Lucent added the MIT License's text itself), both
  compiled into `raw_r.dll`; and the X3F tools (`internal/x3f_tools.h`,
  `src/x3f`, Roland Karlsson; "BSD-style" in COPYRIGHT, BSD-3-Clause in the files), which the port
  does not compile (LibRaw-cmake's `ENABLE_X3FTOOLS` is off, so `USE_X3FTOOLS` is undefined) but
  which the source archive Lucent publishes holds. Packaging writes them, verbatim from the archive,
  into `licenses\libraw` (`installer\license-policy.json`, `parts`), so `raw_r.dll` is used under
  `LGPL-2.1-only AND BSD-3-Clause AND MIT`;
- name the code that LibRaw's headers say is "partially backported from DNGLab's rust code" (the Sony
  YCC and lossless JPEG decoders, `src/decoders/sonycc.cpp`, `src/decompressors/losslessjpeg.cpp`,
  `internal/losslessjpeg.h`, compiled into `raw_r.dll`; DNGLab by Daniel Vogelbacher): the files offer
  LibRaw's choice of LGPL-2.1 or CDDL-1.0, and LibRaw's archive states no licence of DNGLab, whose
  repository states LGPL-2.1 (read for the gate-8 review, round 2, not a source Lucent ships), so
  Lucent's LGPL-2.1 fits it, while whether CDDL-1.0 is available for these files depends on DNGLab's
  licence. `licenses\libraw\DNGLab-derived-decoders.txt` holds `sonycc.cpp`'s header;
- provide the corresponding source: LibRaw 0.22.2 (tag `0.22.2`, pinned by SHA-512 in the port),
  the LibRaw-cmake build files (commit `eb98e4325aef2ce85d2eb031c2ff18640ca616d3`) and the vcpkg
  port patches (`dependencies.patch` and `fix-install.patch`, which change only LibRaw-cmake's CMake
  files: its `CMakeLists.txt`, under Gilles Caulier's GPL-2.0-or-later notice, and two `cmake/` files
  under its BSD-3-Clause `LICENSE`, so the patches are under those licences, and the archive's
  `licenses/GPL-2.0-only.txt`, from qtbase's verified archive, holds the GPL's text that
  LibRaw-cmake's archive lacks; the gate-8 review, round 2). The portfile also edits the installed header `libraw_types.h` after the build
  (`vcpkg_replace_string` of `#ifdef LIBRAW_NODLL`, `#if 0` for the DLL build); that changes no
  source of `raw_r.dll` and nothing that ships, so it is no patch of LibRaw. Lucent itself does not
  modify LibRaw; vcpkg's patches are listed with their files and date in `LGPL-NOTICE.txt` (W12 review
  LIC-1). All of it is in `Lucent-<version>-corresponding-source.zip` beside the installer (ADR 0024);
- not forbid, in Lucent's end-user terms, modification of the library for the user's own use or
  reverse engineering for debugging such modifications.

The port's copyright file lists no GPL demosaic packs. Enabling the `openmp` feature does not
change the licence, but it does change the build and would be recorded here.

## Lens-profile corrections (W13, [ADR 0023](../adr/0023-lens-profile-corrections.md))

Added with the user's approval of 2026-10-09: the Lensfun library (`vcpkg.json` dependency
`lensfun`) and the Lensfun lens database (the bundled asset `assets/lens-database`). The port closure
is the install plan of the W13 `dev-core` configure at the pinned baseline; the licences below are
the port manifests' declarations and the upstream terms named by each port's copyright step (read in
the pinned vcpkg checkout and the upstream sources on 2026-10-10). The shipped DLL set and the
installed copyright files are re-derived by the packaging audit (`tools\package.ps1`) for every
package; `installer\license-policy.json` records the reviewed versions and conclusions.

| Component (port) | Version | Licence | Usage | Main obligations when distributed | Licence text source |
|---|---|---|---|---|---|
| Lensfun (`lensfun`) | 0.3.4, port 2 | **LGPL-3.0-only**: the README licenses the libraries under "the GNU Lesser General Public License, version 3" (the public header `lensfun.h` additionally says LGPL-2.0-or-later; the sources of `libs/lensfun` carry copyright lines without a licence statement). Its `libs/regex` is a subset of the TRE regular expression library (Copyright (c) 2001-2006 Ville Laurikari), whose files carry LGPL-2.1-or-later notices: Lensfun's build compiles it into `lensfun.dll` (the static library `tre_regex`) where no `regex.h` exists, as with MSVC (the build log: "regex.h - not found"). Used as `LGPL-3.0-only AND LGPL-2.1-or-later` (since the gate-8 review, round 1, of 2026-10-16; until then this row said the other library sources carry no notice). Its applications are GPL-3.0 and are not built (`BUILD_LENSTOOL=OFF`); the port manifest declares no licence | Shipped DLL `lensfun.dll`, imported by `Lucent.exe` and `lucent-worker.exe` through `lucent_engine` (a private dependency: no Lensfun type in Lucent's headers) | As for the other LGPL libraries (ADR 0003): replaceable DLL; licence texts; corresponding source (`lensfun/lensfun` tag `v0.3.4`, pinned by SHA-512 in the port, with the port's `fix_build.patch`, which changes only `CMakeLists.txt` and is listed with its date in `LGPL-NOTICE.txt`, W12 review LIC-1); end-user terms that permit modification and reverse engineering for debugging. The database is not part of the port (row below) | `share/lensfun/copyright` (the README's licence section, GPL-3.0 and LGPL-3.0 texts) |
| GLib (`glib`) | 2.90.0 | LGPL-2.1-or-later (the port declares `LGPL-2.1-or-later AND (LGPL-2.1-only OR MPL-1.1)` for its whole tree) | Shipped DLL `glib-2.0-0.dll`, imported by `lensfun.dll` (Lensfun's data structures, XML parsing). GObject, GIO, GModule and GThread are not imported and do not ship (the packaging audit of the W13 stage, 2026-10-10). Built from Lucent's overlay of the port (`cmake/ports/glib`, W13 closure): vcpkg's recipe (its `use-libiconv-on-windows.patch` makes `glib/gconvert.c` convert with GNU libiconv instead of the bundled win_iconv, and its `libintl.patch` changes `meson.build`) plus `lucent-neutral-locale-dir.patch`, which compiles only the part of `GLIB_LOCALE_DIR` that `glib/ggettext.c` uses on Windows ("/share/locale", appended to the DLL's own folder), so the DLL no longer names the build machine's install prefix; no behaviour of the DLL changes | As for LGPL libraries; the modified library's recipe and patches ship in `licenses\glib\recipe`, and `LGPL-NOTICE.txt` lists every patch with the files it changes and its date (W12 review LIC-1); the SBOM names the recipe | `share/glib/copyright` (`LICENSES/LGPL-2.1-or-later.txt`, `LGPL-2.1-only.txt`, `MPL-1.1.txt`) |
| GNU libintl (`gettext-libintl`) | 0.22.5, port 4 | LGPL-2.1-or-later; the parser `intl/plural.c`, generated by GNU Bison 3.8.2, holds Bison's skeleton under GPL-3.0-or-later with the Bison exception (`GPL-3.0-or-later WITH Bison-exception-2.2`), which lets the larger work be distributed under its own terms: used as `LGPL-2.1-or-later AND GPL-3.0-or-later WITH Bison-exception-2.2` | Shipped DLL `intl-8.dll`, imported by GLib. Built from Lucent's overlay of the port (`cmake/ports/gettext-libintl`): vcpkg's recipe (its `uwp.patch` and `0003-Fix-win-unicode-paths.patch` change `gettext-runtime/intl/langprefs.c`, `gnulib-lib/localcharset.c` and `loadmsgcat.c`), a configure-time locale probe preset, and since the W13 closure `lucent-no-builder-paths.patch`, which gives the default catalog folder of unbound text domains a name no file can have and turns off gnulib's "costly" relocation (both compiled the build machine's folders into the DLL; Lucent binds no text domain and never calls the relocation, `docs/adr/0023`). The recipe also runs vcpkg's `bashify.cmake` on `gettext-runtime/configure` before configuring (libtool quoting into bash variable expansion), listed with the patches since the gate-8 review, round 1 | As for LGPL libraries; the modified library's recipe and patches ship in `licenses\gettext-libintl\recipe`, and `LGPL-NOTICE.txt` lists every patch with the files it changes and its date (W12 review LIC-1); the SBOM names the recipe | `share/gettext-libintl/copyright` (`gettext-runtime/intl/COPYING.LIB`) |
| GNU libiconv (`libiconv`) | 1.19 | LGPL-2.1-or-later (libiconv and libcharset); the `iconv` program and the documentation are GPL-3.0 and are not shipped. The port manifest declares no licence | Shipped DLL `iconv-2.dll`, imported by GNU libintl and GLib; `charset-1.dll` is not imported and does not ship (the packaging audit, 2026-10-10) | As for LGPL libraries | `share/libiconv/copyright` (read 2026-10-10: the port's note "The libiconv and libcharset libraries and their header files are under LGPL ... The iconv program and the documentation are under GPL", then `COPYING.LIB` and `COPYING`) |
| PCRE2 (`pcre2`) | 10.47, port 1 | As in the Qt section | The 8-bit library, imported by GLib, ships in addition to Qt's 16-bit one | As in the Qt section | As in the Qt section |
| libffi (`libffi`) | 3.8.0 | MIT | Dependency of GLib's GObject; does not ship (`glib-2.0-0.dll` does not import it, confirmed by the packaging audit of 2026-10-10; `installer/license-policy.json` reviews it in case `ffi-8.dll` ever ships) | Reproduce the MIT notice if it ships | `share/libffi/copyright` (read 2026-10-10: the MIT permission notice, "Copyright (c) 1996-2026 Anthony Green, Red Hat, Inc and others") |
| dirent (`dirent`) | 1.26 | MIT | Build-only header for GLib (`neverShipped` in the licence policy) | None while not shipped | `share/dirent/copyright` |
| GNU gettext tools (`gettext[core,tools]`, host) | 0.22.5, port 4 | GPL-3.0 (programs) | Build-only: GLib's build runs them. Listed in `neverShipped`: the packaging audit rejects any of its files | None while not shipped | `share/gettext/copyright` |
| `vcpkg-make` | 2026-07-09 | MIT (vcpkg helper scripts) | Build-only (autotools ports) | None | `share/vcpkg-make/copyright` |
| **Lensfun lens database** (asset `lens-database` 1.0.0, not a vcpkg port) | Data format 1, published 2026-10-08 (`https://lensfun.github.io/db/version_1.tar.bz2`, SHA-256 `97c9f8059accae0128a413bc610a04a4548e8da2764a6d3d60d82bf917639457`), generated from lensfun commit `43f9e001ab66c4fcdd4f400463f13b72f94b2288` | **CC BY-SA 3.0** Unported (copyright the Lensfun project and its contributors) | Bundled data: `assets\lens-database` (56 XML files, unmodified, with `COPYING.CC_BY-SA_3.0` and `provenance.json`), verified against the build's pin and parsed by Lensfun in the engine | Attribution (credit line in `THIRD-PARTY-NOTICES.txt`, "Data"); the licence text (`licenses\lens-database\COPYING.CC_BY-SA_3.0`, also inside the asset); keep the files unmodified or license any adaptation of the database under BY-SA. Lucent ships the files unchanged beside its own code: a collection, not an adaptation, so the share-alike terms bind the database alone | `assets/lens-database/COPYING.CC_BY-SA_3.0` (`data/COPYING.CC_BY-SA_3.0` of the same commit) |

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

## Application dependencies (vcpkg `ui` feature)

Not part of `dev-core` builds. Verified on 2026-10-03 (W5) against the installed copyright files and
the files the packaging audit found in the installation folder.

| Component (port) | Version | Licence | Usage | Main obligations | Status |
|---|---|---|---|---|---|
| Qt Base (`qtbase`, default features off; features `concurrent`, `doubleconversion`, `freetype`, `gui`, `harfbuzz`, `jpeg`, `network`, `opengl`, `pcre2`, `png`, `testlib`, `thread`, `windeployqt`) | 6.11.1, port 3 | Lucent uses **LGPL-3.0-only**; Qt also offers GPL and commercial terms. The port manifest's `license` field is empty | Shipped DLLs `Qt6Core`, `Qt6Gui`, `Qt6Network` (imported by Qt QML and Qt Quick), `Qt6OpenGL` (imported by Qt Quick) and plugins `qwindows`, `qjpeg`, `qgif`, `qico`. Qt Concurrent is not imported and not shipped; Qt Test is test-only; `windeployqt` is a deployment tool. The QML debugging, TUIO, network-information and TLS plugins are not shipped. Qt's Windows entry point `Qt6EntryPoint.lib` (`LicenseRef-Qt-Commercial OR BSD-3-Clause`) is linked statically into `Lucent.exe`, used under **BSD-3-Clause** | LGPL-3.0 obligations as for LibRaw (replaceable DLLs, licence text, corresponding source including the vcpkg `qtbase` patches, and end-user terms that permit modification and reverse engineering for debugging). `Qt6Core.dll` is shipped unpatched (`windeployqt --no-patchqt`). The BSD-3-Clause notice of the entry point, and the holders and licences of the third-party code Qt compiles into the shipped files (Qt's `*_Attribution_*` SBOM packages: Unicode data, hash functions, TinyCBOR, the Public Suffix List under MPL-2.0 and others), must be reproduced; the component's licence names each of their licences (release table). The Public Suffix List's Source Code Form must be named (MPL-2.0 section 3.2(a)), Apache Tika's NOTICE lines for its MIME type definitions included (Apache-2.0 section 4(d)), and C++/WinRT's MIT notice (the Windows SDK's code in `Qt6Core.dll` and `qwindows.dll`) reproduced (the gate-8 review, round 2) | Verified (W5); `licenses\qtbase` carries the copyright file and the extracted LGPL-3.0 and GPL-3.0 texts; `THIRD-PARTY-NOTICES.txt` lists the entry point and the third-party code from Qt's SBOMs (also for Qt Declarative and Qt SVG); since round 2 also `licenses\qtbase\Apache-Tika-NOTICE.txt` and `licenses\cppwinrt\LICENSE.txt`, and the notices name the list's Source Code Form |
| Qt Declarative (`qtdeclarative`) | 6.11.1 | LGPL-3.0-only (as above); the third-party code Qt's SBOM records inside: the JavaScriptCore macro assembler, YARR and WTF (BSD-2-Clause) and Yoga (MIT) | Shipped DLLs (Qt QML, Qt Quick, Quick Controls with the Fusion and Basic styles only, Quick Dialogs, Layouts, Shapes, Effects, Templates, `Qt.labs.folderlistmodel`) and their QML modules | As above | Verified (W5) |
| Qt SVG (`qtsvg`) | 6.11.1 | LGPL-3.0-only (as above); the arc code derived from XSVG (HPND-sell-variant) inside | Shipped DLL `Qt6Svg` with the `qsvg` image and `qsvgicon` icon-engine plugins | As above | Verified (W5) |
| Qt Shader Tools (`qtshadertools`) | 6.11.1 | Qt tools: GPL-3.0 with the Qt GPL Exception 1.0, or commercial (https://doc.qt.io/qt-6/licensing.html) | Build-only: `qsb` compiles shaders at build time. Not shipped; the packaging audit rejects any of its files | None while not shipped | Verified not shipped (W5) |
| Qt Language Server (`qtlanguageserver`) | 6.11.1 | As Qt tools | Build-only (a dependency of `qtdeclarative`'s tooling). The packaging audit rejects any of its files | None while not shipped | Verified not shipped (W5) |
| double-conversion | 3.4.0 | BSD-3-Clause (V8 project authors); the port manifest declares no licence | Shipped DLL `double-conversion.dll` (Qt Core) | Reproduce the notice and disclaimer in documentation | Copyright file read; election recorded in `installer\license-policy.json` |
| PCRE2 (`pcre2[core,jit,platform-default-features]`) | 10.47, port 1 | BSD-3-Clause WITH PCRE2-exception (upstream `LICENCE.md`; the port manifest says BSD-3-Clause), and BSD-2-Clause for its just-in-time compiler sljit, which the port takes from an archive of its own and compiles into the DLLs: `BSD-3-Clause WITH PCRE2-exception AND BSD-2-Clause` | Shipped DLLs `pcre2-16.dll` (Qt Core regular expressions) and `pcre2-8.dll` (imported by GLib since W13); the other PCRE2 DLLs are not shipped (until the gate-8 review, round 2, this row named `pcre2-16.dll` only) | Reproduce the upstream LICENCE notices | Upstream `LICENCE.md` taken from the verified source archive into `licenses\pcre2` (W5) |
| HarfBuzz (`harfbuzz[core,freetype]`) | 14.5.0 | MIT-Modern-Variant ("Old MIT"), with the parts its COPYING points to: UCDN (ISC), fasthash and Microsoft's USE data (MIT), Unicode data (Unicode-3.0): `MIT-Modern-Variant AND ISC AND MIT AND Unicode-3.0` | Shipped DLL `harfbuzz.dll` (text shaping); its other DLLs are not shipped | Reproduce the notices | Copyright file read |
| FreeType (`freetype[brotli,bzip2,core,png,zlib]`) | 2.14.3 | FTL **or** GPL-2.0-or-later; Lucent elects **FTL**. Its LICENSE.TXT names parts under licences like the X Window System's: the BDF and PCF drivers and `fthash.c` (MIT) and `pcfutil.c` (MIT-open-group), all compiled: `FTL AND MIT AND MIT-open-group` | Shipped DLL `freetype.dll` (font rasterisation) | Credit in product documentation: "Portions of this software are copyright (c) 2026 The FreeType Project (https://freetype.org). All rights reserved." (in `THIRD-PARTY-NOTICES.txt`) | Copyright file read |
| libpng (`libpng`) | 1.6.58 | libpng-2.0 | Shipped DLL `libpng16.dll` (FreeType, Qt Gui) | Keep the notice in source distributions | Copyright file read |
| Brotli (`brotli`) | 1.2.0 | MIT | Shipped DLLs `brotlidec.dll` and `brotlicommon.dll` (FreeType WOFF2); `brotlienc.dll` is not shipped | Reproduce the MIT notice | Copyright file read |
| bzip2 (`bzip2[core,tool]`) | 1.0.8, port 6 | bzip2-1.0.6 (BSD-style) | Shipped DLL `bz2.dll` (FreeType); the `tool` executable is not shipped | Reproduce the notice and disclaimer in documentation | Copyright file read |
| MD4C (`md4c`) | 0.6.0 | MIT | Shipped DLL `md4c.dll` (imported by Qt Gui for Markdown text); `md4c-html.dll` is not shipped | Reproduce the MIT notice | Copyright file read |
| EGL Registry (`egl-registry`) | 2025-05-27 | Apache-2.0 and Khronos MIT-style (per file) | Build-only headers | None while not shipped | Copyright file read |
| OpenGL Registry (`opengl-registry`) | 2026-08-03 | Per-file licences (each header states its own) | Build-only headers | None while not shipped | Copyright file read |
| OpenGL (`opengl`) | 2022-12-04, port 3 | Windows SDK licence | Build-only: links the OS `opengl32.dll`, which is part of Windows | None | Copyright file read |

## Runtime prerequisite (not a vcpkg port)

| Component | Version | Licence | Usage | Obligations |
|---|---|---|---|---|
| Microsoft Visual C++ Redistributable (x64, 14.x) | Bundled: `vc_redist.x64.exe` 14.44.35211.0 from the Visual Studio 2022 Build Tools (matches MSVC 14.44) | Its own licence, MICROSOFT SOFTWARE LICENSE TERMS - MICROSOFT VISUAL C++ 2015 - 2022 RUNTIME (EULA ID `Cpp_2015-2022_ENU.1033`, read in its `license.rtf` for the gate-8 review, round 2), and the Build Tools licence's conditions for Distributable Code, under which Lucent redistributes it (the owner's review pending: release table) | Required at runtime by the dynamic CRT (`/MD`) that Lucent and every vcpkg DLL use. The shipped files import `msvcp140`, `msvcp140_1`, `msvcp140_2`, `msvcp140_atomic_wait`, `vcruntime140` and `vcruntime140_1` (W5 audit) | Bundled unmodified in Setup, Authenticode-checked when packaging, and run by Setup (elevated) only when the installed runtime is older ([ADR 0009](../adr/0009-windows-packaging.md), decision 5). The runtime DLLs are never copied into the installation folder |

## Development-only tools and references (never shipped)

| Component | Version on the reference machine | Licence | Usage |
|---|---|---|---|
| Visual Studio 2022 Build Tools (MSVC, Windows SDK 10.0.26100) | 17.14.41, MSVC 14.44.35207 | Microsoft Software License Terms for Visual Studio 2022 Build Tools (<https://go.microsoft.com/fwlink/?LinkId=2179911>, as the installation records them) and the Windows SDK. Each developer must confirm their use is covered. Their conditions for Distributable Code, under which a release redistributes the Visual C++ redistributable and links the runtime's startup code, are recorded for the owner's review (`vcRedistributable.termsReview`, pending; until the gate-8 review, round 2, this row said they were not reviewed, and nothing tracked them) | Compiler, linker, SDK. The code of its C++ standard library headers that the programs instantiate is part of them: release table, "Microsoft C++ Standard Library" (the open-source MSVC STL, whose notices ship); and the Windows SDK's C++/WinRT is compiled into Qt Base's files: release table, "C++/WinRT" (its MIT licence ships) |
| CMake (pip package in the tools venv) | 4.4.3 | BSD-3-Clause | Build system |
| Ninja | 1.13.2 (winget); Visual Studio's bundled 1.12.1 is what builds use today (see setup) | Apache-2.0 | Build tool |
| vcpkg tool and registry | tool 2026-07-27, registry `e143402b` | MIT | Dependency manager; its port recipes ship (release table, "vcpkg port recipes") |
| Python | 3.12.10 | PSF-2.0 | Hosts the pinned CMake and the research environment |
| Git for Windows | 2.55.0 | GPL-2.0 | Version control; not linked |
| Research environment (Python packages such as rawpy, which bundles LibRaw) | See `research/README.md` | Per package (rawpy: MIT wrapper; the bundled LibRaw keeps its LGPL-2.1/CDDL-1.0 terms) | Numerical references and experiments; never a runtime dependency |
| Camera test fixtures | `fixtures/manifest.json` (fetched by `tools/fetch-fixtures.ps1`) | Per fixture, recorded in the manifest. The W1 set comes from raw.pixls.us and each file is marked CC0-1.0 | Downloaded on demand into `fixtures/cache/`, never committed or shipped |
| Inno Setup | 6.7.3 (any 6.3 or later builds the installer) | Inno Setup License; the 6.7.3 compiler reports "Non-commercial use only" | Builds `Lucent-Setup-<version>-x64.exe`. Its Setup and uninstaller code is part of every installer (release table, "Inno Setup"). Reviewed for the first release (2026-10-10): Lucent is distributed free and not sold, so the non-commercial terms apply; packaging records the compiler's licence line and refuses a non-commercial compiler for a commercial distribution; Inno Setup is credited in `THIRD-PARTY-NOTICES.txt` with its version (ADR 0024) |
| RawTherapee, darktable, ART | Any | GPL-family (blueprint Appendix C) | Visual comparison references only: run as separate programs, never linked, bundled, invoked by Lucent or used as a source of code (ADR 0003, decision 6) |

## Known gaps to close before a release

1. **libjpeg-turbo** (closed in W5): the installed copyright file contains only `LICENSE.md`.
   Packaging takes the IJG text it refers to (`README.ijg`) from the upstream source archive, after
   checking the archive's SHA-512 against vcpkg's record, into `licenses\libjpeg-turbo`.
2. **PCRE2** (closed in W5): the installed copyright file only points to the distribution's
   `LICENCE.md`; packaging takes that file (including the JIT compiler's terms) from the verified
   upstream source archive into `licenses\pcre2`.
3. **Qt ports** (closed in W5): licences checked against the installed copyright files; the
   shipped module set is the one the packaging audit records.
4. **Deployed set** (closed in W5): `raw_r.dll` ships, `raw.dll` does not; `jpeg8.dll` ships,
   `turbojpeg.dll` and libspng do not; JasPer does not. The audit re-derives the set for every
   package.
5. **Corresponding source for LGPL components** (closed in W12, ADR 0024): every package run writes
   `Lucent-<version>-corresponding-source.zip` beside the installer with the upstream archives,
   recipes, triplet and build records of the 8 LGPL libraries, reads it back to check it, and names it
   in `licenses\LGPL-NOTICE.txt`; it is published with the installer, from the same place, for as
   long as the installer is offered.
6. **Inno Setup terms** (closed in W12, ADR 0024): Lucent is distributed free and is not sold, so
   the compiler's non-commercial terms apply; the policy records it and packaging enforces it.
7. **Packaging inputs:** the two texts of items 1 and 2 and the corresponding-source archive need the
   upstream archives in vcpkg's downloads folder; a machine that restored the ports from the binary
   cache must download them first (`vcpkg install --only-downloads`, which fetches only the archives
   the ports pin; `docs/development/setup.md`, Packaging the installer).
8. **Third-party code inside Qt** (closed in W5): Qt's copyright files carry generic licence texts
   only, not the holders of the code Qt compiles into its DLLs. Packaging reads Qt's own SBOMs
   (`vcpkg_installed\<triplet>\sbom\`) and lists that code with holders, licences and sources in
   `THIRD-PARTY-NOTICES.txt` and the SBOM; a licence without a text in the Qt copyright files stops
   packaging.
9. **Static linking** (closed in W5): packaging detects vcpkg libraries linked statically into
   `Lucent.exe` and requires a reviewed entry for each (today `Qt6EntryPoint.lib`, BSD-3-Clause);
   one under an LGPL or GPL licence stops packaging.
10. **Modified LGPL libraries** (closed in the W12 release review, ADR 0003's amendment of
    2026-10-15): the notices named only Lucent's two `lucent-*.patch` files as changes, although
    vcpkg's recipes patch the source of GLib, libintl, Qt, LibRaw, Lensfun and libiconv too.
    Packaging now lists every patch a library's recipe names, with the files it changes, who made it
    and when, in `LGPL-NOTICE.txt` and the corresponding-source archive.
11. **The gate-8 reading of the 0.1.0 package** (closed on 2026-10-16, ADR 0024's amendment of that
    date): LibRaw's licence left out its MIT code and the notices named its parts' licences without
    their terms; `THIRD-PARTY-NOTICES.txt` named neither the Visual C++ redistributable's nor Inno
    Setup's version, nor the camera support database; vcpkg's licence shipped nowhere although its
    recipe files ship in the installation and the archive, where the SBOM left them NOASSERTION, as it
    did `qt.conf` and `Lucent-files.txt`; Qt Base's note left out four patches that change C++ code,
    libiconv's note `lib/compat.c`; the LibRaw section counted a header edit after the build as a
    patch, and the nlohmann-json row left out `lucent-worker.exe`. Packaging and
    `installer\tests\Test-SourcePackage.ps1` (its self-test, and `-PackageManifest` on a package and
    its installation folder) now check what the package states of each of these; this ledger's own
    rows (LibRaw's patches, nlohmann-json's programs, the libiconv sources) were corrected by hand
    and are read at gate 8.
12. **The gate-8 review, round 1, and its sweep** (closed on 2026-10-16, ADR 0024's amendment of that
    name): the licences of FreeType, PCRE2, nlohmann-json, Lensfun and libtiff left out parts compiled
    into the shipped files under licences of their own (the BDF and PCF drivers, `pcfutil.c` and
    `fthash.c`; sljit; the UTF-8 decoder, Grisu2 and Hedley; the TRE subset; the LZW code), and the
    sweep found more: libtiff's PixarLog, LogLuv, CCITT and hash-set code, HarfBuzz's UCDN, fasthash,
    USE data and Unicode data, libjpeg-turbo's SIMD code, GNU libintl's Bison skeleton, the C++
    standard library's Unicode tables, {fmt} and Ryu code in both programs, and the third-party code
    Qt's SBOMs record inside Qt Base, Qt Declarative and Qt SVG, which their licences did not cover.
    Each is now in its component's licence, notices and SBOM ([the section above](#code-under-licences-of-its-own-inside-the-shipped-files)),
    with the code present but not compiled and the reason. Also: GNU libintl's recipe runs
    `bashify.cmake` on `gettext-runtime/configure`, which the notices did not list; the notices said
    that compiled-in and static code is part of `Lucent.exe` only; this ledger's Lucent row named
    third-party files as Lucent's; Qt Base's row left out the configure test of
    `silence-winrtbase-coroutine-warnings.diff`; three passages read as Lucent adding `lib/compat.c` to
    libiconv or as every part's terms coming from its archive; the gate-7 check read the patch notes of
    the working tree instead of the package's; the SBOM gave LibRaw's parts LibRaw's version, had no
    package for Inno Setup, and left 470 files NOASSERTION where ADR 0024 said only the licence texts
    were (435 of them: vcpkg's recipe files of every port package and the third-party binaries). Packaging and
    `Test-SourcePackage.ps1` check each of these now.
13. **Open after the gate-8 review, round 1** (recorded for the owner's review at gate 8; none is
    checked by a tool beyond what it says). Superseded by the gate-8 review, round 2: its first point
    is closed (item 14), and the others, with what round 2 changed in them, are item 15.
    - Inno Setup's setup engine and uninstaller hold code of RemObjects Pascal Script, zlib and Igor
      Pavlov's LZMA decoder (their copyright strings are in the binaries, and the notices name them),
      but Inno Setup's `license.txt` states no terms for them and no verified source of their texts is
      at hand, so none ships; Inno Setup's own licence file does.
    - Two notices are verified by SHA-256 values that `installer\license-policy.json` pins after a
      review, not by a source archive or the vcpkg registry: the C++ standard library's headers in the
      MSVC toolset and Inno Setup's `license.txt`. Another toolset or Inno Setup version stops
      packaging until it is reviewed.
    - Three licence texts come from elsewhere than the part's own archive: TRE's LGPL-2.1 (GNU
      libintl's `copyright.txt`), Hedley's CC0 legal code (tl-expected's) and the Unicode License v3
      of HarfBuzz's data (Qt Base's); the Adobe DNG SDK code's MIT text is the licence's standard
      words, supplied by Lucent. Each part's file says so.
    - Judgments: the SPDX identifier `BSD-4.3TAHOE` for libtiff's Berkeley LZW notice and `libtiff`
      for the PixarLog, LogLuv and CCITT notices (the libtiff licence's form with other holders);
      Hedley named under both CC0-1.0 (its README and dedication) and MIT (the 3.12.0 copy's SPDX
      line); Qt's attribution of the JavaScriptCore macro assembler includes UDis86's holder,
      although UDis86 is not compiled.
    - The Unicode-derived tables of PCRE2, GLib and MD4C carry no Unicode terms in their archives;
      they are recorded, not given a licence of their own.
14. **The gate-8 review, round 2** (closed on 2026-10-16, ADR 0024's amendment of that name): round
    1's fixes left twenty-one discrepancies, F1 to F21. C++/WinRT of the Windows SDK, compiled into
    Qt Base's `Qt6Core.dll` and `qwindows.dll`, was unrecorded (F1); the Public Suffix List's Source
    Code Form was said to be in qtbase's archive, which holds only the array generated from it (F2);
    the SBOM's text for the Visual C++ redistributable said its installer shows its terms when Setup
    runs it, which Setup does silently, and the Build Tools licence's conditions for Distributable
    Code were not tracked (F3); the code of others inside Inno Setup shipped without its licences, and
    its LZMA SDK 4.40 decoder without its source (F4); every patch had its library's licence instead
    of the licences of the files it changes, wrongly for Qt's deployment-tool patches (GPL-3.0 with
    Qt's exception, F5) and LibRaw's, which change LibRaw-cmake's GPL-2.0-or-later and BSD-3-Clause
    build files, whose GPL text the archive lacked (F6); the Adobe code's MIT text was written by
    Lucent (F7); gnulib's GPL-3.0 `alloca.h` in `intl-8.dll` (F8) and LibRaw's DNGLab-derived decoders
    (F9) were not recorded; the archive's README said each library's licence texts are in its source
    archive, which is not so where the archive names a licence without its text (F10);
    Lensfun's build system was given GPL-3.0, which its archive states for a build system it no
    longer has (F11); Tika's NOTICE lines did not ship (F12); Khronos' headers were MIT instead of
    `MIT-Khronos-old` (F13); the SIMD part named only `jsimdext.inc`'s holders (F14); the programs'
    version resources said "MIT License" (F15), and the SBOM gave the program files only
    `MIT AND LicenseRef-Lucent-Logo` while the checklist said more (F16); the Qt section's PCRE2 row
    left out `pcre2-8.dll` (F17); the notices counted Qt Declarative's QML source files as module
    files (F18); Lensfun's GPL-2.0 getopt (F19) and libiconv's Unicode-derived tables (F20) were not
    recorded; and the notices' sentence on `copyright.txt` did not hold for `msvc-stl` (F21). Each is
    fixed at its source (policy, packaging, notice templates, SBOM, this ledger) and guarded by
    `Test-SourcePackage.ps1` (self-test section 13; `-PackageManifest`); the review also found the
    Open Government Licence of the PRONOM definitions in Tika's MIME types and the licences of the
    files that the patches of libtiff (`FindCMath.cmake`, BSD-3-Clause) and libiconv
    (`libcharset.h.in`, LGPL-2.0-or-later) change.
15. **Open after the gate-8 review, round 2** (for the owner at gate 8; packaging checks what each
    point says and nothing beyond it):
    - **The Distributable Code conditions** of the Visual Studio 2022 Build Tools' licence, under which
      a release redistributes `vc_redist.x64.exe` (run silently) and links the runtime's startup code
      into both programs and every vcpkg DLL: not reviewed. `installer\license-policy.json`
      `vcRedistributable.termsReview` is `pending`; packaging warns, the package manifest records it,
      and `Test-SourcePackage.ps1 -PackageManifest` prints it. Gate 8 stays open until the owner
      records the review (what the conditions require, for example naming Microsoft's terms, which the
      notices now do, or running the redistributable with `/passive`).
    - **The verification route of five sources that are no source archive and no vcpkg registry
      file:** the C++ standard library's headers in the MSVC toolset, Inno Setup's `license.txt` and the
      Windows SDK's `cppwinrt\LICENSE.txt`, each pinned by SHA-256 after a review (Inno Setup's also by
      its git blob at the signed tag `is-6_7_3`), and the files of Inno Setup's and Apache Tika's
      published sources kept in `installer\third-party`, pinned by their git blob ids at the commits
      the policy names. The owner accepts this route (or names another) at gate 8.
    - **Licence texts from another component's file** (each part's file says which): TRE's LGPL-2.1
      and the LZMA SDK 4.40 decoder's (GNU libintl's `copyright.txt`), Hedley's CC0 legal code
      (tl-expected's), the Unicode License v3 of HarfBuzz's data (Qt Base's), the Adobe code's MIT text
      (nlohmann-json's licence files) and Inno Setup's zlib's (zlib's `copyright.txt`).
    - **Elections and judgements:** the LGPL-2.1 elected for the LZMA SDK 4.40 decoder (its files name
      the GNU LGPL without a version, or the CPL); `OGL-UK-3.0` added to Qt Base for the PRONOM
      definitions in Tika's MIME types (Tika's NOTICE; Qt's SBOM records Apache-2.0); Lensfun's
      `fix_build.patch` NOASSERTION (its archive states no licence for `CMakeLists.txt`); DNGLab's
      LGPL-2.1, read in its repository, not in a source Lucent ships or verifies; the `alloca.h`,
      Direct3D 12 and DXC headers giving no code (above); and round 1's: `BSD-4.3TAHOE` and `libtiff`
      for libtiff's notices, Hedley under both CC0-1.0 and MIT, and UDis86's holder in Qt's attribution
      of the JavaScriptCore macro assembler.
    - The Unicode-derived tables of PCRE2, GLib, MD4C and GNU libiconv carry no Unicode terms in their
      archives; they are recorded, not given a licence of their own.
