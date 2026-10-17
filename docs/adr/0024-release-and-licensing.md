# ADR 0024: Release and licensing: free MIT distribution, a corresponding-source archive beside every installer, unsigned releases, and the release checklist

- **Status:** Accepted (amended 2026-10-15 by the W12 release review: "Amendment: the release review"
  below; amended 2026-10-16 by the gate-8 reading of the 0.1.0 package: "Amendment: the gate-8
  reading" below; amended again 2026-10-16 by that reading's review and its sweep: "Amendment: the
  gate-8 review, round 1" below; and by the review's second round: "Amendment: the gate-8 review,
  round 2" below)
- **Date:** 2026-10-10
- **Workflow:** W12 (Release hardening), licence and release phase
- **Amends:** ADR 0003 (the corresponding-source provision and the copyleft guard), ADR 0009
  (decision 10's "not for public distribution" is lifted; package manifest schema 5; the LGPL
  notice names the archive), ADR 0022 (a release published on an update channel needs its archive)

## Context

Since W5 the installer named the exact sources and recipes of the LGPL libraries it contains but
provided neither the sources nor a written offer, so it was "not for public distribution" (ADR 0009,
decision 10). LGPL-2.1 section 4 (LibRaw, GLib, GNU libintl, GNU libiconv) lets object code be
distributed if the complete corresponding machine-readable source accompanies it or "equivalent
access to copy the source code from the same place" is offered. LGPL-3.0 (Qt, Lensfun) is GPL-3.0
with additional permissions; conveying the libraries' object code falls under GPL-3.0 section 6,
whose option 6(d) is the same: offer the object code from a designated place and "equivalent access
to the Corresponding Source in the same way through the same place at no further charge", for as
long as the object code is offered (LGPL-3.0 section 4(d)(1) lets the application itself use the
libraries through a shared-library mechanism instead of shipping its own object code). Both
licences define the source as the library's source with the changes used and the scripts that
control its compilation and installation, which for Lucent are the vcpkg port recipes (portfile,
patches) of the registry baseline and Lucent's overlay ports for GLib and GNU libintl, whose patches
keep build-machine folders out of the DLLs (ADR 0009, decisions 17 and 18).

The user decided on 2026-10-10:

- Lucent is distributed **free and open source** under its MIT licence; it is not sold. The
  compiler Lucent's Setup is built with, Inno Setup 6.7.3, reports "Non-commercial use only"; the
  blueprint (section 9) asked for its terms to be reviewed before a public release.
- Each release ships **`Lucent-<version>-corresponding-source.zip`** next to its installer: the exact
  verified source archives, Lucent's patches and overlay ports, and the build recipes.
- Releases are **unsigned** for now; the signing pipeline (ADR 0009, decision 13) stays ready.
- The clean-machine gate runs in **Windows Sandbox**, which the user enables later; it is not
  available on the reference machine yet.

Facts on the reference machine (2026-10-10, the `release` preset in `build\rel-lic` at `f2faf0e` plus
this phase's changes): the package ships 23 vcpkg components, 8 of them LGPL (`gettext-libintl`,
`glib`, `lensfun`, `libiconv`, `libraw`, `qtbase`, `qtdeclarative`, `qtsvg`). Their 9 upstream archives
are in vcpkg's downloads folder; each one's SHA-512 is the value vcpkg recorded in the port's SPDX
document and appears in the port's files at the registry baseline (`git grep` in the pinned vcpkg
checkout); every recipe file vcpkg recorded (SHA-256 in the SPDX document) is byte-identical to the
vcpkg working tree or to Lucent's overlay port; the first part of each port's `triplet_abi`
(`vcpkg_abi_info.txt`) is the SHA-256 of `cmake/triplets/x64-windows-lucent.cmake`, and its
`post_portfile_include_0` that of `post-portfile/install-pdbs.cmake`. Help > About shows Lucent's
version and the lens database's CC BY-SA 3.0 credit but no copyright notice.

## Decision

1. **Distribution model.** Lucent is distributed free of charge as open-source software under its
   MIT licence and is not sold. `installer\license-policy.json` records this under "distribution"
   (`model`, `lucentLicence`, `commercial: false`, the corresponding-source provision, the signing
   status and the installer tool's terms); `tools\package.ps1` refuses to run without a complete
   record, prints it and copies it into the package manifest. Inno Setup is used under its
   non-commercial terms: packaging records the compiler's licence line (`toolchain.innoSetupLicence`)
   and refuses a non-commercial compiler when the policy says the distribution is commercial; the
   notices credit Inno Setup, whose own copyright notices stay in Setup and the uninstaller.

2. **The corresponding source is an archive beside the installer, not a written offer.**
   `tools\package.ps1` writes `Lucent-<version>-corresponding-source.zip` next to the installer in
   every run, `-StageOnly` included (`New-CorrespondingSourcePackage`,
   `installer\scripts\LucentPackaging.psm1`). Inside one folder `Lucent-<version>-corresponding-source/`
   it holds, for every shipped component whose licence needs a source provision:
   - `sources/<archive>`: each upstream source archive vcpkg built it from, taken from vcpkg's
     downloads folder. Its SHA-512 must equal the one vcpkg verified for the build (the port's SPDX
     document) and must be pinned in the recipe: at the registry baseline commit for a registry port
     (`git grep` of the baseline tree), in the overlay port for a port Lucent overlays (the registry
     port's pin is recorded too). Packaging never downloads; a missing archive stops it with the
     `vcpkg install --only-downloads` hint.
   - `ports/<port>/`: the recipe exactly as built. A registry port's folder must be the port at the
     baseline (the git tree vcpkg recorded is the baseline's), and every file vcpkg recorded must
     have the recorded SHA-256, with nothing else in the folder. An overlay port is compared with the
     registry's port of that name, file by file through git blob ids (unchanged, line endings only,
     changed, added, removed), so the README names Lucent's changes.
   - `build/<port>/vcpkg_abi_info.txt`: vcpkg's record of the build's inputs (features, recipe and
     helper-script hashes, tool versions). Its triplet and post-portfile hashes must be those of the
     files in `triplets/`.

   And for the archive as a whole: `triplets/` (the overlay triplet folder), `lucent/vcpkg.json` and
   `lucent/vcpkg-configuration.json` (registry baseline, features, overlay folders), the recipes of
   Lucent's other overlay ports that the build installed (the `gettext` build tool, whose programs
   GLib's build runs; its archive is the same as GNU libintl's), `MANIFEST.json` (every component with
   its licence, shipped files and what loads them, features, archives with download location, size,
   SHA-512, SHA-256 and pin, recipe files with SHA-256, Lucent's changes; and the build's other 30
   ports with the hashes of their sources, not included), `README.txt`
   (`installer\notices\CORRESPONDING-SOURCE-README.txt.in`: contents, the libraries, how Lucent
   loads each DLL, the exact `vcpkg install` command with every library's features to rebuild them,
   how to change a library and replace its DLL, where Lucent's own source is) and `SHA256SUMS.txt`.

3. **Which components: a copyleft licence needs a reviewed provision.** A component is included when
   the policy marks it `lgpl` (or `correspondingSource`, for another copyleft licence such as
   MPL-2.0). `Get-SourceProvisionPlan` stops packaging when a shipped component's concluded licence,
   a library linked statically from it or third-party code that Qt's SBOM records inside it leaves no
   choice but a copyleft licence (`Test-CopyleftLicence`: GPL, LGPL, AGPL, MPL, EPL, CDDL, EUPL, CC
   BY-SA and similar; AND needs one copyleft operand, OR all of them, so FreeType's "FTL OR
   GPL-2.0-or-later" does not) and the policy does not mark it. The Public Suffix List inside Qt
   (MPL-2.0) is covered by qtbase's archive. (Corrected by the gate-8 review, round 2, decision 37:
   qtbase's archive holds only the array Qt generated from the list; its Source Code Form,
   `public_suffix_list.dat` at the commit Qt's SBOM records, is named in the notices and the SBOM.)

4. **The archive is checked independently and is deterministic.** After writing it, packaging reads
   it back with `System.IO.Compression` (not the writer's code; `Test-CorrespondingSourcePackage`)
   and requires: every expected component listed in `MANIFEST.json`, each of its source archives
   present by SHA-512, each recipe file by SHA-256, its build record; `SHA256SUMS.txt` complete and
   right; entries sorted, unique, stored, dated 1980-02-01 00:00. A failure deletes the archive and
   stops packaging. The writer (`New-DeterministicZip`, a small C# ZIP writer compiled by `Add-Type`)
   stores every entry (method 0; the large members are compressed archives already), sorted by name,
   with a fixed time, no extra fields, attributes or data descriptors, so the same inputs give the
   same bytes on any machine. Lucent's own source is not copied: the README and the LGPL notice name
   the repository and commit (it is MIT and public; Lucent loads the libraries dynamically, so
   relinking needs no Lucent object code).

5. **The installation names the archive.** `licenses\LGPL-NOTICE.txt` is written after the archive
   (`Write-LgplNotice`) and names it with its size and SHA-256, says it is published next to the
   installer from the same place for as long as the installer is offered, and lists for every
   library where its archive and recipe are inside it. `THIRD-PARTY-NOTICES.txt` mentions it too.
   The archive's own files (everything but the upstream archives) are scanned for the build
   machine's folders like the installation folder (ADR 0009, decision 12).

6. **The package manifest records it** (schema 5): `correspondingSource` (file, size, SHA-256, the
   components with their archives' SHA-512 and SHA-256 and recipe files, the build tools, every file
   of the archive), `distribution`, `components[].correspondingSource`, `toolchain.innoSetupLicence`.
   `installer\tests\Test-SourcePackage.ps1 -PackageManifest <manifest>` checks a published archive
   against its manifest without the build tree (the release checklist's gate 7); without arguments it
   tests the functions on synthetic data (CTest `lucent_source_package_tests`).

7. **Publishing.** The installer, its corresponding-source archive and its package manifest are
   published together, from the same place, and the archive stays available for as long as the
   installer is; every release is archived permanently (`docs\release\checklist.md`).
   `New-LucentReleaseManifest` (ADR 0022) refuses a package whose manifest records no archive, or
   whose archive is not beside the installer with the recorded size and SHA-256 (`-AllowUnsignedInstaller`,
   for tests and rehearsals only, accepts a package that records none). Lucent's own source is pushed
   with the release tag before publishing, since the notices point at it.

8. **Releases are unsigned until a certificate exists.** The policy says `signing.release:
   unsigned`; the signing hooks and their verification stay as decided (ADR 0009, decision 13).
   Once the policy says `signed`, packaging refuses a Setup without a release signature (and warns
   when a release-signed package's policy still says `unsigned`). An
   unsigned package cannot be published on an update channel (`New-LucentReleaseManifest` requires a
   timestamped release signature), so unsigned releases are packaged with `-UpdateChannel none` and
   users update by installing the next Setup over the old one. The release notes say the installer
   is unsigned and give its SHA-256.

9. **Lucent's version is written twice and must agree.** `tools\package.ps1` refuses a build whose
   `vcpkg.json` `version-semver` differs from the CMake project version (the archive ships
   `vcpkg.json`). The checklist states the SemVer rules for releases.

10. **The clean-machine gate runs in Windows Sandbox** ([docs/release/clean-machine.md](../release/clean-machine.md);
    the harness as completed later in this phase, ADR 0009's amendment "the release phase's deferred
    fixes"): `tools\Test-CleanMachine.ps1` copies Setup, its manifest, its version rehearsal, a few
    smoke fixtures and the gate's scripts into a folder mapped read-only, with a writable results
    folder, writes a `.wsb` with networking, the GPU, the clipboard and printers off, validates it
    against `installer\sandbox\wsb-configuration.xsd` (the documented elements and values), starts the
    sandbox and waits for its result with a timeout. At logon `installer\sandbox\Invoke-CleanMachineGate.ps1`
    records that the machine is clean and offline, installs silently (with the real, elevated
    redistributable), runs the installed `Lucent.exe` to import, run Auto, export JPEG and 16-bit TIFF
    and take screenshots, uninstalls, runs `Test-InstalledPackage.ps1 -RuntimeOutcomes -NewerInstaller`
    and writes `gate.json`, which the host checks itself. Manual steps follow (a declined elevation
    prompt, the wizard, an import, edit and export through the user interface). Until the gate passes
    for a release, the release is not published.

11. **Data and notices.** The Lensfun lens database is credited (title, authors, home page, licence)
    in `THIRD-PARTY-NOTICES.txt` and in Help > About, its CC BY-SA 3.0 text ships twice, its files are
    unmodified with their provenance. Lucent's check of the database against its pinned manifest only
    decides whether Lucent uses it: the files stay plain XML that anyone may copy, change and
    redistribute, so it is not a technological measure restricting the licence's rights. Help >
    About shows no copyright notice, so the LGPL clauses about programs that display copyright notices
    (LGPL-2.1 section 6, LGPL-3.0 section 4(c)) do not apply; if it ever shows one, it must also show
    the libraries' notices and point to `licenses\`.

## Verification (2026-10-10)

On the reference machine, `tools\package.ps1 -BuildDir build\rel-lic -SkipBuild -StageOnly` (release
preset, commit `f2faf0e` with this phase's uncommitted changes):

- The archive: **132,443,744 bytes (126.3 MiB)**, 110 files, SHA-256
  `0156cf4a58ddad478f1c0a4a0bcd818de8c2fc87d3a43cc2d991c4e110754c1c`: the 9 archives of the 8 LGPL
  libraries (qtbase 48.3 MiB, qtdeclarative 36.9 MiB, gettext 25.6 MiB, libiconv 5.6 MiB, glib
  5.6 MiB, qtsvg 2.2 MiB, lensfun 1.0 MiB, LibRaw 0.6 MiB and LibRaw-cmake 14 KiB), 86 recipe files of
  9 ports (the `gettext` build tool's included), 2 triplet files, 8 build records, Lucent's 2 vcpkg
  files and the 3 generated files. Three separate runs (two `-StageOnly`, one that also compiled
  Setup) wrote byte-identical archives and LGPL notices. The archive step adds about 8 seconds
  (58 s for the whole `-StageOnly` run instead of 50 s).
- The run with Setup (`-UpdateChannel none`, unsigned): a 41.7 MiB installer (43,679,225 bytes);
  the package manifest (schema 5) records `toolchain.innoSetupLicence` "Non-commercial use only",
  the distribution decision and the archive with its 110 files; the builder-path scan covered 493
  files (installation folder, the archive's own files, Setup, the manifest) without a hit.
- The README names Lucent's changes as computed: `gettext-libintl`: `lucent-no-builder-paths.patch`
  added, `portfile.cmake` changed, `usage` line endings only; `glib`: `lucent-neutral-locale-dir.patch`
  added, `portfile.cmake` changed; every registry port unchanged at the baseline.
- Read back with `System.IO.Compression`, every archive and recipe file matched; the builder-path
  scan found nothing in the archive's 101 own files (385 KiB) or the installation folder's 391 files.
  `Test-SourcePackage.ps1 -PackageManifest` passed its 139 checks against the package manifest.
- On the same build data, with the policy's `lgpl` mark removed from `lensfun`, packaging stops
  ("Port lensfun 0.3.4 ships under a copyleft licence (its licence LGPL-3.0-only), but
  installer\license-policy.json does not provide its corresponding source"); an archive made
  without `lensfun` fails the read-back check with six errors (not listed, archive, 3 recipe files,
  build record).
- `Test-SourcePackage.ps1` (94 checks), `Test-ReleaseManifest.ps1` (41, with the new archive checks),
  `Test-UpdateChannelPolicy.ps1` (32) and `Test-BuilderPathScan.ps1` (54) pass. The sandbox scripts
  were rehearsed on the host (the configuration is well-formed XML; the environment record reports the
  host as not clean: runtime registered, Python and Git on `PATH`, network available); the gate
  itself waits for Windows Sandbox.
- `tools\build.ps1 -Preset dev -BuildDir build\dev -Jobs 6 -Test`: 2,289 tests, 2,226 passed, 57
  fixture tests skipped by their tier, 6 disabled, none failed (`lucent_source_package_tests` is the
  new one). A first run, while the machine was also packaging, failed one timing bound
  (`WorkerProcess.CancelMidRenderStopsTheJobAndKeepsTheWorker`: 5.3 s against 2.5 s); that test passed
  alone and in the full run that followed.

## Consequences

- An installer can now be published: with its archive, after the gates of `docs\release\checklist.md`.
  The clean-machine gate (Windows Sandbox) is still open, so the first public release waits for it.
- Each release publishes about 126 MiB of source next to its installer, most of it Qt's. A
  dependency upgrade changes the archive automatically; a new copyleft dependency cannot ship until the
  policy provides for it.
- The archive depends on vcpkg's downloads folder holding the archives: a machine that restored the
  ports only from the binary cache must fetch them first (`vcpkg install --only-downloads`, which
  contacts only the pinned upstream locations).
- `MANIFEST.json` lists the build's permissive dependencies with the hashes of their sources, so the
  libraries can be rebuilt exactly, but their archives are not included: they are not under a source
  obligation, and the vcpkg registry at the baseline names them.
- The package manifest's schema is 5; tools that read schema 4 only (none outside this repository)
  must accept it. `New-LucentReleaseManifest` refuses schema-4 packages for release.
- Unsigned installers draw SmartScreen warnings and cannot use "Check for updates"; both end when a
  certificate exists (checklist steps 11 and 12).

## Alternatives considered

- **A written offer** (LGPL-2.1 section 6(c), GPL-3.0 section 6(b)): a promise, valid for three
  years, to send the source on request. It needs someone to answer requests for years and leaves
  users without the source when they need it; an archive beside the installer costs only storage.
  Rejected.
- **Pointing at the upstream projects and the vcpkg registry** instead of shipping the archives. The
  LGPL requires the source to be provided by the distributor (from the same place); third-party
  servers may change or disappear, and Lucent patches two of the libraries. Rejected; the archive
  keeps the upstream locations as information.
- **Shipping the sources of every dependency**, permissive ones included: 198 MiB more for this
  build's 30 other ports (108.6 MiB of it the OpenGL registry), without the tools vcpkg downloads.
  Not required by their licences; their hashes are recorded instead.
- **A copy of Lucent's own source in the archive.** Lucent is MIT and published; the libraries are
  loaded dynamically, so rebuilding or replacing them never needs Lucent's code. A pointer (repository
  and commit) avoids publishing the same source twice.
- **A compressed (deflate) archive.** Saves almost nothing (the members are `.tar.gz` and `.tar.xz`)
  and makes the bytes depend on the compressor's implementation and version. Stored entries keep the
  archive reproducible anywhere.
- **`System.IO.Compression` as the writer.** Its .NET Framework version always deflates, even at
  `NoCompression` (method 8, measured on the reference machine), and writes what the runtime chooses;
  a writer of stored entries of about a hundred lines is fully specified. The reader stays
  `System.IO.Compression`, so writer and checker are independent.
- **A commercial Inno Setup licence.** Not needed for a free, non-commercial distribution; the
  policy's `commercial` flag and the compiler's licence line make a future change of model fail
  loudly instead of silently.
- **Signing now with a self-signed certificate.** Gives users no trust (SmartScreen and Windows do not
  trust it) and would teach them to accept untrusted signatures; test signatures stay a rehearsal only
  (ADR 0009, decision 14).

## Amendment: the release review (W12, 2026-10-15)

The review of the release phase found the notices short of the facts and four checks weaker than
this ADR says (findings LIC-1 and LIC-3 to LIC-7; the gate's findings are in ADR 0009's amendment of
the same date):

12. **The copyleft guard** (decision 3; LIC-4): an OR alternative under a `LicenseRef-` licence (a
    vendor's commercial terms, which Lucent does not hold) or an unknown identifier is no choice,
    GPL or AGPL code without an exception stops packaging even when the policy provides its source,
    and so does copyleft code compiled into the programs (`Get-LicenceKind`; ADR 0003's amendment
    of this date, decisions 13 and 14). The error no longer offers the policy's mark as the remedy
    for GPL code.
13. **The read-back check stands on its own** (decision 4; LIC-3). A `MANIFEST.json` that is valid
    JSON but no object (`null`, `[]`, `0`, `false`, `""`) skipped every per-component check, and `{}`
    stopped the check with a strict-mode exception (which also left the archive on disk). Now anything
    but a JSON object is an error ("MANIFEST.json is not a JSON object"), members are read
    strict-mode safely, and each expected library's source archives, recipe files and build record
    are checked whatever `MANIFEST.json` holds; only "listed in MANIFEST.json" needs it.
14. **The same bytes on any machine, whatever its regional format** (decision 4; LIC-7). The orders
    that reach `MANIFEST.json` (the components, their recipe files and loading, the overlay
    comparison, the triplet files, `notIncluded`), the README and the notices came from
    `Sort-Object`, which compares by the thread's culture: under Estonian, which sorts z between s and
    t, `notIncluded` (which holds `tiff` and `zlib`) and therefore the archive's SHA-256 differed.
    Every such order is now ordinal (`ConvertTo-OrdinalOrder`); the ZIP entries and `SHA256SUMS.txt`
    were already. The other files packaging writes for publication no longer depend on it either:
    `LGPL-NOTICE.txt` wrote the archive's size as "132 480 210" under Estonian (it is grouped with
    commas everywhere now), and the package manifest's file list and the SBOM's files are in ordinal
    order. (The SBOM's creation time and namespace differ on every run, as SPDX intends.)
15. **The archive's README says which library code is inside the programs** (decision 2; LIC-5). It
    said that no code of the libraries is linked into `Lucent.exe` or `lucent-worker.exe`, but qtbase's
    Windows entry point (`Qt6EntryPoint.lib`, BSD-3-Clause) is linked into `Lucent.exe`. The README
    now states it from the build (`@STATIC_CODE@`: each statically linked library with its port, the
    programs that link it and its licence), and `Get-StaticVcpkgLibraries` records which program links
    each library, so `LGPL-NOTICE.txt`, `THIRD-PARTY-NOTICES.txt` and the SBOM name `Lucent.exe` alone
    instead of both programs.
16. **The notices list every modification** (decision 5; LIC-1): `LGPL-NOTICE.txt` and the archive's
    README give each library a "Modified" entry with every patch its recipe names, the files it
    changes, who made it and when, and `MANIFEST.json` records them (`components[].modifications`;
    ADR 0003's amendment, decision 12).
17. **A commercial distribution needs a compiler that states a commercial licence** ("Alternatives
    considered", "A commercial Inno Setup licence"; LIC-6). The licence line was read only from the
    line after "Compiler engine version", and an unrecognised banner left it empty, which passed
    whatever the policy said. `Get-InnoSetupLicence` reads the whole banner (a non-commercial line
    first); `Test-InnoSetupLicence` refuses a commercial distribution unless the banner positively
    states a commercial licence, so a non-commercial, reworded or translated banner fails loudly, and
    warns for the free distribution when it recognises no licence line (the package manifest then
    records none).

### Verification (2026-10-15)

- **The defects, shown with the committed module** (334d0bb): `Test-CopyleftLicence` returned false
  for `LicenseRef-Qt-Commercial OR LGPL-3.0-only` and `LicenseRef-Qt-Commercial OR GPL-3.0-only`; a
  GPL DLL marked `correspondingSource` passed the plan without an error; under `et-EE` the plan's
  order was `sqlite3|zlib|tiff`.
- **`Test-SourcePackage.ps1`** (CTest `lucent_source_package_tests`): 177 checks (94 at this ADR's
  date). New: the licence kinds of 16 expressions; a `LicenseRef-` alternative leaves the copyleft
  one; a GPL DLL marked `correspondingSource` is refused unless its entry names an ADR, LGPL code
  compiled in is refused marked or not, file-level copyleft compiled in passes only with its review,
  GPL code inside a marked port is refused, and the mark is not offered as the remedy; a
  `MANIFEST.json` of `null`, `[]`, `0`, `false`, `""`, an empty file or `{}` is checked without an
  exception, refused, and the missing source archive or recipe file is still found; ordinal orders
  (the plan, records by a property, `-Unique`) are the same under `en-US` and `et-EE`, where
  `Sort-Object` differs, and so are `LGPL-NOTICE.txt` and the package manifest's file list (both
  differed with the committed code); a patch's files, the three patches of a test recipe with who
  made them and when, "Modified: no"; the README's statement of the linked code; five compiler
  banners (6.7.3's, the licence line elsewhere, a commercial one, none, a translated one) against a
  commercial and a free distribution. `Test-ReleaseManifest.ps1` (41) and
  `Test-UpdateChannelPolicy.ps1` (32) pass unchanged.
- **The package** (`tools\package.ps1 -BuildDir build\wl-fix-scripts-rel -Jobs 5 -UpdateChannel none
  -StageOnly`, the release preset with this review's changes uncommitted): the archive is
  **132,480,210 bytes**, 110 files, SHA-256
  `3c50c75d06f3b7c26d7de10a5907d76b89bf5cfab5c2e8db70c3a487edb7e082` (36,466 bytes more than on
  2026-10-10: the "Modified" lines and records); `MANIFEST.json` lists 26 patches for qtbase, three
  each for glib, gettext-libintl and libiconv, two for libraw, one for lensfun and none for
  qtdeclarative and qtsvg; `LGPL-NOTICE.txt` and the README say the same, and the README names
  `Qt6EntryPoint.lib`, linked into `Lucent.exe` under BSD-3-Clause. Four runs, two of them with the
  thread's regional format set to Estonian (`et-EE`, `-SkipBuild -OutputDir`), wrote archives with
  the same SHA-256. The first Estonian run also showed `LGPL-NOTICE.txt` and the order of the package
  manifest's and the SBOM's files changing with the regional format; after their fix the Estonian
  run's installation folder differed from the English one only in the SBOM's creation time and
  namespace, and its package manifest only in its own creation time and the SBOM's SHA-256.
  `Test-SourcePackage.ps1 -PackageManifest` (the release checklist's gate 7) passed against both
  package manifests (139 checks each).

## Amendment: the gate-8 reading (2026-10-16)

The release checklist's gate 8 (licence review) was read on the 0.1.0 package of 2026-10-16
(`build\rel-0.1.0\package`) and found eleven discrepancies (D1 to D11) between the shipped notices, the
SBOM, the corresponding-source archive, `installer\license-policy.json` and the ledger. Most were facts
packaging could have derived or checked, so they are now generated and checked:

18. **A library's parts under their own licences ship their terms** (D1, D2). LibRaw's COPYRIGHT names
    parts under licences of their own but contains none of their terms. Read from the verified source
    archive (`LibRaw-LibRaw-0.22.2.tar.gz`, whose SHA-512 is the one the port pins at the registry
    baseline, vcpkg verified and the corresponding-source archive holds), they are: the DCB demosaic
    and FBDD denoise code (`src/demosaic/dcb_demosaic.cpp`; "BSD-like 3-clause", whose text is
    BSD-3-Clause) and code from the Adobe DNG SDK 1.4 (MIT, Copyright (c) 2005 Adobe Systems
    Incorporated; COPYRIGHT names the licence, and the archive holds no text of it), both compiled into
    `raw_r.dll`; and the X3F tools (`internal/x3f_tools.h`, `src/x3f`; "BSD-style", whose text is
    BSD-3-Clause with Roland Karlsson's name in its disclaimer), which the port does not compile
    (LibRaw-cmake's `ENABLE_X3FTOOLS` stays off, its default, so `USE_X3FTOOLS` is undefined and the X3F
    sources compile to nothing). LibRaw's concluded licence `LGPL-2.1-only AND BSD-3-Clause` left out the
    MIT code; it is now `LGPL-2.1-only AND BSD-3-Clause AND MIT`, and the policy's new `declared` records
    what LibRaw offers, `(LGPL-2.1-only OR CDDL-1.0) AND BSD-3-Clause AND MIT` (vcpkg's port declares
    `BSD-3-Clause AND (LGPL-2.1-only OR CDDL-1.0)`). The policy's new `parts` name each part with its
    copyright line, licence, the archive member and the first and last line of its terms there;
    packaging takes those lines verbatim from the verified archive (`Find-SourceArchiveMember`, which
    checks the SHA-512 first, and `Get-LicenceExcerpt`, whose first line must be unique) into
    `licenses\libraw\<part>.txt`, with the member, the lines and the archive's SHA-512, as it already
    did for `README.ijg` and PCRE2's `LICENCE.md`. For the Adobe code the MIT License's text follows,
    with the copyright line COPYRIGHT gives (the policy's `terms`, the MIT License's words; the
    self-test compares them with Lucent's `LICENSE`). (Withdrawn by the gate-8 review, round 2,
    decision 34: no licence text is written by Lucent; the Adobe part names the MIT texts of
    nlohmann-json's licence files from its verified archive.) `THIRD-PARTY-NOTICES.txt` lists the parts with
    LibRaw, and the SBOM adds the two compiled ones as packages inside LibRaw's binary package.
    Packaging stops when a compiled part's licence is missing from the concluded expression
    (`Test-PartLicences`), or a part's member or lines are not found. The copyleft guard is unchanged:
    the expression leaves no choice but LGPL, and LibRaw keeps its source provision.
19. **The notices name the versions the package manifest records** (D3, D4): the Visual C++
    redistributable with `vcRedistributable.version` and Inno Setup with `toolchain.innoSetupVersion`.
    ISCC states its engine version only when it compiles, and `THIRD-PARTY-NOTICES.txt` is written
    before Setup, so packaging reads the version first by compiling an empty script with its output off
    in a temporary folder (`Get-InnoSetupEngine`); compiling Setup must then report the same version,
    or Setup is deleted. A `-StageOnly` run uses a compiler it finds and records it; without one the
    notices name Inno Setup without a version, the manifest records none, and packaging warns.
20. **Every bundled data asset is named** (D5). The "Data" section lists Lucent's own assets too, under
    Lucent's MIT License (the camera support database; their names in the policy's new
    `lucentAssets`).
21. **vcpkg's licence ships where its recipes ship** (D9). vcpkg's recipes are MIT (Copyright (c)
    Microsoft Corporation) and ship in the installation (the overlay recipes of GLib and GNU libintl,
    `licenses\<port>\recipe`, 14 files) and in the corresponding-source archive (every recipe), but its
    licence shipped in neither. `Get-VcpkgLicence` takes `LICENSE.txt` of the pinned registry checkout,
    verified as the baseline's by its git blob id (a checkout with other line endings gives the blob's
    bytes, so the archive stays the same on every machine). Packaging writes it as
    `licenses\vcpkg-LICENSE.txt` when a recipe ships, and into the archive as
    `licenses/vcpkg-LICENSE.txt` beside `licenses/Lucent-LICENSE.txt` (the archive also holds Lucent's
    overlay ports, triplet and vcpkg manifest, whose MIT text it lacked too). The read-back check
    requires both, `MANIFEST.json` records them (`vcpkg.licenceFile` with the blob, `lucent.licenceFile`),
    and `THIRD-PARTY-NOTICES.txt`, `LGPL-NOTICE.txt` and the archive's README say which files each
    covers.
22. **The SBOM concludes a licence for every shipped recipe file and for Lucent's own files** (D9,
    D10), from each recipe file's status against vcpkg's port at the baseline (`Compare-OverlayPort`,
    `Get-RecipeFileLicence`): vcpkg's files MIT with Microsoft's copyright line; the two portfiles Lucent
    changed MIT with both copyright lines, because they are vcpkg's MIT files with Lucent's changes,
    which are under Lucent's MIT License (the MIT License lets modified copies be distributed under its
    notice, and both licensors' notices ship); every patch (vcpkg's four, Lucent's two)
    `MIT AND LGPL-2.1-or-later`, because besides its author's lines it quotes and changes lines of GLib or
    GNU libintl, which stay under the library's licence. `qt.conf` and `Lucent-files.txt` are Lucent's own,
    MIT (`Get-OwnFileLicence`); only the licence texts and notices remain NOASSERTION among Lucent's
    own files (the port packages' 155 recipe files and the 280 third-party binaries were NOASSERTION
    too: corrected by decision 31). (Since the gate-8 review, round 2, decision 36, a patch's licence
    is derived from the files it changes; GLib's and GNU libintl's patches remain
    `MIT AND LGPL-2.1-or-later`, because the files they change are under that licence.)
23. **A library's note names every patch that changes more than build files** (D6, D8). The notes of
    the libraries with a source provision, which `THIRD-PARTY-NOTICES.txt` repeats, must name each patch
    that changes a file other than a CMake, Meson, autotools or pkg-config file, by its name or by every
    such file (`Test-ModificationNotes`, on the modifications the corresponding-source archive
    records); packaging stops otherwise.
    qtbase's note summed up its 26 patches as CMake build files, `md4c.diff` and header patches for
    clang-cl or static builds; it now also names `silence-winrtbase-coroutine-warnings.diff` (a `#define`
    silencing a deprecation warning in `qt_winrtbase_p.h`, compiled because this build configures
    C++/WinRT: the only one of the four that reaches the Windows binaries), `use_inotify_on_freebsd.patch`
    (a FreeBSD-only condition), `macdeployqt-symlinks.patch` (macdeployqt, not built on Windows) and
    `windeployqt-webengine-debug-resources.patch` (windeployqt, which the build runs but does not ship).
    libiconv's note now names `lib/compat.c` among `iconv-2.dll`'s sources (with
    `woe32dll/iconv-exports.c` and the version resource, from `lib/Makefile.in` and the build log).
24. **The ledger** (D6, D7, D10, D11) is corrected: the rows of Qt Base, LibRaw and GNU libiconv as
    above; the LibRaw section, which counted the `LIBRAW_NODLL` edit of the installed `libraw_types.h`
    (a `vcpkg_replace_string` after the build, which changes no source of `raw_r.dll` and nothing that
    ships) among the port's patches; Lucent's row, which now lists `Lucent-files.txt` and the licence
    bundle; and nlohmann-json's, compiled into `lucent-worker.exe` too. A row records vcpkg's recipes.

`installer\tests\Test-SourcePackage.ps1` guards each item: its self-test (section 11) covers the
functions above on synthetic data and the policy's LibRaw, qtbase and libiconv entries, and with
`-PackageManifest` it checks a package's archive for both licences and the notes for the patches
changing code, and, when the installation folder is beside the manifest, that the notices name the
recorded versions and every asset, that each part's terms ship and its licence is in the expression,
that vcpkg's licence is beside the shipped recipes, and that the SBOM concludes a licence for the
recipe files, `qt.conf` and `Lucent-files.txt` and gives each component's packages its licence.

### Verification (2026-10-16)

- **The facts behind the fixes.** `LibRaw-LibRaw-0.22.2.tar.gz` in vcpkg's downloads folder has the
  SHA-512 that `ports/libraw/portfile.cmake` pins at `e143402b` and the 0.1.0 archive records
  (`24313fbd...5f4`). Its `COPYRIGHT` names the parts and their licences; `src/demosaic/dcb_demosaic.cpp`
  (lines 2 to 32) holds the BSD-3-Clause terms of DCB and FBDD and, below them, their authors;
  `internal/x3f_tools.h` (lines 15 to 43) the X3F terms, the same as in `src/x3f`; no file of the
  archive holds an MIT permission notice. vcpkg's build log of the port (its ABI hash is this build's)
  shows `ENABLE_X3FTOOLS:BOOL=OFF` and "compiled with Foveon X3F support ... NO". Qt's build defines
  `QT_FEATURE_cpp_winrt 1`. libiconv's build log compiles `lib/iconv.c`, `lib/compat.c`,
  `libcharset/lib/localcharset.c`, `woe32dll/iconv-exports.c` and `windows/libiconv.rc` into the DLL.
  vcpkg's `LICENSE.txt` in the pinned checkout is the baseline's blob `4d23e0e3...`.
- **The package:** `tools\package.ps1 -BuildDir build\rel-0.1.0-lic -Jobs 6 -UpdateChannel none
  -StageOnly`, a fresh `release` build of `972f349` with these changes uncommitted (the compiler found
  through the uninstall registry). Exit code 0. LibRaw is `LGPL-2.1-only AND BSD-3-Clause AND MIT` in the
  notices, the package manifest and the SBOM's port and binary packages; `licenses\libraw` holds
  `DCB-FBDD-BSD-3-Clause.txt`, `Adobe-DNG-SDK-MIT.txt` and `X3F-BSD-3-Clause.txt`;
  `THIRD-PARTY-NOTICES.txt` names "Microsoft Visual C++ Redistributable (x64) 14.44.35211.0" and
  "Inno Setup 6.7.3" (the manifest's records), the camera support database and the recipes' licences;
  `licenses\vcpkg-LICENSE.txt` is the baseline's `LICENSE.txt` (with CRLF line endings, as every text
  there). The installation holds 395 files (391 before). The SBOM, 109 packages and 549 files (107 and
  545 before), concludes MIT for 8 recipe files, `MIT AND LGPL-2.1-or-later` for the 6 patches and MIT
  for `qt.conf` and `Lucent-files.txt`; NOASSERTION remains for the 35 licence texts and notices only
  (47 files before). pyspdxtools (spdx-tools 0.8.5) validated it. Packaging checked 38 patches against
  the notes. The archive is 132,484,138 bytes, 112 files (the two licences added), SHA-256
  `9642187f6953f053ed56018ff5f6f2b08e85deffe99c9fb3de6f0accf713f35d`, the same in a second run and
  from the existing 0.1.0 build tree; the builder-path scan found nothing in 499 files.
- A Setup compiled from the same tree (`-SkipBuild -UpdateChannel none`, into
  `build\rel-0.1.0-lic\package-setup`, not installed) reported the engine version the notices name,
  which its manifest records with the licence line.
- **`Test-SourcePackage.ps1`:** the self-test passes 260 checks (177 before). With `-PackageManifest`
  it passes 155 checks on the new package and on the Setup package, and on the 0.1.0 package of the
  reading it fails 11 of 153: the archive without vcpkg's and Lucent's licences, D3, D4, D5, D1, the
  three part texts (D2), vcpkg's licence beside the recipes (D9) and the SBOM's NOASSERTION for the
  recipe files, `qt.conf` and `Lucent-files.txt` (D9, D10). `Test-ReleaseManifest.ps1` (41),
  `Test-UpdateChannelPolicy.ps1` (32) and `Test-BuilderPathScan.ps1` (81) pass unchanged.

## Amendment: the gate-8 review, round 1 (2026-10-16)

A review of the gate-8 reading's fixes found fourteen more discrepancies (R1 to R14) and asked for a
sweep of every shipped component (the 23 of the release table, the Visual C++ runtime, and Inno
Setup's loader and uninstaller) for code compiled into what ships under a licence or notice of its
own. The sweep read each component's source archive, verified by the SHA-512 its port pins, and
checked each such file against what this build compiles: vcpkg's build logs of the ports (their ABI
hashes are this build's), the configure summaries, Qt's own SBOMs and the binaries. The ledger's new
section, "Code under licences of its own inside the shipped files", lists what was found, with the
code that is present but not compiled and the reason. Decision 18 now covers every component, and
what it found is generated and checked:

25. **Every compiled part is in its component's licence, notices and SBOM** (R1 to R5 and the sweep).
    The policy's `parts`, each notice verbatim from the verified archive:
    - FreeType: the BDF and PCF drivers and `src/base/fthash.c` (MIT), and the PCF driver's
      `pcfutil.c` (MIT-open-group). FreeType is now `FTL AND MIT AND MIT-open-group`, declared
      `(FTL OR GPL-2.0-or-later) AND MIT AND MIT-open-group`.
    - PCRE2: its just-in-time compiler, sljit (BSD-2-Clause), from the port's second archive
      (`zherczeg/sljit`; a part names the archive that holds it, `source`). PCRE2 is
      `BSD-3-Clause WITH PCRE2-exception AND BSD-2-Clause`.
    - nlohmann-json: Bjoern Hoehrmann's UTF-8 decoder and Florian Loitsch's Grisu2 (MIT, as the
      README states, with `LICENSES/MIT.txt`), and Hedley (CC0-1.0, as the README and Hedley's own
      header state; the 3.12.0 copy's SPDX line says MIT with Evan Nemerson's copyright, which the
      MIT text covers). nlohmann-json is `MIT AND CC0-1.0`. The parts of Google Abseil are not
      compiled: they are the `#else` branch of `JSON_HAS_CPP_14`, which C++20 defines.
    - Lensfun: the TRE subset of `libs/regex` (LGPL-2.1-or-later), compiled as `tre_regex` because
      MSVC has no `regex.h`. Lensfun is `LGPL-3.0-only AND LGPL-2.1-or-later`.
    - libtiff: the LZW code (the Berkeley notice in `LICENSE.md`, `BSD-4.3TAHOE`), the PixarLog,
      LogLuv and CCITT code (the libtiff licence's form with other holders), and the hash set (MIT).
      libtiff is `libtiff AND BSD-4.3TAHOE AND MIT`.
    - HarfBuzz: UCDN (ISC), fasthash and Microsoft's Universal Shaping Engine data (MIT), and the
      Unicode data of its generated tables (Unicode-3.0). HarfBuzz is
      `MIT-Modern-Variant AND ISC AND MIT AND Unicode-3.0`.
    - libjpeg-turbo: its SIMD code (Zlib), in `jpeg8.dll`. libjpeg-turbo is
      `BSD-3-Clause AND IJG AND Zlib`.
    - GNU libintl: GNU Bison's parser skeleton in `intl/plural.c`, under
      `GPL-3.0-or-later WITH Bison-exception-2.2`, with the GPL's text from the archive. libintl is
      `LGPL-2.1-or-later AND GPL-3.0-or-later WITH Bison-exception-2.2`.

    A part's licence may carry its exception. The licence's text comes from a member of the same
    archive (`text`, with `about` when the excerpt is something else, such as Frank Cringle's
    copyright in libtiff's fax decoder), from another component's licence file that holds it
    (`textIn`, with what the file holds, checked once every file is written), or, where the archive
    names a licence without its text, from the licence's standard words (`terms`: only the Adobe
    code's MIT text; withdrawn by the gate-8 review, round 2, decision 34, and refused since). Each
    part's file says which (R11). A part has its own `version` or none (R13:
    the SBOM gave LibRaw's parts LibRaw's version). `libraryLicence` is a library's own licence
    without its parts, which the licences of its recipe's patches use. The copyleft guard (decisions 3
    and 12) is unchanged and reads the new expressions: the Bison skeleton's GPL carries its exception,
    and libintl and Lensfun keep their source provision.
26. **Qt's licences cover the third-party code Qt compiles in** (the sweep). Qt's SBOMs record code of
    others inside the shipped Qt files, but the Qt components were `LGPL-3.0-only` alone. Packaging
    now requires every compiled attribution to be covered by its component's expression
    (`Test-AttributionLicences`; for a choice, the election: FTL for the rasterizer from FreeType 2,
    CC0-1.0 for BLAKE2):
    - qtbase: `LGPL-3.0-only AND Apache-2.0 AND BSD-2-Clause AND BSD-3-Clause AND CC0-1.0 AND FTL AND
      HPND AND Imlib2 AND LicenseRef-ICC-License AND LicenseRef-Lcs-Telegraphics AND
      LicenseRef-SHA1-Public-Domain AND MIT AND MPL-2.0 AND Unicode-3.0 AND X11`;
    - qtdeclarative: `LGPL-3.0-only AND BSD-2-Clause AND MIT`;
    - qtsvg: `LGPL-3.0-only AND HPND-sell-variant`.

    An attribution this build does not compile is named in `attributionsNotCompiled` with the reason:
    the Vulkan Memory Allocator, which Qt attaches to Qt Gui, is compiled only with Vulkan, which is
    off. It is listed as such in the notices and left out of the SBOM and of the source provision. A
    name that Qt's SBOM does not record is refused.
27. **The compiler's own library code inside the programs** (the sweep). The programs instantiate the
    templates of the MSVC toolset's C++ standard library (Apache-2.0 WITH LLVM-exception, whose
    exception waives the notice conditions for compiled code). Three parts of it carry notices of
    their own:
    - `std::format`'s Unicode tables (Unicode-DFS-2016), whose notice must appear with the copies or
      in the documentation;
    - code derived from {fmt} (`MIT WITH fmt-exception`);
    - Ryu (BSL-1.0).

    The last two waive their notices for object code; they are named all the same. The policy's
    `toolchainLibraries` records the reviewed toolset (14.44.35207) and pins each header by SHA-256.
    `Get-ToolchainLibraries` finds the toolset from the build's `dumpbin`, refuses another toolset or
    header, takes each notice verbatim, and probes the programs for the width table's first eight
    values; both programs hold it. The notices list `msvc-stl` as compiled into both programs, its
    notices ship in `licenses\msvc-stl`, and the SBOM has a package for it (STATIC_LINK from Lucent)
    with its parts. The thread headers derived from William E. Kempf's Boost.Thread hold declarations
    only; the runtime DLLs stay Microsoft's, installed by its redistributable ([ADR 0009](0009-windows-packaging.md), decision 5).
28. **Inno Setup's licence ships, and the SBOM has a package for it** (R14). `installerTool` pins the
    reviewed version's `license.txt` by SHA-256 (`licenseSha256`, 6.7.3). Packaging copies the
    compiler's file byte for byte into `licenses\inno-setup\license.txt`; another version or text
    stops packaging. `THIRD-PARTY-NOTICES.txt` names the file. The SBOM's `SPDXRef-InnoSetup`
    (`InnoSetup`, the engine version and the file's copyright lines, CONTAINED by Lucent's package)
    stands for Setup's loader and the uninstaller. The code of others inside them (RemObjects Pascal
    Script, zlib, Igor Pavlov's LZMA decoder) is named with the copyright strings it carries. Their
    licence texts do not ship: the ledger's gap 13. (Closed by the gate-8 review, round 2, decision
    39: each ships with its licence, and the LZMA SDK 4.40 decoder with its source.)
29. **A recipe's own scripts are modifications** (R6). GNU libintl's portfile runs vcpkg's
    `bashify.cmake` on `gettext-runtime/configure` before configuring. The policy's `scriptedEdits`
    names such a script and the files it changes, and packaging refuses a portfile that runs
    (`cmake -P`) a script of its recipe that the policy does not name (`Test-ScriptedEdits`). The
    script is listed with the patches in `LGPL-NOTICE.txt`, the archive's README and `MANIFEST.json`
    (kind `script`). The notices and the README also name a recipe's other steps (copying build files
    into the source tree, rewriting the Makefiles the build generates), which stay in its port folder.
30. **The notices name both programs** (R7). `@PROGRAMS@` (`Get-LucentExecutablesText`) fills
    `THIRD-PARTY-NOTICES.txt` and `LGPL-NOTICE.txt`. Compiled-in code is part of
    `Lucent.exe and lucent-worker.exe`, and no LGPL code is linked statically into either.
31. **NOASSERTION in the SBOM** (R9; decision 22 corrected). Decision 22 said only the licence texts and
    notices remain NOASSERTION. That held for Lucent's own package, but of the SBOM's 470
    NOASSERTION files only 35 were those: the other 435 were the recipe files of every port package
    (155, from vcpkg's SPDX documents) and the third-party binaries (280). Now:
    - a binary file has its component's concluded licence;
    - a port package's recipe file has the licence of its shipped copy (the overlay ports), or vcpkg's
      MIT for a registry port;
    - a patch adds the library's own licence (`libraryLicence`), each licence once, so a patch of an
      MIT library is MIT (replaced by the gate-8 review, round 2, decision 36: the licences of the
      files the patch changes).

    NOASSERTION remains only on the 60 licence texts and notices that Lucent writes, which quote the
    components' licences.
32. **Gate 7 reads the package, not the working tree** (R12). `Test-SourcePackage.ps1
    -PackageManifest` takes the patch notes as the package states them: the manifest's
    `components[].note`, or, for an older manifest, its `THIRD-PARTY-NOTICES.txt`. It takes the parts
    as the manifest records them (`components[].parts`); the working tree's policy is used only for a
    manifest that records none, and the check says so. It also checks R6, R7, R9, R11, R13 and R14,
    the compiler's library and Qt's coverage.
33. **The ledger and the wording** (R8, R10, R11):
    - Lucent's row names only Lucent's own files.
    - Qt Base's row names the configure test that `silence-winrtbase-coroutine-warnings.diff` changes.
    - The passages that read as Lucent adding `lib/compat.c` to libiconv, or as every part's terms
      coming from its archive, are reworded in the policy's notes, the notices' template and the
      checklist. The checks refuse them in a package and in the policy.

`installer\tests\Test-SourcePackage.ps1` guards each item. Its self-test, section 12, covers:

- the new functions on synthetic data;
- the regressions: each finding's part refused under the licence the package had;
- the policy's entries;
- the part files' wording;
- the scripted edits;
- the recipe files' licences;
- the Qt attributions, Inno Setup's licence file and the toolchain library (with a fake toolset and
  a probe).

With `-PackageManifest`, it checks the package itself as item 32 says.

### Verification (2026-10-16, round 1)

- **The facts behind the fixes.** Each part's notice was read in its archive in vcpkg's downloads
  folder, whose SHA-512 is the one its port pins at `e143402b`. sljit's is the port's second archive.
  What this build compiles:
  - FreeType's build log compiles `bdf.c`, `pcf.c` and `ftbase.c`, and its `ftmodule.h` registers the
    BDF and PCF drivers.
  - PCRE2 is built with `PCRE2_SUPPORT_JIT`, and `pcre2_jit_compile.c` includes `sljitLir.c`.
  - Lensfun's build log reports "regex.h - not found" and builds `tre_regex`.
  - libtiff's configure summary has LZW, LogLuv, Pixar log-format and CCITT on, and its build links
    `tif_hash_set.c`.
  - HarfBuzz's link line takes `hb-ucd.cc`.
  - libjpeg-turbo is built with `WITH_SIMD`.
  - libintl's build log compiles `plural.c`.
  - Qt's configure summary reports "Vulkan: no", and qtdeclarative builds with `WTF_USE_UDIS86=0`.
  - The three MSVC headers have the SHA-256 values the policy pins, and both programs hold the width
    table.
  - Inno Setup 6.7.3's `license.txt` has the pinned SHA-256.
- **The package:** `tools\package.ps1 -BuildDir build\rel-0.1.0-lic -Jobs 6 -UpdateChannel none
  -StageOnly`, the same build of `972f349` with these changes uncommitted. Exit code 0.
  - The installation holds 420 files (395 before): the 25 new files are the parts' notices,
    `msvc-stl`'s three and Inno Setup's licence.
  - The SBOM has 134 packages, 574 files and 764 relationships (109 packages and 549 files before),
    and pyspdxtools (spdx-tools 0.8.5) validated it.
  - NOASSERTION remains on the 60 licence texts and notices only (470 files before, 435 of them
    recipe files and binaries).
  - Packaging checked 39 patches and scripts against the notes.
  - The archive is 132,489,045 bytes in 112 files, SHA-256
    `90029c4e2abfc92a960b210f2961b023fc0e938487aacaadde51cc96d5467db2`, the same in every run of
    the round. The archive is larger by its README's and `MANIFEST.json`'s new lines (bashify.cmake);
    its sources are unchanged.
  - The builder-path scan found nothing in 524 files.
  - Every file of `stage\licenses` was read.
- **The tests.** `Test-SourcePackage.ps1` passes its self-test, 357 checks (260 before). With
  `-PackageManifest` it passes 209 checks on the new package. It fails 50 of 203 on the 0.1.0 package
  of the gate-8 reading (`build\rel-0.1.0\package`) and 41 of 205 on the Setup package of the reading's
  fixes (`build\rel-0.1.0-lic\package-setup`): the parts' notices and licences (D1, D2), R6, R7, R9,
  R11, R13, R14 and Qt's coverage. `Test-ReleaseManifest.ps1` (41), `Test-UpdateChannelPolicy.ps1`
  (32) and `Test-BuilderPathScan.ps1` (81) pass unchanged.

## Amendment: the gate-8 review, round 2 (2026-10-16)

A second review of the fixes found twenty-one more discrepancies (F1 to F21; the ledger's gap 14
lists them). Each is fixed where it arises: the policy, packaging, the notice templates, the SBOM, the
ledger and, where a rule changed, the checklist. Every text the fixes add comes from a verified source,
never from memory: the corresponding-source archive, vcpkg's downloads verified by the recipe's
SHA-512, the pinned vcpkg registry, or the sources of decisions 35 and 38. What needs the owner is
recorded for gate 8 (the ledger's gap 15).

34. **No licence text written by Lucent** (F7). Decision 25's `terms`, the MIT License's words that
    the policy wrote out for LibRaw's Adobe DNG SDK code, is withdrawn, and packaging refuses a part
    that has it (`Test-PartLicences`). The Adobe part names, with `textIn`, the MIT texts of
    nlohmann-json's `copyright.txt` (its `LICENSE.MIT`) and `UTF-8-decoder-MIT.txt`
    (`LICENSES/MIT.txt`), both from nlohmann-json's verified archive. The checklist's exception for it
    is gone.
35. **Published sources kept in the repository** (F4, F12). Some texts a release must ship are in no
    vcpkg archive: Inno Setup's and Apache Tika's. `installer\third-party` keeps the files byte for
    byte (`.gitattributes`: `-text`); the policy's `thirdPartySources` names each folder's
    repository, commit and tag and pins every file by its git blob id, and `Get-ThirdPartySourceFile`
    refuses other bytes (the same text with other line endings gives the blob's bytes). The files are
    RemObjects Pascal Script's `LICENSE.md` and the LZMA SDK 4.40 decoder's five source files from
    `jrsoftware/issrc` at `is-6_7_3` (commit `4adf37ed`, the signed tag of the compiler that makes
    Setup), and Tika's `NOTICE.txt` at `408c26e1` (the commit Qt's SBOM records). Inno Setup's
    `license.txt` must also have its blob at that tag (`licenseGitBlob`). The owner accepts this route,
    like the SHA-256 values the policy pins, at gate 8.
36. **A patch's licence is that of the files it changes** (F5, F6, F11). Decision 31's rule (`MIT AND`
    the library's licence) is replaced. `Get-PatchLicences` reads each patch a recipe applies, finds
    the files it changes in the port's verified archives and gives each its licence:
    - its `SPDX-License-Identifier` line, with Lucent's election (a `LicenseRef-` alternative such as
      Qt's commercial licence is dropped, and where the library's own licence is among the rest, that
      one: Qt's `LGPL-3.0-only`);
    - else the port's `fileLicences` rule in the policy: the files it covers, the licence, and a
      statement the file's header must hold or an evidence line of an archive member. A rule may say
      NOASSERTION where the archive states no licence (Lensfun's `CMakeLists.txt`: its README's
      GPL-3.0 sentence names the build system 0.3.4 no longer has).

    A file that neither gives stops packaging. The patch is `MIT AND` those licences (vcpkg's or
    Lucent's MIT for its own lines), or NOASSERTION; the SBOM and the archive's `MANIFEST.json`
    (`modifications[].licence`, `quotes`) agree. Qt's `macdeployqt-symlinks.patch` and
    `windeployqt-webengine-debug-resources.patch` are `MIT AND GPL-3.0-only WITH Qt-GPL-exception-1.0`;
    LibRaw's two patches, which change LibRaw-cmake's files, not LibRaw's,
    `MIT AND BSD-3-Clause AND GPL-2.0-or-later`; libiconv's `libcharset.h.in` patch
    `MIT AND LGPL-2.0-or-later`; libtiff's `FindCMath.patch` `MIT AND BSD-3-Clause`. When a file the
    archive conveys needs a licence text its own archive lacks, the policy's
    `correspondingSource.licenceTexts` takes it from another verified archive into the archive's
    `licenses/` (the GPL-2.0 for LibRaw-cmake's `CMakeLists.txt`, qtbase's
    `LICENSES/GPL-2.0-only.txt`), `MANIFEST.json` records it, and the read-back check requires a text
    for every GPL-2.0 licence a modification records. The notices and the archive's README say that a
    patch's quoted lines keep their files' licences.
37. **Qt's records reviewed** (F2, F12, F13). Where Qt's SBOM falls short, the policy corrects it per
    port, and `Resolve-AttributionPolicy` applies it:
    - `attributionLicences`: the licence the code carries instead of the one Qt records, with
      evidence lines from the archive (Khronos' OpenGL and OpenGL ES headers: `MIT-Khronos-old`, as
      Qt's `REUSE.toml` names them), or a licence in addition (Apache Tika's MIME type definitions:
      `Apache-2.0 AND OGL-UK-3.0`, for the PRONOM definitions Tika's NOTICE names);
    - `attributionNotices`: the NOTICE lines that pertain to the code, from the kept file of decision
      35 (Tika's, as `licenses\qtbase\Apache-Tika-NOTICE.txt`; Apache-2.0 section 4(d));
    - `attributionSources`: the Source Code Form of copyleft code at the commit of the package URL
      Qt's SBOM records (the Public Suffix List: `public_suffix_list.dat` of `publicsuffix/list` at
      `944fb533`; MPL-2.0 section 3.2(a)). Decision 3 said qtbase's archive covers it, but that archive
      holds only the array Qt generated from it. The notices name it, and the SBOM gives its package
      that commit as the download location with a `sourceInfo`.

    Copyleft code inside a Qt file with no Source Code Form recorded stops packaging, as does a record
    whose commit is not Qt's, an unknown licence or missing evidence. qtbase is now
    `... MIT AND MIT-Khronos-old AND MPL-2.0 AND OGL-UK-3.0 ...`.
38. **Code of the Windows SDK inside Qt's files** (F1). Qt's build configures C++/WinRT
    (`QT_FEATURE_cpp_winrt`), whose templates are compiled into `Qt6Core.dll` and `qwindows.dll`; Qt's
    SBOM does not record it. The policy's `toolchainLibraries.cppwinrt` (kind `windows-sdk`) records the
    reviewed SDK (10.0.26100.0) and version (2.0.250303.1), pins the SDK's `cppwinrt\LICENSE.txt` by
    SHA-256 and names the version line of `winrt/base.h` and the probe `C++/WinRT version:`.
    `Get-WindowsSdkLibrary` finds the SDK, refuses another version or licence file, probes every
    shipped binary and requires the binaries that hold it to belong to the components the policy names
    (`inside`). The licence ships as `licenses\cppwinrt\LICENSE.txt`, the notices list C++/WinRT with
    Qt Base, and the SBOM's package is CONTAINED by Qt Base's binary package. The SDK's Direct3D 12
    and DXC headers that Qt Gui includes give declarations and GUID constants only (Qt Base's note).
39. **The code of others inside Inno Setup** (F4; decision 28's open part, the ledger's gap 13).
    `installerTool.parts` names each part with the compiler's files that hold it, a probe text it
    carries there, its licence and the source of that licence's text:
    - RemObjects Pascal Script, in the setup engine: its own licence (`LicenseRef-`), `LICENSE.md` of
      decision 35; its third condition asks for a line naming it and where it can be found, which
      the notices carry under "Required notices" with the address the engine itself holds;
    - zlib 1.3.1, in the engine: Zlib, whose text zlib's `copyright.txt` holds;
    - the LZMA SDK 25.00 decoders, in the engine: public domain, as their probe states;
    - the LZMA SDK 4.40 decoder, in Setup's loader (`SetupLdr.e32`; Lucent Setup is a 32-bit program
      made with it): the GNU LGPL or the Common Public License; Lucent elects the LGPL in its version
      2.1, whose text GNU libintl's `copyright.txt` holds, and ships its five source files as Inno
      Setup publishes them in `licenses\inno-setup\LzmaDecode`.

    `Get-InnoSetupParts` refuses a part that the compiler's files do not hold, a copyleft part
    without its source and a notice whose address the engine lacks. Each part's file is in
    `licenses\inno-setup`, the notices list the parts, and the SBOM's Inno Setup package (download
    location `issrc` at the commit) contains a package for each.
40. **The redistributable's terms named truthfully** (F3). The SBOM's text said the redistributable
    installer shows its terms when Setup runs it, but Setup runs it silently
    (`/install /quiet /norestart`). Its own licence, read in the `license.rtf` of the bundled
    `vc_redist.x64.exe`, is "MICROSOFT SOFTWARE LICENSE TERMS - MICROSOFT VISUAL C++ 2015 - 2022 RUNTIME"
    (EULA ID `Cpp_2015-2022_ENU.1033`). `vcRedistributable.reviewed` records it with the version and
    SHA-256, packaging stops for another redistributable, and the notices, the SBOM and the package
    manifest name the terms. Lucent redistributes it as Distributable Code under the licence of the
    Visual Studio 2022 Build Tools, whose conditions were never reviewed. The policy's `termsReview`
    records them as pending, together with the runtime's startup code that the toolset links into
    the binaries; packaging warns, and gate 8 stays open until the owner records the review.
41. **Each program's whole licence** (F15, F16). The SBOM gives `Lucent.exe` and `lucent-worker.exe`
    `MIT AND LicenseRef-Lucent-Logo` and the licences of the code compiled or linked into each
    (`Get-ProgramLicences`: the compiled-in components, the static libraries, the compiler's library),
    and its comment names them (ADR 0025's item 29 gave the programs `MIT AND LicenseRef-Lucent-Logo`
    alone). Their version resources (`cmake\LucentBranding.cmake`) and Setup's `AppCopyright` keep
    "Copyright (c) 2026 Fan Zhu. MIT License.": the review proposed adding the exceptions there, but the
    owner had decided on 2026-10-15 to keep those lines (ADR 0025, item 27), so the notices and the SBOM
    state the exceptions instead.
42. **What the sweep missed** (F8, F9, F14, F19, F20). LibRaw's Sony YCC and lossless JPEG decoders,
    "partially backported from DNGLab's rust code", are a part under LibRaw's LGPL-2.1 with
    `sonycc.cpp`'s header (DNGLab's repository states LGPL-2.1, which Lucent's election fits); the SIMD
    part of libjpeg-turbo names the holders of every compiled SIMD file. The notes and the ledger
    record gnulib's GPL-3.0 `alloca.h` in `intl-8.dll` (for MSVC it gives no code: a judgement),
    Lensfun's GPL-2.0 getopt (not compiled) and libiconv's Unicode-derived tables (no Unicode terms
    stated).
43. **The wording** (F10, F17, F18, F21). The archive's README says where the licence texts its
    archives lack are. The notices and `LGPL-NOTICE.txt` count Qt Declarative's 174 QML source files
    with its 19 `qmldir` and 19 `.qmltypes` files. The notices' sentence on `copyright.txt` is limited
    to vcpkg components: `msvc-stl`'s folder holds no `copyright.txt`, `cppwinrt`'s the SDK's licence.
    The ledger's PCRE2 row names `pcre2-8.dll`.

`installer\tests\Test-SourcePackage.ps1` guards each item. Its self-test, section 13, covers the new
functions on synthetic data (patch licences with elections, rules and errors; kept files with wrong
bytes; Qt corrections with stale commits, missing evidence and copyleft code without a source; a fake
Windows SDK; a fake Inno Setup compiler; the programs' licences), the policy's entries, the
templates' wording and the copyright lines. With `-PackageManifest` it checks the package itself:
the Windows SDK's code with its licence file and its SBOM package, the Source Code Form of copyleft
code in Qt, Khronos' licence and Tika's NOTICE lines, the redistributable's terms (and prints the
owner's review), every Inno Setup part with its file, notice, source and SBOM package, each patch's
licence in `MANIFEST.json` and the SBOM (with the findings' patches), each program's licence and
version resource, Qt Declarative's QML count, the notices' and the README's wording.

### Verification (2026-10-16, round 2)

- **The facts behind the fixes.**
  - C++/WinRT: the Windows SDK 10.0.26100.0's `cppwinrt\winrt\base.h` defines `CPPWINRT_VERSION
    "2.0.250303.1"`, and its `LICENSE.txt` has the pinned SHA-256 `f6048594...`; `Qt6Core.dll` and
    `qwindows.dll` carry "C++/WinRT version:2.0.250303.1", no other shipped binary does.
  - Qt's SBOM gives the Public Suffix List the package URL
    `pkg:generic/psl-data@944fb53321606f08740f04ef7c386f64df34ff76`; qtbase's archive holds only
    `src/3rdparty/libpsl/psl_data.cpp`, generated from it. Qt's `REUSE.toml` names `MIT-Khronos-old`
    for the Khronos headers. Tika's `NOTICE.txt` at `408c26e1` (blob `e4713574...`) names
    `tika-mimetypes.xml`'s PRONOM definitions and the Open Government Licence 3.0 (lines 19 to 21).
  - The redistributable's English `license.rtf` (SHA-256 `8099dc3c...`), taken with 7-Zip from the
    Authenticode-checked `vc_redist.x64.exe` (SHA-256 `cc0ff0eb...`), names the terms in its first
    lines and the EULA ID in its last.
  - Inno Setup: the tag `is-6_7_3` of `jrsoftware/issrc` is signed; its `license.txt` is the
    compiler's (blob `ac4dfe9e...`). The compiler's `Setup.e32` holds Pascal Script's copyright and
    address, zlib 1.3.1's and the LZMA SDK 25.00 decoders' strings, and `SetupLdr.e32` the 4.40
    decoder; a Setup this compiler made (`build\rel-0.1.0\package`) is a 32-bit program holding
    `SetupLdr.e32`'s code, not `SetupLdr.e64`'s.
  - The patches' files were read in the ports' verified archives: LibRaw-cmake's `CMakeLists.txt`
    has Gilles Caulier's GPL-2.0-or-later notice, its `LICENSE` is BSD-3-Clause; Lensfun's README
    gives the GPL-3.0 to `build/` and `ac.py`, and its ChangeLog records that build system's removal;
    Qt's deployment tools' files carry `LicenseRef-Qt-Commercial OR GPL-3.0-only WITH
    Qt-GPL-exception-1.0`.
  - LibRaw 0.22.2's `sonycc.cpp`, `losslessjpeg.cpp` and `losslessjpeg.h` name DNGLab; DNGLab's
    repository (tag `v0.7.0`, commit `8f9675c0`) states LGPL-2.1 in its `LICENSE` and
    `rawler/Cargo.toml`.
- **The package:** `tools\package.ps1 -BuildDir build\rel-0.1.0-lic -Jobs 6 -UpdateChannel none
  -StageOnly`, the same build of `972f349` with these changes uncommitted. Exit code 0.
  - The installation holds 432 files (420 before): the 12 new files are C++/WinRT's licence, Tika's
    NOTICE lines, LibRaw's DNGLab file, Inno Setup's four part files and the 4.40 decoder's five
    source files.
  - The SBOM has 140 packages, 586 files and 782 relationships (134, 574 and 764 before), and
    pyspdxtools (spdx-tools 0.8.5) validated it. NOASSERTION remains on the 67 licence texts and
    notices and on Lensfun's `fix_build.patch`.
  - Packaging derived the licences of 63 patches from the files they change (one NOASSERTION) and
    checked 39 patches and scripts against the notes. It warned that the owner's review of the
    redistributable's terms is pending.
  - The archive is 132,543,226 bytes in 113 files (the GPL-2.0 text added), SHA-256
    `db7a8d1c245cddd2e03fbff7df578400425e8b2b44ba44b6d118d38315120bb8`; its sources are unchanged.
  - The builder-path scan found nothing in 537 files.
  - Every file of `stage\licenses` was read.
- **The tests.** `Test-SourcePackage.ps1` passes its self-test, 428 checks (357 before). With
  `-PackageManifest` it passes 241 checks on the new package. It fails 64 of 218 on the 0.1.0 package
  of the gate-8 reading (`build\rel-0.1.0\package`) and 56 of 221 on the Setup package of the reading's
  fixes (`build\rel-0.1.0-lic\package-setup`); of those, 13 and 14 are round 2's: the Public Suffix
  List's Source Code Form, Khronos' licence, the redistributable's terms, the Inno Setup parts, the
  patches' licences, the programs' licences and copyright lines, the QML count, the notices' and
  README's wording, the Adobe text and the SIMD holders. `Test-ReleaseManifest.ps1` (41),
  `Test-UpdateChannelPolicy.ps1` (32) and `Test-BuilderPathScan.ps1` (81) pass unchanged.
