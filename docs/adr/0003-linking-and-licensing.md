# ADR 0003: Linking model and third-party licensing

- **Status:** Accepted (amended 2026-10-10: "Amendment: the release decisions" below, with ADR 0024;
  amended 2026-10-15 by the W12 release review: "Amendment: modified libraries and the copyleft guard"
  below)
- **Date:** 2026-09-24
- **Workflow:** W1 (Foundation and RAW probe)

## Context

Lucent's own source code is MIT-licensed (`LICENSE`). The product it becomes is an installed Windows
application that bundles third-party libraries (blueprint section 9). Those libraries come under
several kinds of terms (blueprint Appendix C; verified against the installed vcpkg copyright files
on 2026-09-24, see `docs/legal/third-party-notices.md`):

- **Weak copyleft, library-level.** LibRaw 0.22.2 is offered under a choice of **LGPL-2.1** or
  **CDDL-1.0**. It also carries notices for code it includes: dcraw, the DCB demosaic and FBDD
  denoise code (BSD-style 3-clause), the X3F library (BSD-style) and parts of the Adobe DNG SDK
  (MIT). Qt 6 modules (the `ui` feature) are available under LGPL-3.0, GPL or a commercial licence.
  Qt's tools are GPL-3.0 with the Qt GPL Exception 1.0, or commercial.
- **Permissive.** Little CMS (MIT), libjpeg-turbo (IJG and BSD-3-Clause), libtiff (libtiff
  licence), zlib (Zlib), SQLite (public domain), JasPer (JasPer License 2.0), libspng (BSD-2-Clause
  and libpng-2.0), nlohmann-json (MIT), tl-expected (CC0-1.0), GoogleTest (BSD-3-Clause).
- **Licence-changing options inside permissive ports.** The Little CMS `fastfloat` and `threaded`
  plugins are GPL-3.0-or-later. FreeType (a Qt dependency) is offered under either the FreeType
  License (FTL) or GPL-2.0-or-later.
- **GPL applications.** RawTherapee, darktable and ART are full applications under GPL-family
  licences. The blueprint uses their AMaZE and RCD renderings as quality references and states that
  direct reuse "requires choosing a compatible distribution strategy" (section 5, Appendix A.4).

The blueprint requires dynamic linking for the LGPL route, an accurate inventory, notices, a source
provision for covered libraries and a shipped licence bundle generated from the actual build
(section 9). Development also uses tools and data that must never reach the installed product.

## Decision

1. **Dynamic linking for all third-party libraries.** The `x64-windows-lucent` triplet (ADR 0002)
   builds every vcpkg dependency as a DLL. LGPL-covered libraries (LibRaw, Qt) are shipped as
   separate, replaceable DLLs and are never linked statically into Lucent binaries. Lucent does not
   modify LibRaw or Qt sources itself. The vcpkg port patches and build settings that apply are part
   of the corresponding source recorded for a release.
2. **LibRaw is used under LGPL-2.1.** Lucent elects the LGPL-2.1 option of LibRaw's dual licence,
   for three reasons:
   - Shipping Qt under LGPL-3.0 already obliges Lucent's distribution to use a replaceable shared
     library, provide library source, carry notices, and not forbid modification and reverse
     engineering for debugging such modifications. LGPL-2.1 for LibRaw adds no new kind of
     obligation, so one compliance process covers both libraries.
   - LGPL-2.1 is compatible with the GPL (its section 3 allows conversion to GPL-2.0 or later).
     CDDL-1.0 is generally regarded as GPL-incompatible. The LGPL choice keeps open the blueprint's
     option of a GPL-compatible distribution if a GPL reference algorithm is ever adopted.
   - The main advantage of CDDL-1.0 (file-level copyleft without relinking requirements) matters
     for static linking or proprietary modifications of LibRaw, and neither is planned.

   CDDL-1.0 remains available as an alternative. Switching requires a new ADR, for example if
   LibRaw ever has to be linked statically. Whatever the choice, the notices LibRaw carries for
   dcraw, DCB/FBDD, X3F and the Adobe DNG SDK code are reproduced.
3. **Qt under LGPL-3.0**, dynamically linked, using only modules offered under LGPL-3.0. Qt tools
   used at build or deployment time (for example `qsb`, `windeployqt` and the language server) are
   not shipped. A commercial Qt licence stays possible without code changes.
4. **Guarded options.** Features that change a component's licence are not enabled without a new
   ADR: the Little CMS `fastfloat` and `threaded` plugins (GPL-3.0-or-later) are prohibited, and
   FreeType is used under the FreeType License (FTL), which requires crediting the FreeType project
   in the product documentation. Any change to features in `vcpkg.json` includes a check of the
   port's feature-level licence field.
5. **What ships and what does not.**

   | Category | Components | Shipped? |
   |---|---|---|
   | Runtime DLLs | LibRaw and the DLLs it imports (libjpeg-turbo's `jpeg8.dll`, Little CMS, zlib), libtiff, SQLite, the export encoders' dependencies, Qt modules and plugins once the UI exists | Yes, as separate DLLs: exactly the set the deployment step selects, which the ledger tracks. JasPer is installed as a LibRaw build dependency, but the current LibRaw DLLs do not import it. libspng is imported only by `turbojpeg.dll` and ships only if Lucent uses the TurboJPEG API |
   | Compiled into Lucent binaries | Header-only nlohmann-json (MIT) and tl-expected (CC0-1.0) | Yes (the MIT notice must be reproduced) |
   | Windows runtime | Microsoft Visual C++ Redistributable (dynamic CRT) | Through Microsoft's official redistributable installer only (blueprint section 9) |
   | Test-only | GoogleTest and GoogleMock | No; test executables and their DLLs are never packaged |
   | Development tools | MSVC Build Tools, Windows SDK, CMake, Ninja, vcpkg and its helper ports (for example Meson), Python | No |
   | Research environment | Python packages such as rawpy (`research/README.md`) | No; not a runtime dependency |
   | Test fixtures | Camera files downloaded by `tools/fetch-fixtures.ps1` | No; never committed or shipped, used under the licence recorded per fixture |
   | GPL applications | RawTherapee, darktable, ART | No; see point 6 |

6. **GPL applications are visual references only.** They may be run as separate programs to produce
   comparison renderings. They are never linked, bundled, launched by Lucent at runtime, or used as
   a source of code: their source is not copied, translated or ported into Lucent. Adopting a
   GPL-licensed algorithm implementation (for example RawTherapee's AMaZE or RCD) requires a new ADR
   with an explicit distribution decision.
7. **Ledger and release artefacts.** `docs/legal/third-party-notices.md` is the development licence
   ledger. It is updated in the same change as any edit to `vcpkg.json`, the registry baseline or
   the triplet. The release notice bundle and the SBOM are generated from the files the deployment
   step actually ships (W5, W12), using vcpkg's per-port copyright files and SPDX documents,
   completed from upstream sources where those files are incomplete. The ledger records the known
   gaps.
8. **Models, datasets and weights** need their own rights review before distribution (blueprint
   Appendix C). A code licence never implies permission for weights or training data. This is
   decided per model when learned components arrive (W11).

## Consequences

- Lucent's own code keeps its licence, and the same binaries could be distributed under other terms
  without relicensing any third-party component, provided the LGPL obligations are met.
- Every release needs:
  - a notice bundle with the licence texts;
  - a corresponding-source provision for LibRaw and Qt (the upstream source archives, which vcpkg
    pins by SHA-512, plus the port patches and build flags);
  - end-user terms that allow replacing the LGPL DLLs and do not forbid reverse engineering for
    debugging such modifications.
- The installer deploys many DLLs, and the deployment step (W5) must be inspected, not trusted
  blindly.
- Using a GPL-3 demosaicer such as AMaZE requires either an independent implementation from a
  permissively licensed or published description, or a GPL-compatible distribution. Either way it
  needs a new ADR.
- Enabling a vcpkg feature is a licensing event as well as a build change.

## Alternatives considered

- **Static linking (for example `x64-windows-static-md`).** A single self-contained binary, but
  LGPL compliance would require distributing relinkable object files for Lucent. It also conflicts
  with the blueprint's route for LGPL Qt. Rejected.
- **LibRaw under CDDL-1.0.** Avoids LGPL-style relinking and reverse-engineering terms for LibRaw,
  but brings no practical benefit while Qt is used under LGPL-3.0 and would close the
  GPL-compatible option. Kept as the documented fallback, not chosen.
- **A commercial Qt licence.** Removes Qt's LGPL obligations at a recurring cost. Unnecessary while
  dynamic linking is acceptable. It remains possible later without code changes.
- **Embedding or porting RawTherapee code now.** Would require distributing Lucent under GPL-3.0.
  Rejected for now, as the blueprint directs, and revisited only through a new ADR.
- **Running a GPL application as a subprocess** (for example RawTherapee's command line) to get its
  demosaicing. The blueprint notes that a command-line boundary is not an automatic answer to
  licensing obligations. It would also bundle a full application. Rejected.

## Amendment: the release decisions (W12 licence and release phase, 2026-10-10)

The user decided how Lucent is distributed and how the LGPL libraries' source reaches recipients
([ADR 0024](0024-release-and-licensing.md)). For this ADR:

9. **Lucent is free and open source under the MIT licence and is not sold.** The linking model
   (decisions 1 to 6) is unchanged: LGPL libraries stay unmodified replaceable DLLs, except the two
   patches of Lucent's overlay ports for GLib and GNU libintl (ADR 0009, decision 17), which ship with
   the source. Lucent's MIT terms never restrict modifying the libraries or reverse engineering Lucent
   to debug such modifications, as LGPL-2.1 section 6 and LGPL-3.0 section 4 require of the terms of a
   combined work.
10. **The corresponding-source provision is an archive beside every installer**,
    `Lucent-<version>-corresponding-source.zip`, written and checked by `tools\package.ps1`: the
    upstream archives (verified by SHA-512 against vcpkg's record and the recipe's pin at the registry
    baseline), the recipes as built (registry ports and Lucent's overlay ports), the triplet, vcpkg's
    build records and a README on rebuilding and replacing each library. It covers every LGPL library
    that ships today: Qt (`qtbase`, `qtdeclarative`, `qtsvg`), LibRaw, Lensfun, GLib, GNU libintl and
    GNU libiconv. The "Consequences" item "a corresponding-source provision for LibRaw and Qt" is met
    by it.
11. **A copyleft licence cannot ship without a reviewed source provision.** Packaging stops when a
    shipped component's concluded licence, a library linked statically from it, or third-party code
    recorded inside it leaves no choice but a copyleft licence and `installer\license-policy.json`
    does not mark the port `lgpl` or `correspondingSource`. This extends decision 7: the ledger
    records each shipped component's obligations and their status for a release
    ([licence ledger](../legal/third-party-notices.md), "Release status").

## Amendment: modified libraries and the copyleft guard (W12 release review, 2026-10-15)

The review of the release phase found decision 9 inaccurate (LIC-1) and decision 11 open in two
directions (LIC-4).

**Modified libraries.** Decision 9 called the LGPL libraries unmodified except for Lucent's two
overlay patches, and the ledger marked GLib's and libintl's "changed files marked" as met. But the
vcpkg recipes Lucent builds with patch the upstream source too: GLib's `use-libiconv-on-windows.patch`
makes `glib/gconvert.c` convert with GNU libiconv instead of the bundled win_iconv (code of
`glib-2.0-0.dll`), libintl's `0003-Fix-win-unicode-paths.patch` changes `intl/loadmsgcat.c` and its
`uwp.patch` two more files, Qt's recipe patches mostly its CMake build files but also
`src/gui/text/qtextmarkdownimporter.cpp` (`md4c.diff`, in `Qt6Gui.dll`), and LibRaw's and Lensfun's
patch their CMake files. By the standard decision 17 of ADR 0009 applies to Lucent's own patches,
those libraries are modified, and LGPL-2.1 section 2 and GPL-3.0 section 5(a) (which LGPL-3.0
incorporates) ask for a notice of the changed files and the date. vcpkg's patch files cannot be edited
(vcpkg hashes the recipe), so:

12. **Every patch is listed with the library it modifies.** Packaging reads, for every library with a
    source provision, each patch file its recipe as built names (`Get-RecipeModifications`: a few
    apply only on other platforms or with features Lucent does not use, which the portfile says;
    listing them errs on the side of naming more), with the files it changes and who made it when:
    vcpkg's patches as they are in its registry at the baseline commit, dated by that commit; Lucent's
    own (`lucent-*.patch`, which also mark the files they change) with the date they state. The list
    is in `licenses\LGPL-NOTICE.txt` ("Modified"), in the corresponding-source archive's README and
    `MANIFEST.json` (`components[].modifications`), and a library without patches says "Modified:
    no". GNU libiconv's three patches change only the `iconv` program's helpers and an installed
    header, which `iconv-2.dll` does not compile, and are listed all the same. Decision 9's
    "unmodified" now reads "replaceable"; the ledger's "Met" for GLib and libintl rests on the list.

**The copyleft guard.** Decision 11's check took every alternative of an OR as one Lucent could
choose, so `LicenseRef-Vendor-Commercial OR LGPL-3.0-only` passed unmarked, although Lucent holds no
commercial licence; and its only remedy, the `lgpl` or `correspondingSource` mark, also admitted GPL
code and copyleft code compiled into the programs, which decisions 4 and 6 keep out without a new ADR.

13. **What an expression leaves Lucent to use decides** (`Get-LicenceKind`): a `LicenseRef-` or
    `DocumentRef-` alternative, or an identifier the packaging scripts do not know, is no alternative
    Lucent can take (an election is recorded as the policy's `concluded`); AND needs every operand, OR
    takes the best alternative Lucent can use.
14. **A source provision is not enough for GPL code or for copyleft code compiled in.** GPL or AGPL
    without an exception stops packaging even for a marked port, unless the port's policy entry names
    the ADR that accepted it (`strongCopyleftAdr`); a header-only port (compiled into the programs)
    under a copyleft licence stops it as a statically linked one does, a file-level copyleft (MPL, EPL,
    CDDL) only with a review recorded in `fileLevelCopyleftReview`. The error no longer offers the mark
    as the remedy for GPL code. Nothing that ships today is affected: the 0.1.0 package's archive and
    plan were made again with these rules without an error.
