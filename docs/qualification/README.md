# Camera qualification

Lucent proposes a compatibility state for every exact camera model and capture mode it has
evidence for (blueprint section 3, "User-visible compatibility states"; section 10, "Camera
correctness" gate). The decisions behind this process are recorded in
[ADR 0016](../adr/0016-camera-qualification.md).

| File | What it is |
|---|---|
| [`support-table.md`](support-table.md) | **Generated.** The support table (model x mode -> proposed state, evidence, limitations), the mode keys, the reasons and evidence gaps per fixture, the coverage-gap list against the blueprint's qualification families and the measured time and memory. |
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
`LUCENT_TEST_FIXTURE_TIER=all`.

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
7. Re-run the whole tier and regenerate `support-table.md`; commit the expectations, the table and
   (when it changed) the families file together.

When Lucent's behaviour changes for a camera (decoder upgrade, new route, default crop
implemented), the GTest wrapper fails until the expectation is updated: review the change, update
the expectation and regenerate the support table in the same commit.
