# Release checklist

How a Lucent release is made, checked, published and archived, from a clean commit to the files on
the download page. Every gate has a command and a pass criterion; a release is published only when
all of them pass. The decisions behind it are in
[ADR 0024](../adr/0024-release-and-licensing.md); the tools are described in
[setup](../development/setup.md#packaging-the-installer) and
[ADR 0009](../adr/0009-windows-packaging.md).

**Distribution (decided 2026-10-10).** Lucent is free and open-source software under its MIT
licence and is not sold, so Inno Setup's non-commercial terms apply. Each installer is published
with `Lucent-<version>-corresponding-source.zip`, the corresponding source of the LGPL libraries it
contains. Releases are **unsigned** until a code-signing certificate exists; the signing pipeline
stays ready (step 11).

Commands run from the repository root in Windows PowerShell on the release machine (the reference
machine of [setup](../development/setup.md)). `<v>` is the version being released, for example
`0.1.0`; `<prev>` the previous release.

## Gates at a glance

| # | Gate | Command (details below) | Pass criterion |
|---|---|---|---|
| 1 | Clean commit | `git status --porcelain`; `git log -1` | No output; `HEAD` is the reviewed release commit on `main` |
| 2 | Version | `Select-String` on `CMakeLists.txt` and `vcpkg.json` | Both say `<v>`; `<v>` follows the version rules and was never released before |
| 3 | Tests from a clean build | `tools\build.ps1 -Preset dev-core -Fresh -Test`, `-Preset dev -Fresh -Test`, fixture tier `all` | Exit code 0; no test failed |
| 4 | Qualification data | `lucent-qualify --tier all` from the clean build; compare with the committed table | 0 expectation mismatches; the same outcomes as the committed report, which a clean build of the release's code produced |
| 5 | Performance (regression) | `tools\bench\Invoke-LucentBench.ps1`, `Test-BenchBudgets.ps1` | `Test-BenchBudgets.ps1` exits 0 (every scenario started on a quiet machine), or each exceedance is explained in the release notes |
| 6 | Package | `tools\package.ps1 -Fresh -RunTests -UpdateChannel none` | Exit code 0; manifest `gitDirty` false, `gitCommit` the release commit, audit and builder-path errors 0 |
| 7 | Corresponding source | `installer\tests\Test-SourcePackage.ps1 -PackageManifest ...` | Exit code 0 (every LGPL library's archives and recipe present, every hash as recorded, vcpkg's and Lucent's licences beside the recipes; the installation folder's licences as step 7 lists them) |
| 8 | Licence review | Read `THIRD-PARTY-NOTICES.txt`, `LGPL-NOTICE.txt`, the ledger, `assets\branding\provenance.json` | No component, version or licence differs from the [ledger](../legal/third-party-notices.md); the LGPL notice names the archive and lists each library's modifications; the logo's licence terms are recorded and `THIRD-PARTY-NOTICES.txt` states them; the notices name the redistributable's and Inno Setup's versions with Inno Setup's licence file, every data asset, every part of a component under a licence of its own with the file of its notice and terms (the compiler's library, the Windows SDK's C++/WinRT and the code inside Inno Setup among them), and vcpkg's licence beside its recipes; the owner's open decisions (the redistributable's Distributable Code conditions, the pinned sources) are recorded |
| 9 | Installer on the release machine | `installer\tests\Test-InstalledPackage.ps1 ... -RuntimeOutcomes -NewerInstaller` (and `-PreviousInstaller`) | Exit code 0, every check passed (install, same-version repair, upgrade, refused downgrade) |
| 10 | Clean machine, offline | Windows Sandbox: `tools\Test-CleanMachine.ps1`, then the manual steps ([clean-machine.md](clean-machine.md)) | The script exits with 0 (`<gate>\host-check.json` passed); every manual step as described, the declined and the accepted elevation prompt on a Windows that asks for consent |
| 11 | Signing | Only when a certificate exists (until then: skipped, release unsigned) | Every signature verified by `tools\package.ps1` and `Test-InstalledPackage.ps1 -SignerThumbprint` |
| 12 | Update channel | Only for a signed release on a configured channel (until then: `-UpdateChannel none`) | `Test-LucentReleaseManifest` passes |
| 13 | Publish | Upload the files of "What is published" | Checksums on the page equal the package manifest's |
| 14 | Archive | Copy the release folder to the archive | The archive holds every file of "What is archived" |

The blueprint's required release gates (section 10) map onto these as listed in
[Blueprint release gates](#blueprint-release-gates).

## 1. Clean commit

Everything that ships comes from one commit on `main`, with nothing uncommitted: the package and
Lucent.exe name it, the corresponding-source archive points at it, and Lucent's own source is
published at it.

```powershell
git switch main
git status --porcelain          # must print nothing
git log -1 --format='%H %s'     # the release commit
```

**Pass:** `git status --porcelain` prints nothing (untracked build output under `build\` is ignored);
`HEAD` is the reviewed commit. Every later step builds this commit; a change after this point
(for example regenerated qualification data, step 4) makes a new release commit, and the steps run
again from here.

## 2. Version

Lucent's version is `MAJOR.MINOR.PATCH` (SemVer, no leading zeros, no fourth part;
`cmake/LucentVersion.cmake` refuses anything else). It is written in two places that change
together, in one commit: `project(Lucent VERSION <v> ...)` in `CMakeLists.txt` and
`"version-semver": "<v>"` in `vcpkg.json` (`tools\package.ps1` refuses a mismatch, because the
corresponding-source archive ships `vcpkg.json`).

- **PATCH:** fixes only. No new catalog schema, recipe ("lucent.develop") version, worker protocol,
  asset schema or dependency version.
- **MINOR:** new features, a new dependency version, or a new catalog schema or recipe version that
  older releases still open read-only ([ADR 0022](../adr/0022-updates-and-migrations.md),
  [ADR 0008](../adr/0008-catalog-and-source-identity.md)). The release notes say which older versions
  can still open a library this version wrote.
- **MAJOR:** an incompatible change: dropping the reading of an older catalog or recipe version, or
  dropping a supported Windows version. (Before 1.0.0, MINOR is used for these too, and the notes say
  so.)
- A version is never reused: "Check for updates" compares versions, and the update rehearsal installs
  the previous release's Setup. A failed release attempt that was published gets the next PATCH.
- The camera support database (`assets\camera-support`) has its own SemVer
  ([qualification README](../qualification/README.md)); a dependency upgrade is a licence event
  ([ADR 0003](../adr/0003-linking-and-licensing.md), decision 7): its row in the
  [ledger](../legal/third-party-notices.md) and `installer\license-policy.json` change in the same
  commit.

```powershell
Select-String -Path CMakeLists.txt -Pattern '^\s+VERSION ' ; Select-String -Path vcpkg.json -Pattern 'version-semver'
git tag --list "v<v>"            # must print nothing: never released before
```

**Pass:** both files say `<v>`; no tag `v<v>` exists.

## 3. Tests from a clean build

Fresh build trees of the release commit, with the camera fixtures cached
(`tools\fetch-fixtures.ps1 -Tier all`):

```powershell
powershell -NoProfile -File tools\fetch-fixtures.ps1 -Tier all
powershell -NoProfile -File tools\build.ps1 -Preset dev-core -BuildDir build\rel-<v>-core -Fresh -Jobs 6 -Test
powershell -NoProfile -File tools\build.ps1 -Preset dev -BuildDir build\rel-<v>-dev -Fresh -Jobs 6 -Test
. .\tools\dev-env.ps1             # the pinned CMake's ctest on PATH
$env:LUCENT_TEST_FIXTURE_TIER = 'all'
ctest --test-dir build\rel-<v>-dev -L "fixtures|qualification" --output-on-failure
Remove-Item Env:\LUCENT_TEST_FIXTURE_TIER
```

**Pass:** every command exits with 0 and CTest reports no failed test (skipped fixture tests are
allowed only in the first two runs, which use the default tier). The build trees are configured on
the clean release commit of step 1 (a build of uncommitted changes names its commit `<commit>-dirty`,
which `lucent-qualify` refuses to export and the package manifest records).

## 4. Qualification data from the clean build

The qualification report, the support table and the camera support database are evidence for
"Camera correctness"; they must come from a clean build of the release's code
([qualification README](../qualification/README.md), [ADR 0016](../adr/0016-camera-qualification.md),
[ADR 0020](../adr/0020-camera-correctness-gate.md)).

```powershell
$q = 'build\rel-<v>-core\bin\lucent-qualify.exe'
& $q --tier all --out-json build\rel-<v>-qualification.json --out-md build\rel-<v>-support-table.md
# The same outcomes as the committed table: only the "Generated ..." line (date, commit) and the last
# section, "## Measurements" (times and memory, which differ on every run), may differ.
$outcomes = { param($table) $lines = @(Get-Content $table); $end = [Array]::IndexOf($lines, '## Measurements')
              if ($end -lt 0) { $end = $lines.Count }; $lines[0..($end - 1)] | Where-Object { $_ -notmatch '^Generated ' } }
Compare-Object (& $outcomes docs\qualification\support-table.md) (& $outcomes build\rel-<v>-support-table.md)
# The committed report came from a clean build of this release's code: between its commit (the
# "Generated" line of docs\qualification\support-table.md) and HEAD nothing but data changed.
git diff --stat <report commit> HEAD -- src apps cmake CMakeLists.txt vcpkg.json vcpkg-configuration.json fixtures
```

**Pass:** `lucent-qualify` exits with 0 (no expectation mismatch, no harness error), both
comparisons print nothing, and step 3 passed `CommittedData.TheCameraSupportAssetIsTheExportOfTheReport`
and `BundledCameraSupport.*` (the committed database is the export of the committed report and holds
for this build's versions). Otherwise regenerate the committed report, table and database from this
clean build as the [qualification README](../qualification/README.md) says (raising the database
version by its rules), commit them together, and start again at step 1 with that commit: its
report then names the commit just before it, whose code is the release's.

## 5. Performance regression check

```powershell
powershell -NoProfile -File tools\build.ps1 -Preset release -BuildDir build\rel-<v>-perf -Jobs 6
powershell -NoProfile -File tools\package.ps1 -BuildDir build\rel-<v>-perf -SkipBuild -StageOnly
powershell -NoProfile -File tools\bench\Invoke-LucentBench.ps1 -BuildDir build\rel-<v>-perf -OutputDir build\rel-<v>-bench
powershell -NoProfile -File tools\bench\Test-BenchBudgets.ps1 -DataDir build\rel-<v>-bench
```

**Pass:** `Test-BenchBudgets.ps1` exits with 0. A budget that fails because of a measured, wanted
change is accepted only with a new baseline ([ADR 0021](../adr/0021-performance-baseline.md)) or a
line in the release notes. Run it on an idle machine; the measurements take about an hour. The
harness waits for a quiet machine before every scenario (no other process above a quarter of one
processor) and records each wait; `Test-BenchBudgets.ps1` refuses a run in which a scenario started
busy, skipped the wait or recorded none (exit code 3): measure again on an idle machine, never pass
it with `-AllowBusyMachine` for a release.

## 6. Package

Unsigned, without an update channel (the state of every release until step 11 and 12 apply):

```powershell
Remove-Item Env:\LUCENT_SIGN_CERT_SHA1, Env:\LUCENT_SIGN_TEST -ErrorAction SilentlyContinue
powershell -NoProfile -File tools\package.ps1 -BuildDir build\rel-<v> -Fresh -RunTests -Jobs 6 -UpdateChannel none -VersionRehearsal
```

`-VersionRehearsal` also writes `rehearsal\Lucent-Setup-<next patch>-x64-VERSION-REHEARSAL.exe`, the
same files registered as the next patch version, for step 9's and step 10's downgrade checks. It is a
test installer: never published, never archived as a release file.

`tools\package.ps1` builds the release preset from scratch and runs its tests, lays out and audits
the installation folder, runs it with a Windows-only `PATH`, writes the licences, **writes and
re-reads the corresponding-source archive**, writes the LGPL notice that names it, the SBOM and the
installed-file list, scans every shipped file and the archive's own files for the build machine's
folders, compiles Setup and writes the package manifest. Any failure stops it.

**Pass:** exit code 0, and in `build\rel-<v>\package\Lucent-Setup-<v>-x64.manifest.json`:
`gitDirty` is `false`, `gitCommit` is the release commit, `audit.errors` is 0,
`audit.builderPaths.hits` is 0, `distribution.commercial` is `false`, `toolchain.innoSetupLicence`
is the compiler's licence line, `correspondingSource.file` is
`Lucent-<v>-corresponding-source.zip` with the size and SHA-256 the script printed, and `signed` is
`false` (until step 11). `sbom.validatedBy` is `pyspdxtools`: install spdx-tools in the tools
environment from `tools\requirements-spdx-tools.txt` (pinned, `--require-hashes`;
[setup](../development/setup.md)) or pass `-SpdxValidator`; without it only the structural check
runs, and the release notes do not claim a validated SBOM. The script prints the installer's and the
archive's path, size and SHA-256: copy them into the release notes.

## 7. Corresponding source

```powershell
powershell -NoProfile -File installer\tests\Test-SourcePackage.ps1 -PackageManifest build\rel-<v>\package\Lucent-Setup-<v>-x64.manifest.json
```

It checks the archive beside the manifest against it: name, size and SHA-256; exactly the files the
manifest lists; for every component the manifest marks `correspondingSource` (every LGPL library)
its source archives by SHA-512 and its recipe files by SHA-256; vcpkg's licence
(`licenses/vcpkg-LICENSE.txt`) and Lucent's beside the recipes; no component with a licence that
leaves no choice but a copyleft one is unmarked; the archive's form (sorted, stored, fixed time,
`SHA256SUMS.txt` complete); that each library's note, as the package states it (the manifest's
`components[].note`, or for an older manifest its `THIRD-PARTY-NOTICES.txt`; never the working
tree's policy), names every patch and script of its recipe that changes more than build files; and
that `MANIFEST.json` lists a recipe's scripts with its patches (GNU libintl's `bashify.cmake`). With
the installation folder beside the manifest (`stage\`), it also checks the licences step 8 reads
(ADR 0024, amendments of 2026-10-16): `THIRD-PARTY-NOTICES.txt` names the Visual C++
redistributable's and Inno Setup's versions as the manifest records them, every bundled asset, and
both programs as the home of compiled-in code (and `LGPL-NOTICE.txt` that neither links LGPL code
statically); each component's parts under their own licences, as the manifest records them, have
their notices in `licenses\<port>\` and their licences in its expression; the compiler's library
(`msvc-stl`) is listed with its notices from the reviewed headers; Inno Setup's `license.txt` ships
with the SHA-256 the manifest records; `licenses\vcpkg-LICENSE.txt` is beside the shipped recipes;
the SBOM concludes a licence for every file but the licence texts and notices (each licence once, a
recipe file the same in the port's package as in the installation), gives each component's packages
its licence, has a package for Inno Setup and gives no part its library's version; and each Qt
component's licence covers the third-party code Qt's SBOM records inside it. Since the gate-8 review,
round 2 (ADR 0024, amendment of that name): the Windows SDK's code the manifest records (C++/WinRT)
ships with its licence file and is a package of the binary that holds it; copyleft code inside Qt
has its Source Code Form named at a commit; Khronos' headers are `MIT-Khronos-old`, and the NOTICE
files the notices name ship; the redistributable's licence terms are named (the check prints the
owner's review of the conditions under which Lucent redistributes it, as the manifest records it);
every part inside Inno Setup has its file, its notice and, under a copyleft licence, its source, and
is a package that Inno Setup's contains; each patch has the same licence in the archive's
`MANIFEST.json` and in the SBOM, and the patches the review named are no longer their library's
licence; each program's file in the SBOM carries its whole licence, and its version resource excepts
the logo and third-party code; Qt Declarative's QML source files are counted; the notices supply no
licence words; and the archive's README says where the licence texts its archives lack are.

**Pass:** exit code 0. Then open `README.txt` in the archive: the version, the registry commit, the
libraries and the rebuild command are this release's. Optional: package again into another folder,
`tools\package.ps1 -BuildDir build\rel-<v> -SkipBuild -StageOnly -OutputDir build\rel-<v>\package-again`;
its archive must have the same SHA-256 (the archive is deterministic: the same inputs give the same
bytes on any machine, whatever its regional format; ADR 0024, decision 14). Never package again into
the release's own folder: that replaces the files step 6 recorded.

## 8. Licence review ("Distribution")

Packaging already refuses an unreviewed dependency version, an unknown licence, LGPL or GPL code
linked statically or compiled into the programs, a copyleft licence without a source provision (an
alternative under a `LicenseRef-` licence, such as a vendor's commercial terms, is no choice Lucent
has), GPL or AGPL code without an exception even with a source provision (unless an ADR accepted it),
a missing licence text and, for a commercial distribution, an Inno Setup whose banner does not state
a commercial licence (ADR 0003 and ADR 0024, amendments of 2026-10-15); a part of a library compiled
under a licence its concluded expression leaves out, a part's terms not found in the verified source
archive, vcpkg's licence not verified at the registry baseline, a note that leaves out a patch
changing more than build files, and a Setup compiled by another Inno Setup than the notices name
(ADR 0024, amendment of 2026-10-16); third-party code that Qt's SBOM records inside a Qt component's
files and its licence does not cover, a part's licence text not found in the archive or in the
other component's licence file named for it, a recipe script the policy does not name, and another
MSVC toolset, C++ standard library header or Inno Setup `license.txt` than the policy reviewed
(ADR 0024, amendment "the gate-8 review, round 1"); a part whose licence text the policy writes out
(`terms`), a file of `installer\third-party` without its pinned git blob id, a patch that changes a
file whose licence neither its SPDX line nor a rule of the policy gives, copyleft code inside a Qt
file without its Source Code Form or with one at another commit than Qt's record, a correction of
Qt's record without its evidence, another Windows SDK, C++/WinRT version or licence file than the
policy reviewed, a part of Inno Setup that the compiler's files do not hold or a copyleft one without
its source, and another Visual C++ redistributable than the one whose licence was read (ADR 0024,
amendment "the gate-8 review, round 2"). What remains is a reading:

- `build\rel-<v>\package\stage\licenses\THIRD-PARTY-NOTICES.txt` lists the same components, versions
  and licences as the [ledger](../legal/third-party-notices.md)'s release table, and the credit lines
  (FreeType, IJG, libtiff's LZW code, the Lensfun lens database, Inno Setup). It names the Visual C++
  redistributable and Inno Setup with the versions the package manifest records
  (`vcRedistributable.version`, `toolchain.innoSetupVersion`), every bundled data asset under "Data"
  (Lucent's own, such as the camera support database, too), every part of a component under a
  licence of its own with the file of its notice and terms in `licenses\<component>\` (the ledger's
  section "Code under licences of its own inside the shipped files" lists them, and the code present
  but not compiled), and the licences of the vcpkg recipes that ship (`licenses\vcpkg-LICENSE.txt`
  beside them). Each part's file says where each of its texts comes from: the component's source
  archive, the toolset's header for the compiler's library (`licenses\msvc-stl`), Inno Setup's
  published source for its parts, or, where the archive names a licence without its text, another
  component's licence file from a verified source that holds it (the MIT text of LibRaw's Adobe DNG
  SDK code is nlohmann-json's). No licence text is Lucent's own words (since the gate-8 review,
  round 2, which withdrew the only exception). Inno Setup's licence is
  `licenses\inno-setup\license.txt`, and the code of others inside its loader and engine is listed
  with its licences in `licenses\inno-setup` (the LZMA SDK 4.40 decoder with its source); C++/WinRT,
  the Windows SDK's code in Qt Base's files, is listed with Qt Base, its licence in
  `licenses\cppwinrt`; the notices name the Visual C++ redistributable's licence terms.
- `LGPL-NOTICE.txt` names `Lucent-<v>-corresponding-source.zip` with the size and SHA-256 of step 6,
  and gives every library a "Modified" entry: each patch its recipe applies to the upstream source,
  with the files it changes and who made it when (vcpkg's dated by the registry commit, Lucent's
  `lucent-*.patch` by their own text), or "Modified: no". The ledger's release table says the same.
- No model weights, datasets or other assets without a recorded licence ship
  ([ADR 0019](../adr/0019-learned-appearance-research.md)): `assets\` holds `camera-support` and
  `lens-database`, which ship as files, and `branding`, Lucent's logo, which is built into the programs
  and Setup ([ADR 0025](../adr/0025-branding.md)).
- The logo's licence terms are decided by the project owner and recorded: `licence` in
  `assets\branding\provenance.json` holds them (their `LicenseRef-` identifier, a name, the text and
  the date of the decision), the asset's README states them, and so do the top of
  `THIRD-PARTY-NOTICES.txt` and the SBOM (Lucent's package is `MIT AND` that identifier, and
  `Lucent.exe` and `lucent-worker.exe` are `MIT AND` that identifier and the licences of the code
  compiled or linked into each; until the gate-8 review, round 2, this sentence and the SBOM gave
  the programs `MIT AND` the identifier alone). The programs' version resources and Setup's
  copyright line keep "Copyright (c) 2026 Fan Zhu. MIT License." (the owner's decision of
  2026-10-15); the notices state the exceptions. The logo is not covered by the MIT License; packaging refuses a
  logo whose terms are not recorded. **Met for 0.1.0:** the owner decided on 2026-10-15 (all rights
  reserved; others may show the logo, unmodified, only to refer to Lucent; the GNU LGPL's rights are
  not limited; `LicenseRef-Lucent-Logo`),
  recorded as branding 1.0.2 ([ADR 0025](../adr/0025-branding.md), amendment of 2026-10-15). What
  remains for each release is to read that the notices state these terms. (The logo was supplied by
  the owner on 2026-10-11 and made with an image generator; its provenance is recorded there.) A
  change of the terms changes the asset: edit `provenance.json` (`licence`, `licence_status`) and the
  asset's README, run
  `powershell -NoProfile -File tools\branding\Update-Branding.ps1 -Version <the next PATCH version>`
  (the manifest lists `provenance.json`, so `-Check`, the build and packaging refuse the folder until
  then), commit, and start again at step 1 with that commit. A new logo records its terms with its
  provenance (the asset's README, "Replacing the logo").
- `installer\license-policy.json` "distribution" still describes the release: free, MIT, not sold
  (`commercial: false`), unsigned or signed as built.
- The owner's decisions that the gate-8 review, round 2, left open are recorded (the ledger's
  gap 15): the review of the conditions of the Visual Studio 2022 Build Tools' licence for
  Distributable Code, under which a release redistributes `vc_redist.x64.exe` (run silently) and links
  the runtime's startup code (`vcRedistributable.termsReview` in `installer\license-policy.json`:
  `status` "reviewed", the date and what the conditions require; while it is "pending" packaging
  warns and step 7 prints it); and the acceptance of the sources verified by pinned values instead of
  a source archive or the vcpkg registry (the SHA-256 values of the MSVC headers, Inno Setup's
  `license.txt` and the Windows SDK's `cppwinrt\LICENSE.txt`, and the git blob ids of the files in
  `installer\third-party`), with the elections and judgements the ledger lists there. **Open for
  0.1.0.** Record them in a commit and start again at step 1.

**Pass:** no difference, and the owner's decisions recorded; otherwise update the ledger and the
policy in a commit and start again at step 1.

## 9. Installer on the release machine

```powershell
$pkg = 'build\rel-<v>\package'
$fixture = 'fixtures\cache\canon-eos-r5-cr3-raw\Canon_EOS_R5_RAW_ISO_100_nocrop_nodual.CR3'
powershell -NoProfile -File installer\tests\Test-InstalledPackage.ps1 -Installer "$pkg\Lucent-Setup-<v>-x64.exe" `
    -Manifest "$pkg\Lucent-Setup-<v>-x64.manifest.json" -Fixture $fixture -RuntimeOutcomes `
    -NewerInstaller "$pkg\rehearsal\Lucent-Setup-<next patch>-x64-VERSION-REHEARSAL.exe"
# The update rehearsal, from the previous release's archived Setup:
powershell -NoProfile -File installer\tests\Test-InstalledPackage.ps1 -PreviousInstaller <archive>\<prev>\Lucent-Setup-<prev>-x64.exe `
    -PreviousManifest <archive>\<prev>\Lucent-Setup-<prev>-x64.manifest.json `
    -Installer "$pkg\Lucent-Setup-<v>-x64.exe" -Manifest "$pkg\Lucent-Setup-<v>-x64.manifest.json" -Fixture $fixture
```

**Pass:** both exit with 0 and report every check passed (the first release has no rehearsal). The
first run installs, repairs the installation over the running Lucent (same version), upgrades it to
the version rehearsal, and checks that the release's Setup then refuses to install over the newer
version (exit code 1, `Downgrade refused` in its log, nothing changed; W12 review SEC-04); the update
rehearsal checks that the previous release's Setup refuses to go back too.
Lucent must not be installed for the user running them (the script refuses otherwise).

## 10. Clean machine, offline (Windows Sandbox)

The blueprint's "Offline installation" gate: a clean Windows without Python, Qt, Git, a compiler,
a development `PATH` or the Visual C++ runtime installs Lucent, which then imports, edits and exports,
offline. [The clean-machine gate](clean-machine.md) describes it in full: how to enable Windows
Sandbox once, what the gate checks, the options and what it cannot show.

**Status:** Windows Sandbox is enabled on the reference machine. The first run (2026-10-11, a release
candidate) went through every step and failed; the defects it showed are fixed (2026-10-15,
[clean-machine.md](clean-machine.md)), and the gate has not passed yet. Until it has passed for a
release, that release is not published.

**The automatic run:**

```powershell
powershell -NoProfile -File tools\Test-CleanMachine.ps1 -PackageDir build\rel-<v>\package
```

It copies Setup, its manifest, its version rehearsal (step 6 with `-VersionRehearsal`) and three
smoke fixtures into a gate folder mapped read-only into the sandbox, with a writable results folder;
the sandbox has no network, GPU, clipboard or printers (the configuration is validated against
`installer\sandbox\wsb-configuration.xsd` first). Windows Sandbox runs one sandbox at a time: the
script refuses to start while one is open, and stops with a diagnosis when the gate has not started
within five minutes. At logon `installer\sandbox\Invoke-CleanMachineGate.ps1` records that the
machine is clean and offline, and its own token and UAC settings, installs Setup silently (it installs
Microsoft's redistributable for real; the sandbox runs the gate elevated, so no elevation prompt is
expected), runs the installed `Lucent.exe` to import the fixtures, run Auto on one, export them as
JPEG and as 16-bit TIFF and save screenshots, uninstalls, runs `Test-InstalledPackage.ps1
-RuntimeOutcomes -NewerInstaller`, writes `gate.json` and shuts the sandbox down. The host checks the
result itself and writes its verdict beside the gate folder's `gate-request.json`.

**The manual run:** the four steps of [clean-machine.md](clean-machine.md#the-manual-steps) (a declined
elevation prompt, the wizard, an import, edit, Auto and export through the user interface,
uninstalling from Settings). The sandbox elevates without asking, so steps 1 and 2 run on a Windows
with UAC at its default and without the Visual C++ runtime (a virtual machine installed from Windows'
own media, or a spare computer), offline; steps 3 and 4 may run there too, or in a sandbox opened with
`tools\Test-CleanMachine.ps1 -PackageDir build\rel-<v>\package -Manual`.

**Pass:** the automatic run prints `Clean-machine gate: PASSED` and exits with 0
(`<gate>\host-check.json`, beside `gate-request.json`, says `"passed": true`; a `host-check.json` in
`results\` is not the host's verdict), and the manual steps behave as described. Keep the gate folder's
`results`, `gate-request.json` and `host-check.json` (with the date, the host's Windows build and the
Windows Sandbox version), and the record of the manual run with the machine it used (Windows edition,
version and build, virtual or physical, its UAC setting), in the release archive.

## 11. Signing (when a certificate exists)

Not part of releases yet: they are unsigned, and Windows SmartScreen warns before Setup runs (the
release notes say so and give the installer's SHA-256). With a code-signing certificate from a
certification authority, its private key in `Cert:\CurrentUser\My` and the authority's RFC 3161
timestamp server:

```powershell
$env:LUCENT_SIGN_CERT_SHA1 = '<thumbprint>'; $env:LUCENT_SIGN_TIMESTAMP_URL = '<timestamp URL>'
powershell -NoProfile -File tools\package.ps1 -BuildDir build\rel-<v> -Fresh -RunTests -Jobs 6 -UpdateChannel none
```

**Pass:** the script verifies every signature it makes (Lucent.exe, lucent-worker.exe, Setup) and
the manifest records `signed: true`, `signing.test: false`, `signing.timestamped: true`; step 9 with
`-SignerThumbprint <thumbprint>` passes, which also checks the uninstaller. Change
`installer\license-policy.json` `distribution.signing.release` to `signed` in the release commit:
from then on `tools\package.ps1` refuses to build a Setup without a release signature (and it warns
while a release-signed package's policy still says `unsigned`). Never publish a `-TEST-SIGNED`
package.

## 12. Update channel (signed releases only)

Rules:

- A release is offered through "Check for updates" only when it is signed for release and its
  `Lucent.exe` checks the channel it is published on: `New-LucentReleaseManifest` refuses anything
  else, and it refuses a package whose corresponding-source archive is not beside the installer with
  the recorded size and SHA-256.
- While releases are unsigned there is no channel: package with `-UpdateChannel none`; users update
  by installing the next Setup over the old one (the upgrade path of step 9's rehearsal).
- A channel is added once, in a reviewed commit of `installer\update-channels.json`, with its release
  manifest key and the publisher certificate's thumbprint
  ([setup](../development/setup.md#an-update-channel-and-signing-for-a-release),
  [ADR 0022](../adr/0022-updates-and-migrations.md)); a build for it is configured with the
  `LUCENT_UPDATE_*` values and packaged with `-UpdateChannel <name>`.

For a signed release on channel `stable`:

```powershell
Import-Module .\installer\scripts\LucentUpdate.psm1
$pkg = 'build\rel-<v>\package'
New-LucentReleaseManifest -Installer "$pkg\Lucent-Setup-<v>-x64.exe" -PackageManifest "$pkg\Lucent-Setup-<v>-x64.manifest.json" `
    -InstallerUrl '<https URL of the installer>' -Channel stable -ReleaseNotesFile notes.txt -OutFile "$pkg\release-manifest.json"
Protect-LucentReleaseManifest -Manifest "$pkg\release-manifest.json" -KeyName '<key name>'
Test-LucentReleaseManifest -Manifest "$pkg\release-manifest.json" -PublicKey '<public key>' -Installer "$pkg\Lucent-Setup-<v>-x64.exe"
```

**Pass:** `Test-LucentReleaseManifest` succeeds. Upload in this order: the installer and the
corresponding-source archive, then `release-manifest.json.sig`, then `release-manifest.json`, so
that no client is offered an installer whose files are not all in place.

## 13. Publish

First push the release commit and its tag to the public repository: the LGPL notice, the archive's
README and the SBOM point at Lucent's source at that commit.

```powershell
git tag -a v<v> -m "Lucent <v>"
git push origin main v<v>
```

Then publish the files of [What is published](#what-is-published), together, on the release's page
(the repository's release for tag `v<v>`, or wherever the installer is offered): the
corresponding-source archive sits next to the installer, from the same place, for as long as the
installer is offered (ADR 0024). The release notes state: the installer's and the archive's
SHA-256, that the installer is unsigned (until step 11 applies) and what SmartScreen shows, the
supported Windows versions, the camera support summary, known limitations (for example that the
Natural v1 automatic settings are not yet calibrated), and that the corresponding source of the
LGPL libraries is the archive beside the installer.

**Pass:** the page lists the installer and the archive side by side; downloading both and checking
`Get-FileHash` gives the SHA-256 values of the package manifest.

## 14. Archive

Copy to the release archive (outside the repository; kept at least as long as any release it holds
is offered, and in practice permanently): the published files, plus the evidence that is not
published.

**Pass:** the archive's folder `<v>\` holds everything listed in [What is archived](#what-is-archived),
and its copy of the installer and the archive have the manifest's SHA-256.

## What is published

| File | From | Why |
|---|---|---|
| `Lucent-Setup-<v>-x64.exe` | `build\rel-<v>\package` | The installer |
| `Lucent-Setup-<v>-x64.exe.sha256` | same | Its checksum line |
| `Lucent-<v>-corresponding-source.zip` | same | The corresponding source of the LGPL libraries (LGPL-2.1 section 4; GPL-3.0 section 6(d), which LGPL-3.0 incorporates); named in the installation's `licenses\LGPL-NOTICE.txt` |
| `Lucent-Setup-<v>-x64.manifest.json` | same | The package manifest: every installed file with its SHA-256, the archive's files, the distribution and signing records |
| `Lucent-Setup-<v>-x64.spdx.json` | same | The SBOM (SPDX 2.3), also inside the installation |
| Release notes | written for the release | See step 13 |
| `release-manifest.json` and `.sig` | step 12 | Only for a signed release on an update channel |

## What is archived

Everything published, and: `audit.txt` and `audit.json`, `builder-paths.txt` and `.json`,
`iscc.log`, `install.log` and the `smoke\` folder of the package run; the clean-machine results of
step 10; the test logs of steps 3 and 9 (`Testing\Temporary\LastTest.log` of each build tree); the
qualification report and support table of step 4 (also committed); the benchmark data of step 5;
the release commit's hash and the tag. The corresponding-source archive already holds the upstream
source archives and recipes, so no separate copy of vcpkg's downloads folder is needed.

## Blueprint release gates

Where each gate of the blueprint (section 10, "Required release gates") is checked:

| Blueprint gate | Evidence | Status |
|---|---|---|
| Camera correctness | Step 4 (qualification, colour reference and rotated files, ADR 0016 and 0020), step 3 (fixture tests) | Checked every release |
| Natural automatic results | Blind preference and correction-effort evaluation (ADR 0011) | **Not yet run** (roadmap W8); until it is, the release notes call the automatic settings uncalibrated |
| Learned-method promotion | No learned method ships (ADR 0019); step 8 checks that no model asset does | Not applicable |
| Numerical colour/model behaviour | Step 3 (unit and render tests) | Checked every release |
| Preview/detail fidelity | Step 3 (tolerances of ADR 0010 and 0015) | Checked every release |
| Original preservation | Step 3 (source-hash tests), step 5 (the harness compares the fixtures' SHA-256 after every run) | Checked every release |
| Batch resilience | `tools\bench\Invoke-BatchResilience.ps1` (the 1,000-file run, ADR 0021) | Run when the batch, worker or catalog code changed since the last pass; recorded in the release archive |
| Restart/publication | Step 3 (worker and publication tests), the resilience run | As above |
| Offline installation | Step 10 | **Pending:** the gate has not passed yet (its first run, 2026-10-11, failed on defects since fixed) |
| Update compatibility | Step 3 (catalog fixtures of every schema), step 9 (the update rehearsal) | Checked every release (the rehearsal from the second release on) |
| Distribution | Steps 6, 7 and 8 | Checked every release |
