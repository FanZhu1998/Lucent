# Camera qualification

Lucent proposes a compatibility state for every exact camera model and capture mode it has
evidence for (blueprint section 3, "User-visible compatibility states"; section 10, "Camera
correctness" gate). The decisions behind this process are recorded in
[ADR 0016](../adr/0016-camera-qualification.md); the camera support database the application
reads the states from in [ADR 0018](../adr/0018-camera-support-data.md).

| File | What it is |
|---|---|
| [`support-table.md`](support-table.md) | **Generated.** The support table (model x mode -> proposed state, evidence, limitations), the mode keys, the reasons and evidence gaps per fixture, the coverage-gap list against the blueprint's qualification families and the measured time and memory. |
| [`qualification-report.json`](qualification-report.json) | **Generated.** The full report of the last complete run (`lucent.qualification-report` v1): every check of every fixture, its probe key (`lookup_key`), the support table and the coverage. |
| `../../assets/camera-support/` | **Generated.** The camera support database Lucent ships (`camera-support.json`, `lucent.camera-support` v1) and its manifest (`manifest.json`, `lucent.asset-manifest` v1), exported from the report. |
| `../../fixtures/qualification/expectations.json` | Reviewed, cited facts per fixture and the outcome the suite must produce (schema `lucent.qualification-expectations` v1). |
| `../../fixtures/qualification/families.json` | The blueprint's qualification families and their targets, matched against mode keys (schema `lucent.qualification-families` v1). |
| `../../apps/lucent-qualify/` | The check suite, mode keys, state rules, report writers and the `lucent-qualify` CLI. |
| `../../tests/qualification/` | Unit tests and the GTest wrapper that asserts each fixture's expected outcome (label `qualification`). |
| `../../tests/qualification/tools/file_facts.py` | Reads the facts a file declares about itself (make, model, orientation, CFA, default crop, CR3 CMP1, RAF crop tags) with the Python standard library only, independently of LibRaw and Lucent. |

## Running the suite

The fixtures must be downloaded first (`fixtures/README.md`):

```powershell
powershell -NoProfile -File tools\fetch-fixtures.ps1 -Tier all
powershell -NoProfile -File tools\build.ps1 -Preset dev-core
build\dev-core\bin\lucent-qualify.exe --tier all --out-json build\qualification-report.json `
    --out-md docs\qualification\support-table.md --previews build\qualification-previews
```

Each fixture runs in its own process (up to about 30 s and 1.4 GB each, about 6 minutes for the
whole corpus), one after another. Exit status: 0 when every fixture produced exactly its expected outcome, 3 when a state,
check status, limitation or mode key differs from `expectations.json`, 2 when a fixture could not
be qualified (missing, altered, or its process failed), 1 for usage errors. `--id <fixture>` runs
single fixtures; `--in-process` runs them in the tool's own process.

In CI the same outcomes are asserted by `lucent_qualification_fixture_tests`
(`ctest -L qualification`): smoke fixtures always, the qualification tier with
`LUCENT_TEST_FIXTURE_TIER=all`. The same executable probes every cached fixture of any tier as the
importer does and checks that the bundled camera support database finds the fixture's mode under
the probe's key with the expected state (`ProbeKeyFindsTheQualifiedState`).

## The camera support database

The application does not read the report: it reads the camera support database, a versioned data
asset exported from it and installed next to `Lucent.exe` (`assets\camera-support`). Lucent
verifies it before use (the manifest against the hash built into the executable, every file
against the manifest, the minimum Lucent version) and combines its answer for a photo's mode key
with the photo's own interpretation (ADR 0018): a missing mandatory step is always Unsupported, a
camera mode the database does not list is Compatible, unverified, and a Verified mode stays
Verified only while the database was qualified with the running build's decoder, decoded-RAW
contract and pipeline versions.

After a complete run whose outcomes all match their expectations:

```powershell
build\dev-core\bin\lucent-qualify.exe --tier all --out-json docs\qualification\qualification-report.json `
    --out-md docs\qualification\support-table.md
build\dev-core\bin\lucent-qualify.exe --from-report docs\qualification\qualification-report.json `
    --export-support assets\camera-support --asset-version <version>
```

- `<version>` is the database's SemVer: keep it when nothing in `camera-support.json` changed
  (`--check` compares without writing and exits 3 on a difference), otherwise raise it (minor for
  added or changed modes, patch for wording only). Lucent treats every version as a new database
  and re-assesses the library's photos against it at the next start, without reading their files.
- `--min-app-version` (default: the building Lucent's version) is the oldest Lucent allowed to use
  the database.
- The export refuses a partial run, a run with expectation mismatches or harness errors, a
  limitation code without user-facing words (`limitation_text()` in
  `apps/lucent-qualify/support_export.cpp`) and two modes that share a probe key.
- Rebuild afterwards: the build pins the manifest's hash, and
  `BundledCameraSupport.*` (`lucent_assets_tests`) and
  `CommittedData.TheCameraSupportAssetIsTheExportOfTheReport` (`lucent_qualification_tests`) fail
  until the committed asset is exactly the export of the committed report and holds for this
  build's versions. Commit the report, the support table and the asset together.

The asset folder's `.gitattributes` keeps git from changing line endings: the manifest hashes the
exact bytes.

## What "Verified" means here

A mode is proposed **Verified** only when, on its exact mode key, every check that applies passed
with strong evidence (decode parity including the raw-buffer hash, like-for-like colour, the
camera's image area, CFA confirmed by the file's own declaration and by green-site statistics,
black/white sanity, required corrections, numerical validity, export round trip, determinism) and
every expected fact (identity, compression mode, camera image area, orientation, representation)
is independently verified. Anything less is *Compatible, unverified* (evidence gap), *Limited* (a
known optional capability is missing) or *Unsupported* (decoding or a mandatory step fails). The
corpus has one daylight test file per mode, so even a Verified mode has not been tested across
scenes, lenses or firmware versions; the table says so.

## The camera's image area

Lucent renders the image area the file declares (DNG DefaultCrop, or the decoder's reading of the
camera's own crop tags; [ADR 0017](../adr/0017-camera-image-area.md)) by default, and the decoder's
whole visible area when the file declares none. The dimensions check measures that output; a
mode whose output still exceeds the camera's recorded image size is Limited
(`default_crop_not_applied`). Each fixture's expectation may pin where the area comes from
(`dimensions.image_area_source`), and the support table lists the declared and applied area per
fixture. The colour check renders the decoder's visible area, like the rawpy reference
(`RenderSettings::apply_camera_area = false`, `lucent-render --no-camera-crop`).

## Adding a camera or mode

1. Add a CC0 fixture to `fixtures/manifest.json` and its rawpy reference (`fixtures/README.md`,
   "Adding a fixture"). Regenerate the rawpy render statistics if the fixture renders
   (`research/tools/render_reference.py --write-stats`), so the colour check has a reference.
2. Run `python tests/qualification/tools/file_facts.py --out facts.json` and read the new
   fixture's declared make, model, orientation, CFA and default crop.
3. Find the manufacturer's specification for the mode's recorded pixel count (a manual or
   specification page; quote it in a `manufacturer_spec` source with its URL and the retrieval
   date) and, where available, an independent decoder's entry (RawSpeed `cameras.xml`).
4. Run `lucent-qualify --id <fixture> --previews <dir>` and look at the preview: upright, natural
   colours (an R/B swap or a wrong CFA phase is obvious), no dark or garbage borders. Record the
   inspection as a `reviewer_visual` source.
5. Add the fixture to `expectations.json`: camera, the derived mode key, dimensions (the rendered
   size and the camera's image area with its sources), orientation, representation, mandatory
   steps, and the expected state, check statuses and limitations. Mark every fact you could not
   check against an independent source as `unverified`; the loader rejects a `verified` fact whose
   only sources are LibRaw/rawpy or the archive's labels.
6. If the mode belongs to a blueprint family target, check that its mode key matches the target
   in `families.json` (or add the target).
7. Re-run the whole tier and regenerate `support-table.md` and the report, then re-export the camera
   support database (above); commit the expectations, the table, the report, the asset and (when
   it changed) the families file together.

When Lucent's behaviour changes for a camera (decoder upgrade, new route, default crop
implemented), the GTest wrapper fails until the expectation is updated: review the change, update
the expectation and regenerate the support table in the same commit.
