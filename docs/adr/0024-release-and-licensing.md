# ADR 0024: Release and licensing: free MIT distribution, a corresponding-source archive beside every installer, unsigned releases, and the release checklist

- **Status:** Accepted (amended 2026-10-15 by the W12 release review: "Amendment: the release review"
  below)
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
   (MPL-2.0) is covered by qtbase's archive.

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
