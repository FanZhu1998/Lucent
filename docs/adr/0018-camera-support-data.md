# ADR 0018: Camera support data: versioned asset manifests, the camera support database and the four support states

- **Status:** Accepted (amended on 2026-10-08 by the closing review of W10: database schema 2 and
  support rules 2, see "Amendment: closing review" below)
- **Date:** 2026-10-08
- **Workflow:** W10 (Multi-brand qualification), phase 3: support data
- **Amends:** ADR 0016 (decision 1: the mode key moves to `lucent_assets`; decision 6: the report
  records each mode's probe key; Consequences: the application consumes the states; see its
  amendment), ADR 0008 (what an item's support columns hold), ADR 0009 (the installed layout gains
  `assets\`, audited), ADR 0012 (receipts and reports record the support state; Limited exports
  name their limitation)

## Context

Blueprint section 3 defines four user-visible states: *Verified* ("exact model and relevant
capture mode passed Lucent's decoding, color, geometry, and export tests"), *Compatible,
unverified* ("decoder and required interpretation steps succeed, but Lucent lacks representative
quality fixtures; show the status and permit conservative processing"), *Limited* ("a known
optional feature or profile is unavailable; describe the limitation and available conservative
path") and *Unsupported* ("decoding or a mandatory interpretation step is unavailable or ambiguous;
skip automatic export and give a useful reason"). "A missing required color interpretation,
unsupported CFA, or mandatory DNG opcode cannot silently become a verified image. Unknown future
models may work through a known representation and valid metadata."

Section 8 versions the "camera/profile database" independently and lets "new supported camera
profiles ... arrive as versioned data packs when the existing engine already understands the
file", verified "against an authenticated release manifest or signature tied to the trusted
publisher identity before activation; hashes alone do not authenticate a download". Its location
table puts "read-only bundled profiles/models, licenses, asset manifest" in the installed
application directory and "installed asset versions" in per-user application data. Section 10's
update gate: "old missing assets are not silently replaced".

Before this phase the application derived a three-valued status from the decoded-RAW contract's
interpretation alone (`Supported`, shown as *Compatible, unverified*; `Limited`; `Unsupported`):
nothing was ever Verified. Phases 1 and 2 (ADR 0016, ADR 0017) produced the evidence: the
qualification report of 34 fixtures proposes Verified 14, Compatible, unverified 11, Limited 6,
Unsupported 3 modes, each keyed by its mode key.

Facts that shaped the decisions (2026-10-08):

- The application assesses a file when it imports it, from a probe (metadata stage AfterOpen); the
  harness keyed its evidence by the decoded file's mode key (AfterUnpack). For 33 of the 34
  fixtures the probe derives the same key. For the Canon EOS 5D Mark IV S-RAW the probe's white
  level gives 15 bits, the decode's 14 (LibRaw sets the final level while unpacking), so a lookup
  by the qualified key would miss it (`QualificationFixture.ProbeKeyFindsTheQualifiedState`).
- The catalog stores an item's status as one of three spellings ("supported", "limited",
  "unsupported"); every reader since schema v1 knows exactly those and reports Corrupt on another
  (`lucent/catalog/types.hpp`). The reasons column is application-owned JSON whose readers keep
  unknown element kinds as generic reasons (`catalog_mapping.hpp`), and the shell shows reasons
  only for Limited and Unsupported items. The catalog is not part of this phase.
- The installer is per-user: `%LOCALAPPDATA%\Programs\Lucent` is writable by the user, as is the
  per-user data folder. A manifest of SHA-256 values in the same folder as the files only detects
  corruption; whoever can rewrite a file can rewrite the manifest. The executable is what
  Authenticode signing (ADR 0009, W12) protects.
- The bundled database is about 27 KB (34 entries); reading, hashing and parsing it at start-up
  takes a few milliseconds (`BundledCameraSupport.VerifiesAndParses` runs in under 10 ms), and
  re-assessing a library from stored records needs no file access and one catalog transaction.

## Decision

1. **`lucent_assets`** (`src/assets`, layered after `analysis`, before `workers` and
   `application`; nlohmann-json private). An asset is a folder with a manifest,
   `manifest.json`, schema `lucent.asset-manifest` version 1 (`asset_manifest.hpp`): `id`
   (`[a-z0-9]` words joined by `-`), `version` (SemVer of the content: any change of a file is a
   new version), `minimum_app_version` (SemVer), `content` (the schema name and version of what the
   files hold) and `files` (relative `/` paths without `.`/`..` segments, drive letters or colons,
   unique ignoring case; each with its exact size and lower-case SHA-256). Readers are strict: an
   unknown member or a malformed value is Corrupt, a newer `schema_version` VersionMismatch; the
   writer is canonical (members in a fixed order, files sorted, one-space indentation, LF, final
   newline). Bundled assets live in the source tree's `assets/<id>/`, whose `.gitattributes`
   (`* -text`) keeps git from converting their line endings, so the bytes the manifest hashes are the
   bytes every checkout has.

2. **Verification before activation** (`asset_store.hpp`, `verify_asset_folder()`), on bytes read
   once into memory (what is verified is what is parsed): the manifest parses and names the
   requested id (`assets.load.id`); for the bundled asset its bytes hash to the **pin**
   (`assets.load.pin`); the running Lucent is at least `minimum_app_version`
   (`assets.load.app_version`, VersionMismatch); every listed file exists (`assets.load.missing`,
   NotFound), is at most 64 MiB and has exactly its size and SHA-256 (`assets.load.hash`, Corrupt).
   Unlisted files are never read. The pin is the SHA-256 of each bundled manifest, recorded when the
   build is configured (`src/assets/CMakeLists.txt`, generated `bundled_assets.cpp`;
   `bundled_manifest_pin()`): it ties the data to the executable that signing protects, so a
   consistent rewrite of a file and its manifest is still refused. Editing a manifest reconfigures
   the build.

3. **Locations and lookup order** (`find_asset()`, blueprint section 8): (1) per-user data packs,
   `<per-user data>\assets\<id>` (`%LOCALAPPDATA%\Lucent\data\assets`), then (2) the bundled asset,
   `<folder of the executable>\assets\<id>` (build trees: `<build>\bin\assets`, copied by
   `src/assets/CMakeLists.txt`). The per-user location is reserved for signed data packs: this
   version cannot verify a publisher's signature, so a pack found there is **refused**
   (`assets.load.unsigned`, Unsupported), reported as a refused candidate (with the version its
   manifest declares) and never replaces the bundled asset; Lucent never writes that folder. A
   bundled asset that fails verification is not replaced by any other copy: nothing is activated,
   and the lookup reports why.

4. **The camera support database** (`camera_support.hpp`): the asset `camera-support`, one document
   `camera-support.json`, schema `lucent.camera-support` version 1: `qualification` (the run's UTC
   time and date, the tool and its commit, the report's path, the **basis**: decoder, decoded-RAW
   contract, mode-key, check-suite, state-rule and pipeline versions, and the counts by state),
   `limitations` (for every code an entry uses, a user-facing summary and meaning, so a code that a
   later database adds can be shown by an older Lucent) and `entries`, sorted by `match`: `match`
   (a mode-key pattern), `qualified_key` (the decoded key the evidence is attached to), `state`,
   `camera`, `mode`, `family`, `limitations` (code and the qualification's detail), `evidence`,
   `evidence_gaps` and `fixtures`. A pattern is a version-1 mode key whose components may be `*`;
   Verified entries must be exact (match and qualified key), Limited entries name a limitation and
   Verified or Compatible, unverified ones none, and the counts must equal the entries'. **Lookup**:
   every matching entry applies; the answer's state is the worst of theirs, its limitations their
   union, its labels those of the most specific match, so a broad pattern can only make an exact
   entry's answer more conservative.

5. **Generation** (`apps/lucent-qualify/support_export.hpp`): `lucent-qualify --from-report
   docs/qualification/qualification-report.json --export-support assets/camera-support
   --asset-version 1.0.0` writes the database and its manifest as a pure function of the report:
   one entry per support-table row, `match` = the row's **lookup key**, `qualified_key` = its mode
   key; the limitation words come from `limitation_text()` (an unknown code fails the export).
   Refused: a partial run (selection tier other than "all"), a run with expectation mismatches or
   harness errors, a mode the harness could not qualify, a mode with other than one probe key, two
   modes sharing a probe key. `--check` compares instead of writing (exit 3 on a difference).
   `CommittedData.TheCameraSupportAssetIsTheExportOfTheReport` asserts that the committed asset is
   byte for byte the export of the committed report.

6. **Probe keys** (amends ADR 0016 decisions 1 and 6). The mode key moves unchanged to
   `lucent/assets/mode_key.hpp` (`apps/lucent-qualify/mode_key.hpp` forwards to it), so the
   harness and the application derive keys with one function. Each fixture result records
   `lookup_key`, the key of the file's probe, and each support-table row `lookup_keys`; the mode
   check records a differing probe key in its details. The committed report was regenerated with
   the same 34 outcomes (2026-10-08T21:47Z); only the 5D Mark IV S-RAW's probe key differs.
   `QualificationFixture.ProbeKeyFindsTheQualifiedState` probes every cached fixture as the
   importer does and asserts that the bundled database finds, under the probe's key, the entry of
   the fixture's qualified key with its expected state.

7. **Support rules, version 1** (`lucent/application/camera_support.hpp`, `assess_support()`):
   (a) the live interpretation first: a missing mandatory step or a failed probe is always
   Unsupported; optional operations the file asks for and Lucent lacks make it at least Limited,
   with those reasons; (b) the database's answer for the probe's key: not listed (an unknown camera
   or mode) is Compatible, unverified; Verified holds only while the database's basis equals this
   build's (`running_basis()`: decoder, contract, mode key and pipeline versions, not how evidence
   was weighed), otherwise Compatible, unverified; Compatible, unverified, Limited and Unsupported
   entries give that state, Limited and Unsupported ones a `CameraLimitation` reason per
   limitation (worded by the database) and Unsupported ones a `CameraUnsupported` reason; without
   an active database every mode is not listed; (c) the result is the worse of (a) and (b).
   `app::SupportStatus` is now `assets::SupportState` (Verified 1, Compatible, unverified 2,
   Limited 3, Unsupported 4: severity order, `assets::worse()`). Every assessment of a probe
   records how it was made (`SupportAssessment::camera`: rules version, the probe's mode key and
   interpretation status, the database id, version and qualification date, the basis check, the
   entry's state, qualified key, labels and evidence).

8. **Persistence and the reader-version contract** (`catalog_mapping.hpp`). No catalog schema
   change: the status column keeps the three spellings every reader knows (Verified and
   Compatible, unverified -> "supported"; Limited -> "limited"; Unsupported -> "unsupported"), and
   the reasons JSON gains `camera_limitation` and `camera_unsupported` elements and one trailing
   `camera_support` element holding the record. A reader restores Verified only from that element
   and only when its state fits the column; an element that does not fit is dropped (the item is
   then probed again). An older build reading the catalog sees a Verified item as supported, which
   it shows as Compatible, unverified: conservative, never better than the truth; it shows Limited
   and Unsupported items with its generic wording. `min_reader_version` is unchanged.

9. **Re-assessment.** At start-up, after loading the library, the shell's Backend activates the
   database (`load_camera_support()`) and calls `refresh_support()`: every item whose record names
   another database (id and version), none, other rules or another basis result is re-assessed from
   its record without reading its file and stored in one transaction (`Library::set_support()`,
   one Updated change; a read-only catalog keeps what it has). Items without a record (catalogued
   before this phase) are outdated (`probe_outdated()`), and source maintenance probes them again;
   the decoded-RAW contract 1.4 of ADR 0017 already makes every older item outdated once. Imports,
   maintenance re-reads and folder batches assess new probes against the session's database
   (`ImportRequest`, `RefreshOptions`, `BatchServiceOptions::camera_support`).

10. **Batches and reports.** An Unsupported item is excluded when a batch is created, with its
    reasons (unless the request allows unsupported items), as before; its report row is Skipped with
    the support state Unsupported. A Limited item is exported with a review note per database
    limitation, "Limited support: <summary>: <meaning>", beside the existing note for optional
    operations a render skipped (which now quotes the interpretation's reason). Receipts record the
    item's state and database version (optional `support` member of `lucent.batch-receipt` v1,
    ignored by older readers); the report's JSON and HTML show "Camera support: <state>" per photo,
    and the Exports view shows it as a badge with the state's meaning.

11. **The shell.** Library cards keep the state badge; its tooltip says why (for Verified: which
    camera and mode, database version and qualification date; for Compatible, unverified: not
    listed, listed without quality evidence, qualified with another decoder or pipeline, or no
    database) and what the state means. The Edit view's details show the badge, the reasons
    (limitations in the shell's own translatable words, `describe_limitation()`, with the
    database's English for codes it does not know) and "What this means". The header's **About**
    opens Help > About: Lucent's version, the camera support database (version, qualification date,
    decoder, counts, location, or why none is in use, and data packs that were not used) and the
    meaning of the four states. Developer automation reports the database at start-up on stderr and
    `--show-about` opens the dialog before a screenshot.

12. **Packaging** (amends ADR 0009). `lucent_install_application()` installs each bundled asset's
    manifest and exactly the files it lists into `<prefix>\assets\<id>` and records them (id, pin,
    source folder, files) in `lucent-package.json` (schema 4). The audit classifies those files as
    origin `asset` (identical to the source tree's) and verifies each asset as Lucent.exe does
    (`Test-BundledAssets`: the manifest's SHA-256 is the pin, every listed file has its size and
    SHA-256, nothing unlisted, no other folder under `assets\`); the smoke run requires Lucent to
    report the folder's camera support database as active. The SBOM lists the asset files as
    Lucent's own (MIT).

## Consequences

- The bundled database 1.0.0 (generated from the report of 2026-10-08T21:47Z, LibRaw
  0.22.2-Release, decoded-RAW 1.4): 34 modes, 14 Verified, 11 Compatible, unverified, 6 Limited
  (X-Trans baseline demosaicing twice, Dual Pixel data, a DNG's Enhanced image, and the two modes
  whose image area LibRaw does not read), 3 Unsupported. A photo of a listed mode now shows the
  qualification's state; any other photo that decodes cleanly shows Compatible, unverified.
- Verified is bound to the exact build basis. A LibRaw upgrade, a contract change or a pipeline
  version change makes every Verified mode Compatible, unverified until the qualification is run
  again and the database re-exported; `BundledCameraSupport.HoldsForThisBuildsDecoderAndPipelines`
  fails until then, as the qualification's own GTest wrapper does. A decoder upgrade that moves a
  key (a new visible area) makes the mode "not listed" instead: conservative.
- Maintenance: after a qualification run, re-export the database (a new asset version whenever its
  content changes), rebuild (the pin changes) and commit the report, the table and the asset
  together (`docs/qualification/README.md`).
- Data packs are designed for but not accepted: the per-user location, the lookup order, the
  manifest's version and minimum app version and the refusal path exist; accepting a pack needs
  signature verification against the publisher identity (W12, with Authenticode signing) and an
  ADR. Until then a pack can never change what Lucent reports.
- The pin makes the bundled asset as trustworthy as the executable: a user (or malware) that
  replaces both the data and its manifest gets "the installed camera support database failed its
  integrity check" in About, all items Compatible, unverified at best, and a log line; never a
  silently different database.
- The catalog cannot filter by Verified (its column says "supported" for both clean states); the
  library has no such filter today. A catalog column for the evaluation (mode key, database
  version) would allow it and is the catalog owners' decision.
- An older Lucent opening a catalog this build wrote shows Verified items as Compatible, unverified
  and database limitations with its generic wording; it does not rewrite their records unless it
  re-probes them (it would, after a decoded-RAW contract change of its own), in which case this
  build probes them again.
- Revisit when data packs get signatures, when the catalog gains a column for the evaluation, when
  a decoder other than LibRaw is added (keys and basis change), or when a mode's probe key cannot
  be made unique.

## Alternatives considered

- **A new catalog spelling "verified" with a schema migration.** It would need a minimum reader
  version bump (older builds would refuse the whole catalog instead of showing it), a change of the
  catalog module, and it would still not carry the record re-assessment needs. Rejected: the
  existing columns hold the four states and the record with an older-reader behaviour that is
  conservative.
- **Look the mode up after decoding (the first preview or export).** The state would change when a
  photo is opened, and a batch decides before decoding. Rejected: probe keys are recorded instead.
- **Match the bits component loosely when it comes from a provisional white level.** It would
  conflate a camera's 12-bit and 14-bit modes of the same size, and Verified must be exact.
  Rejected.
- **Trust the per-file hashes of the manifest alone.** Hashes in a writable folder detect
  corruption, not replacement (section 8: "hashes alone do not authenticate"). Rejected in favour
  of the build's pin of the manifest.
- **Accept unsigned data packs from the per-user folder** (newest version wins). Rejected by the
  blueprint; the lookup order exists so that signed packs can be added without changing callers.
- **Fall back to an older copy, or to no verification, when the bundled asset fails.** Section 10:
  old assets are not silently replaced. Rejected: no database is active and the reason is shown.
- **Compile the database into the executable.** It would need a rebuild for every data change and
  would not exercise the manifest and activation path data packs will use. Rejected; the pin gives
  the same authenticity.
- **Generate the database at build time from the report.** The shipped data would not be a
  reviewable, committed file. Rejected: the committed asset is checked to be the export instead.
- **Make the application's rules part of the database (states computed offline only).** The live
  interpretation must still win (a file of a Verified model whose DNG asks for an unsupported
  mandatory opcode is Unsupported), and unknown cameras must be assessed at all. Rejected: the
  database answers for modes, the rules combine it with the probe.

## Amendment: closing review (2026-10-08)

The closing review of W10 (findings QUA-01 to QUA-10, AST-01 to AST-10) found that a Verified state
could outlive what it certified, that a missing database could improve a photo's state, and that
engine limitations depended on whether the exact mode was qualified. Changes:

1. **Database schema 2** (`kCameraSupportSchemaVersion` 2; version 1, never released, is refused
   with VersionMismatch). The qualification basis also records the decoder's capabilities (its
   codecs), every `engine::kDevelopEngineVersions` entry (`develop_engine`: sensor normalisation,
   white balance, the demosaic routes, camera colour, working space, guided filter, proxy
   downscale, besides the stage graph, DNG opcodes and image-area placement), the neutral look
   (`neutral_look`) and the export path (`export`, the new `exporter::kExportVersion`):
   `basis_differences()` compares them all, so changing any of them makes every Verified entry
   Compatible, unverified until the qualification runs again (QUA-02;
   `CameraSupport.TheBasisCoversEveryEngineDecoderAndExportVersion`). Each entry records the probe
   facts of its qualified file (`assets::derive_mode_variant()`: `image_area`, the image area's
   source and size, and `dng_opcodes`), which the report records per fixture (`lookup_variant`) and
   per row (`lookup_variants`, exactly one for an export) (QUA-08). The bundled database is 2.0.0.
2. **Support rules 2** (`kSupportRulesVersion` 2; records of rules 1 are probed again).
   - A Verified entry holds only for a file whose probe facts equal the entry's
     (`CameraSupportEvaluation::variant_matches`): a Canon aspect-ratio setting, a file without its
     crop tags or a DNG with opcode lists the qualified file did not have is Compatible, unverified
     ("verified with test photos whose image area or built-in corrections differ") (QUA-08).
   - Engine limitations come from the probe, with or without a database (AST-03): X-Trans mosaic data
     ("xtrans_baseline_demosaic") and a file with a Dual Pixel component image
     ("dual_pixel_not_processed", from the importer's subimage list, `SupportFileFacts`) are at least
     Limited, as a new `EngineLimitation` reason (catalog element "engine_limitation") worded by
     the database or by `assets::standard_limitation_text()` (the exporter's words, moved from
     lucent-qualify); a database limitation of the same code is not repeated. Batch review notes name
     both kinds.
   - A database that is missing or refused never improves a state (AST-01): re-assessing against no
     database keeps the last database's answer (`listed`, its CameraLimitation and CameraUnsupported
     reasons) and turns only Verified into Compatible, unverified; when a database is active again
     the items follow it.
   - The record stores the database manifest's SHA-256 (`database.sha256`), and `assessed_with()`
     compares it: other content under the same version is re-assessed (AST-02). The exporter refuses
     to write other content under the version the asset folder holds.
   - `probe_outdated()` is also true for a probe of another LibRaw version and for a record whose
     mode key is not of this build's mode-key version; `refresh_support()` leaves such items to
     source maintenance instead of re-assessing their stale keys, and only caps a stored Verified to
     Compatible, unverified until they are probed again (AST-08). `refreshed_support()` and
     `current_support()` give the state with the active database without storing it; the shell
     shows it for every library item, so a read-only catalog never shows a Verified state the
     active database does not confirm (AST-06).
3. **Batches.** A job whose photo is Unsupported when its attempt starts (a database update or a
   re-probe after the batch was created; resume and retry included) ends Skipped with its reasons,
   as at creation (AST-07).
4. **Activation and the shell.** `bundled_asset_request()` refuses an id the build has no pin for
   (`assets.load.unpinned`), so an asset is never trusted on its own hashes (AST-09). Lucent.exe
   writes its log to `<cache>\logs\lucent.log` (`core::FileSink`, bounded rotation), so the reason a
   database was refused is recorded (AST-04); About shows the refusing step ("Technical detail:
   assets.load.hash: ..."), a database that cannot be read (access denied, I/O errors) is no longer
   reported as failing its integrity check, and the header shows "No camera support database"
   whenever none is active. The meanings shown for the states were corrected: Compatible,
   unverified "lacks representative quality evidence" (it has no test photo, or its tests could not
   be confirmed; QUA-10, AST-05), Unsupported covers modes the tests found not to develop
   correctly, a 360 source says to stitch it first, and batch report rows name the state for screen
   readers (AST-10).
5. **Provenance.** The build's commit carries "-dirty" when tracked files differ at configure time
   (`cmake/LucentVersion.cmake`), and `--export-support` refuses a report of such a build unless
   `--allow-dirty-build` (QUA-06). The database 2.0.0 exported during the review recorded the
   uncommitted build "72cb6d170d50-dirty"; it was superseded by database 2.0.1, exported without
   `--allow-dirty-build` from a fresh build of commit 0161428 (all 34 fixtures matched their
   expectations, same states). Only the provenance changed, hence the patch version.

Consequences: the run of the review fixes over all 34 fixtures gave the same outcomes as before
(Verified 14, Compatible, unverified 11, Limited 6, Unsupported 3; no fixture's state changed); the
bundled database is 2.0.0. An independent colour reference (QUA-03) is not part of the basis: the
colour check still compares with LibRaw's calibration, which the support table, the README and the
Verified meaning now say (ADR 0016 amendment).
