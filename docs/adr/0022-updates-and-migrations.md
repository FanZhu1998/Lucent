# ADR 0022: Updates and migrations: an offline-first "Check for updates" over a signed release manifest, catalog fixtures of every schema version, and the update rehearsal

- **Status:** Accepted
- **Date:** 2026-10-09
- **Workflow:** W12 (release hardening), updates and migrations
- **Amends:** ADR 0008 (a pre-update backup kind), ADR 0009 (the update handoff to Setup, the
  upgrade rehearsal from a previous installer), ADR 0012 (batches frozen with version-1 recipes after
  an update), ADR 0018 (what an update does to camera support data)

## Context

Blueprint section 8 asks for "a signed full installer and a graphical **Check for updates** flow,
with a complete offline installer also available", and to "Verify downloaded installers and
standalone profile/model packs against an authenticated release manifest or signature tied to the
trusted publisher identity before activation; hashes alone do not authenticate a download. Apply
application updates between jobs and retain migration recovery information. No cloud account or
network connection is required for opening, editing, or exporting." Migrations "run transactionally
after backing up the catalog and have explicit compatibility tests. Never interpret a newer unknown
operation as a no-op." Section 9 signs the installer and binaries with Authenticode (ADR 0009). The
section 10 gates:

| Gate | Passing evidence |
|---|---|
| Update compatibility | Prior catalog/recipe fixtures migrate safely; unknown newer operations are reported; old missing assets are not silently replaced |
| Distribution | Exact shipped dependencies, models, profiles, and notices pass the chosen licensing and packaging review |

What existed before this ADR (2026-10-09):

- Lucent never opened a network connection. There was no update mechanism; a new version could only
  be installed by running its Setup, which upgrades in place (ADR 0009, decision 8).
- The catalog migrates older schemas after a verified pre-migration backup and opens a newer one
  read-only when its minimum reader version allows (ADR 0008, decision 3). Migrations were tested by
  `migration_v2_test.cpp` and `migration_v3_test.cpp` on catalogs that each test builds itself; no
  catalog file of an older schema was kept, and nothing checked that a released migration's DDL was
  never edited.
- Recipes of a newer document version, operation version or base look make an item read-only
  (`recipe_service.hpp`), and the camera support database is activated only when its manifest hashes
  to the build's pin (ADR 0018). Neither was tested against catalogs as earlier builds left them.
- Authenticode signing hooks exist and were rehearsed with a test certificate (ADR 0009, amendment);
  no release certificate exists. Windows' CNG offers ECDSA P-256 (`BCRYPT_ECDSA_P256_ALGORITHM`),
  and .NET's `ECDsaCng` on Windows PowerShell 5.1 signs and verifies with the same provider (both
  verified on the reference machine, 2026-10-09, with the RFC 6979 P-256 test vectors).

## Decision

### The update channel

1. **Offline first.** Lucent contacts its update server only when the photographer clicks **Check for
   updates** (Help > About), and only when the build has an update channel. There is no check at
   start-up, no background polling and no telemetry. A build without a channel (the default, and
   every build so far) says "Updates are not configured in this build" and never opens a connection:
   `UpdateService::check()` fails with Unsupported (`app.update.not_configured`) before any I/O.

2. **The channel is fixed at build time** (`src/application/CMakeLists.txt`, `update_channel.hpp`):
   `LUCENT_UPDATE_CHANNEL` (name), `LUCENT_UPDATE_MANIFEST_URL` (https), `LUCENT_UPDATE_PUBLIC_KEYS`
   (pinned ECDSA P-256 keys) and the optional `LUCENT_UPDATE_PUBLISHER_THUMBPRINTS` (Authenticode
   signer certificates), CMake cache variables defaulting to the environment variables of the same
   names at the first configure. The first three must be set together; CMake refuses a partial or
   malformed configuration (a channel name that is not `[a-z0-9]` words joined by `-`, a URL that is
   not https or carries user information, a key that is not 130 hex digits starting with `04`, a
   thumbprint that is not 40 or 64 hex digits), and `make_update_channel()` checks it again at run
   time, the keys through CNG (a point off the curve is refused). The values are compiled into
   `Lucent.exe` (a generated `update_channel_config.cpp`), which Authenticode protects: nothing at run
   time, no setting, file or command-line option, changes the channel or its keys. More than one key
   is pinned only while a key is being replaced.

3. **The release manifest and its signature** (`update_manifest.hpp`). The channel serves
   `release-manifest.json`, "lucent.release-manifest" version 1: product `Lucent`, channel, version
   (SemVer), publication time (UTC), architecture `x64`, minimum Windows (`major.minor.build`), the
   installer's URL, exact size (at most 2 GiB) and SHA-256, and release notes (plain text, at most
   16 KiB, shown as text, never as markup); and beside it `<manifest URL>.sig`,
   "lucent.release-signature" version 1: algorithm `ecdsa-p256-sha256`, the key id (SHA-256 of the
   key's 65-byte uncompressed point) and the IEEE P1363 signature `r || s` (base64) over the
   manifest's exact bytes. Readers are strict: an unknown, missing or repeated member or a malformed
   value is Corrupt, a newer schema version VersionMismatch (a format change therefore needs a new
   channel URL while older clients exist). The manifest is at most 64 KiB, the signature 4 KiB.

4. **Authenticate, then interpret** (`verify_release_manifest()`). The signature document is read,
   the pinned key with its key id is found (none: PermissionDenied `app.update.signature.unknown_key`),
   the signature is verified over the manifest's bytes with CNG (`BCryptVerifySignature`; a failure is
   PermissionDenied `app.update.signature.mismatch`), and only then is the manifest parsed. A signed
   manifest of another channel is refused (`app.update.manifest.channel`), so a beta release cannot be
   replayed on the stable channel. A release not newer than the running build is "up to date": no
   downgrade is ever offered. A newer release whose minimum Windows is newer than this machine's
   (`RtlGetVersion`) is reported and not offered.

   ECDSA P-256 with SHA-256 because Windows CNG implements it on every supported Windows (no new
   dependency), the publisher's tooling signs with the same provider from Windows PowerShell 5.1, keys
   can live in a key storage provider or on a smart card or hardware token without being exportable,
   and keys and signatures are small enough to pin in the build and serve beside the manifest.

5. **Transport** (`http_client.hpp`): WinHTTP in asynchronous mode (a cancelled request's handle is
   closed without racing a call in progress; the caller's thread waits with a cancellation check
   every 50 ms), https only (`UrlPolicy::HttpsOnly`), TLS 1.2 or 1.3 with WinHTTP's certificate
   validation never relaxed, the system's automatic proxy configuration, timeouts per step, no
   cookies, authentication or cache. Redirects are followed by Lucent, at most five, each target
   checked against the policy before anything is sent there, so an https address cannot lead to an
   http one. A body larger than allowed stops the transfer (LimitExceeded), one shorter than its
   Content-Length is Corrupt. Tests use `UrlPolicy::LoopbackOnly` (http or https to 127.0.0.1 or ::1
   only, no proxy), so a test can never reach another machine.

6. **Download and verification** (`UpdateService::download()`). The installer is downloaded into the
   per-user cache, `<cache>\updates`, under a temporary name opened exclusively, streaming at most the
   manifest's size; the complete file is renamed `Lucent-Setup-<version>-x64.exe` and opened for
   reading with writers, renamers and deleters excluded (`FILE_SHARE_READ` only; the handle itself can
   only read, so Setup and the loader still open the file). Through that handle Lucent checks the
   size and SHA-256 the signed manifest names and, when the build names publisher thumbprints, an
   Authenticode signature that verifies and chains to a trusted root (`core::check_authenticode()`,
   WinVerifyTrust through the handle, no revocation check, no network) by one of those certificates
   (SHA-1 or SHA-256 thumbprint). A file that fails is deleted. The returned `DownloadedInstaller`
   keeps the handle until Setup is started: nobody can change the verified file meanwhile. Earlier
   downloads are deleted first, except a complete one of the same release, which is checked the same
   way and reused.

7. **Installation, between jobs** (`UpdateService::install()`, the Backend's blocker and backup):
   - it waits while an export batch runs (`BatchService::list_unfinished()`, state Running) or Auto
     runs (`AutoService::runs_in_progress()`): AlreadyExists `app.update.busy`, which the shell words as
     "Lucent installs updates between jobs". A paused batch does not block: it continues after the
     update (decision 13);
   - the held file's size and SHA-256 are checked once more;
   - **migration recovery information**: a verified pre-update backup of the writable catalog
     (`Catalog::create_backup(BackupKind::PreUpdate)`, `<catalog>.<time>.v<schema>.pre-update.bak`,
     five kept, pruned on their own); an error stops the installation (`app.update.prepare`). The new
     version's first start then migrates the catalog after its own pre-migration backup (ADR 0008);
   - Setup is started with `CreateProcessW` (no inherited handles), graphical (not silent: the
     photographer sees the familiar wizard, which upgrades the installation in place and offers
     "Launch Lucent"), with `/LOG=<cache>\logs\setup-<version>.log`;
   - the shell then exits cleanly (`AppController::quitRequested`, `QCoreApplication::exit(0)`): the
     services stop, running batch jobs are given back, the catalog checkpoints and closes. Setup's
     Restart Manager closes a Lucent that is still running (`CloseApplications=yes`, ADR 0009).

8. **The shell** (`UpdateModel`, `UpdatePanel.qml` in Help > About; `AppUpdateService`). States:
   not configured, idle, checking, up to date, available (version, publication date, size, release
   notes), needs a newer Windows, downloading (progress, cancellable), ready (verified; "Install and
   restart" enabled only between jobs, re-checked every second, with the reason shown otherwise),
   installing, failed (the reason in the photographer's words: `user_facing_update_error()`). Network,
   disk and the backup run on the adapter's worker thread, never the UI thread.

9. **The publisher's tooling** (`installer\scripts\LucentUpdate.psm1`): `New-LucentUpdateSigningKey`
   creates a persisted, non-exportable ECDSA P-256 key in the current user's key storage provider
   (never in the repository) and prints the public key a build pins; `New-LucentReleaseManifest`
   writes the manifest of a packaged Setup in Lucent's canonical form (it refuses an http URL, a bad
   channel name, a version that is not SemVer, notes above 16 KiB); `Protect-LucentReleaseManifest`
   signs it in place with the key; `Test-LucentReleaseManifest` verifies a release as Lucent will
   (signature, members, and optionally the installer's size and SHA-256). Publishing order: the
   installer, then the signature, then the manifest.

### Migrations and compatibility

10. **Catalog fixtures of every historical schema version**, committed in
    `src/catalog/tests/data/migration` (`v1.catalog` ... `v5.catalog`, 57 to 287 KB; `fixtures.json`
    with their SHA-256 and row counts; `README.md`). They are generated by driving the older migration
    states (`tests/catalog_fixtures/generate_catalog_fixtures.cpp`): each era's content is written
    through the catalog's API and the application's own document writers into a scratch catalog,
    with that era's documents (W6 to W8's `lucent.develop` version 1 recipes, W7's batch policy
    without the members its closing review added, receipts without W10's support state, W9 and W10's
    version-2 recipes, real analysis documents and records from `run_automatic()`, camera support
    records naming the database W10 shipped); the fixture file is created by exactly the migrations a
    build of that version ran (`builtin_migrations().first(v)`), and every table is filled with the
    columns that version has, with foreign keys and the schema's triggers enforced.
    `tests/catalog_fixtures/check_historical_ddl.py` verifies against git history that each
    migration's DDL is unchanged since its introducing commit (b2951f7, 0491d19, 6711a6a, 93f72ca,
    7df00b8) and that applying them with another SQLite (Python's) gives exactly each fixture's
    schema. Fixtures never change; a new schema version adds its own (the generator's `static_assert`
    and a test fail until it does).

11. **What the gate tests** on copies of the fixtures:
    - the catalog (`migration_fixture_test.cpp`): each fixture's schema is exactly what that
      version's migrations create (a released migration is never edited); a copy migrates after a
      verified pre-migration backup holding the fixture as it was (and restoring to it); every value
      of every column that existed is unchanged; added columns hold their defaults; integrity and
      foreign keys hold; everything reads through the API; the catalog accepts writes; a catalog of a
      newer Lucent opens read-only and says why ("schema v6 (written by Lucent 9.0.0) ... opened
      read-only"), is never written or backed up, and one this build cannot read is refused;
    - the application (`update_compatibility_test.cpp`): every migrated fixture loads as the library
      (items, support states, flags, duplicates, missing and changed files), every recipe of versions
      1 and 2 reads as the settings it rendered, every batch document, receipt, analysis document and
      record reads with this build's readers; recipes of a newer Lucent (a newer document version, an
      operation version this build does not implement, an operation it does not define) make the item
      read-only with that reason, `current_settings()` fails, so previews, exports and batches refuse
      it instead of rendering it without the unknown part, edits fail, and undo returns to a recipe
      this build reads; a camera support database missing after an update, a replaced file, or a file
      and manifest replaced consistently is never substituted (an exact copy of the genuine database as
      an unsigned data pack is refused too): no item becomes Verified, Limited and Unsupported answers
      stay, records keep the 2.0.1 answer, and only the genuine bundled database re-assesses the items,
      recording its version.

12. **Recipes and assets of a newer Lucent are reported, never no-ops** (unchanged rules, now
    tested on migrated fixtures): `read_recipe()` refuses unknown operations and newer versions;
    `RecipeService` reports the item read-only with `app.recipe.unreadable`; ADR 0018's pin and
    refusal of unsigned data packs keep an old or missing asset from being silently replaced. Signed
    data packs remain future work: the release-manifest signature is the mechanism they will use.

13. **Batches frozen before an update still export** (amends ADR 0012, decision 10). The gate found
    that every unfinished batch frozen by W6 to W8 failed after updating to a W9 or later build with
    "the recipe frozen for this job cannot be reproduced by this version of Lucent": those builds froze
    jobs with `lucent.develop` version-1 documents (a revision's, or the as-shot settings'), whose
    canonical text this build no longer writes. `frozen_document()` now accepts the job's frozen
    recipe when the job's revision is exactly the frozen one (the revision is immutable and the
    catalog checked its text against its SHA-256, which is the job's), or when the job froze version
    1's as-shot document (`kVersion1AsShotRecipe`), and renders this build's canonical form of the
    same settings, which renders exactly as the frozen document (recipe.hpp). The receipt records the
    canonical document rendered; the job's identity is unchanged.

14. **The update rehearsal** (amends ADR 0009, decision 11). `Test-InstalledPackage.ps1
    -PreviousInstaller <Setup> -PreviousManifest <manifest>` installs the previous version first (per
    user, into a temporary folder), runs its Lucent.exe to import the fixture into the data root,
    upgrades in place with the new Setup while that Lucent runs on that data root (as an update from
    Check for updates does), checks the files, the installed-file list and the signatures of the
    upgraded installation, runs the new Lucent.exe on the previous version's library (it must open the
    catalog for writing, after a pre-migration backup when it migrates, find the photo and exit with
    0), uninstalls and checks that nothing is left.

## Consequences

Measured on the reference machine (Ryzen 5 5500U, 11.9 GB), 2026-10-09, `dev` build unless stated:

| What | Result |
|---|---|
| Checking the channel over loopback (manifest, signature, verification), tests of several checks each | 20 to 23 ms per test |
| Verified download of a 300 KB installer over loopback, rename, SHA-256 through the held handle | 56 ms |
| Cancelling a stalled download | Cancelled, nothing left in the folder; the whole test takes 92 ms |
| A refused connection / a server answering after the 0.5 s receive timeout | IoError (`app.update.network`) / Timeout |
| Catalog fixtures v1 ... v5 | 57,344 / 81,920 / 176,128 / 188,416 / 286,720 bytes; regenerated byte for byte in 2 s; every migration's DDL unchanged since its commit, every fixture schema identical |
| Migrating each fixture (copy, verified backup, migrations, comparison of every value) | 105 to 373 ms per version |
| New tests | core 9, application 37 (transport and update service 19; manifest, channel and URLs 10; compatibility 7; frozen batch 1), catalog 14, UI 7, installer 2 PowerShell self-tests (73 checks) |
| Full suite, `tools\build.ps1 -Preset dev -Test` | 2,188 tests: 2,127 passed, 57 skipped (their camera fixtures are not in the local cache), 4 disabled benchmarks, none failed; 10 to 12 minutes (two runs) |
| `tools\package.ps1 -StageOnly`, release | passed (563 s): 308 files, 63.1 MiB, no build-machine path in any file; Lucent.exe now imports WINHTTP.dll, WINTRUST.dll and CRYPT32.dll from Windows (the last two are new to the package) |
| Setup, release, unsigned | 40.0 MiB |
| The update rehearsal: the previous layout's Setup (built from d95c5c1, 308 files) upgraded in place by this one (66 files changed, none added or removed), per user, in a temporary folder | 43 of 43 checks passed: Restart Manager closed the previous Lucent running on its library; the upgraded Lucent opened that catalog (schema v5) for writing and its photo in Edit; the uninstallation left nothing behind |

Both versions in the rehearsal are 0.1.0 with the same catalog schema: it rehearses Setup's
in-place upgrade of an earlier layout and a library the earlier build wrote, while schema migrations
are rehearsed by the fixtures (decisions 10 and 11).

- Lucent stays offline unless the photographer asks, and a build without a channel cannot contact
  anything: the gate "Offline installation" is unaffected.
- A release needs: a Setup built by `tools\package.ps1` (signed for public distribution, ADR 0009),
  its manifest and signature made with `LucentUpdate.psm1` and the publisher's key, and the three
  files on an https server. A build that checks a channel needs the channel's values at configure
  time. No production channel or key exists yet; this build is not configured.
- An attacker on the network can withhold updates (block the server, or replay an older signed
  manifest: it is never offered as an update, but a newer release stays hidden); there is no
  freshness guarantee (no expiry or timestamp role as in TUF). A key compromise is handled by
  shipping a build that pins a new key; until then the Authenticode check (when configured) and the
  manifest's SHA-256 still bind the installer to the publisher.
- Pinning certificate thumbprints means a certificate renewal must be pinned in a build before
  installers are signed with it (several thumbprints may be pinned). Authenticode revocation is not
  checked (decision 6): the manifest signature is the primary authentication.
- A downloaded installer is a full Setup (about 40 MB): no delta updates. Setup is graphical; Lucent
  starts again through its "Launch Lucent" option, not by itself.
- Updates are applied between jobs, but a paused batch continues after the update, and batches frozen
  with version-1 recipes now export (decision 13). A future recipe version must keep reading every
  older version (as `read_recipe()` does) or these batches fail again; the frozen-batch test
  (`BatchUpdate.JobsFrozenWithVersionOneRecipesExportAfterTheUpdate`) catches that.
- Each schema version adds a committed fixture; the migration tests then cover it automatically.
- Revisit for signed data packs (decision 12), a freshness role (expiry, a timestamp file), delta
  updates, or an all-users installation.

## Alternatives considered

- **Check for updates at start-up or in the background.** Rejected: the blueprint's product is
  offline; a check is a network request the photographer did not ask for.
- **Trust https alone (no manifest signature).** Rejected by the blueprint ("hashes alone do not
  authenticate a download"): TLS authenticates a server, not the publisher; a compromised server or
  mirror could serve any installer.
- **Authenticode alone (no manifest).** Rejected: it says nothing about which version is current,
  for which channel, Windows version or architecture, and needs a certificate; it is used as an
  additional check when the build names the publisher's certificates.
- **RSA or Ed25519 signatures.** RSA keys and signatures are several times larger for no benefit
  here; Ed25519 is not available through CNG on every supported Windows. ECDSA P-256 is native to
  CNG and to hardware tokens.
- **minisign / signify, TUF or a third-party updater (WinSparkle, Squirrel, MSIX/App Installer).**
  Rejected for version 1: each adds a dependency, a trust model or an installation technology beside
  Inno Setup (ADR 0009); TUF's freshness guarantees are noted as future work.
- **A silent installation with automatic restart.** Rejected: the photographer should see what
  replaces the program; the graphical wizard is the same one a manual installation shows.
- **Downloading into %TEMP% or verifying by path.** A path verified and then launched leaves a
  window in which the file can change; the held handle that excludes writers closes it, and the
  per-user cache keeps downloads beside Lucent's other disposable files.
- **Fixtures built by each test from today's code** (as before). They cannot show that a released
  migration was edited, and depend on today's writers; committed files of every version plus the
  history check do.
- **Fixtures written by building each historical commit.** Faithful, but five builds of an older
  tree (and its dependencies) for data the migration prefix and the current writers reproduce, with
  the DDL verified against git history; documented as the method's limit (the rows are this build's
  code for those columns).
- **Failing frozen batches with version-1 recipes after an update** (as before). Rejected: the
  conversion is lossless and render-identical, and losing queued work at an update contradicts
  "Apply application updates between jobs".
