# Lucent

Offline, camera-independent RAW photo development for Windows.

> **Status: pre-alpha, under active development.** Lucent has completed its thirteenth development
> increment (W13: lens corrections from the bundled Lensfun database; pending commit and merge), the
> first part of its release hardening (W12 part 1: a camera-correctness gate with an independent
> colour reference, a hard memory limit across the export workers, a performance baseline, an
> offline-first Check for updates that no build is configured to use yet, and catalog fixtures of
> every schema version; pending commit and merge), its tenth development increment (W10, multi-brand
> camera qualification and the four support states), the ninth (W9: DNG opcode lists, crop and
> straighten, masks and colour noise reduction; its Lensfun lens corrections came with W13) and the eighth
> (W8, the Natural v1 automatic settings with the editor's Auto and automatic batches): a Qt Quick
> `Lucent.exe` that adds RAW files in place or as verified copies into a library folder, keeps them in a saved
> library (a local SQLite catalog) with picks, content hashes, duplicate, missing-file and
> removable-media badges and relinking, and browses them with labelled camera previews and support
> states. Opening a photo edits it: white balance (temperature and tint where the camera's
> calibration defines them), exposure, contrast, highlights and shadows, a tone curve, saturation
> and vibrance, crop, straighten, quarter turns and mirror, brush, linear-gradient and radial masks
> with their own exposure, contrast, highlights, shadows, saturation, temperature and tint, colour
> noise reduction, and lens corrections (distortion, chromatic aberration and vignetting from the
> bundled Lensfun database: automatic only when the camera and the lens match a profile exactly,
> never repeating a correction the file already makes, or from a profile you choose), with a histogram, clipping overlay, before/after, undo/redo over an
> immutable revision history and copy/paste by category. Thumbnails, previews and JPEG or 16-bit TIFF exports
> render each photo's recipe. Exports run as durable batches: a selection, every photo or a folder
> is exported to JPEG or 16-bit TIFF in isolated `lucent-worker.exe` processes under a memory
> budget, each output verified before it is published without overwriting anything, with pause,
> resume, cancel, actionable pauses for a full or disconnected destination, recovery after Lucent
> is closed or stops, and a completion report. **Auto** in the editor, and batches set to Auto or
> Analyse only, choose each photo's settings with the deterministic Natural v1 optimiser (locks are
> respected and reported), check the result at full size with a bounded conservative fallback,
> and save it as an ordinary, undoable revision with its analysis record (it adjusts the global
> settings and keeps the photographer's crop, masks and noise reduction); related shots can be
> kept consistent, and uncertain results are marked for review. Natural v1 is deliberately
> conservative and **not yet calibrated** against photographers' preferences. DNG opcode lists are
> applied at the stages the specification defines, and each camera's own image area is rendered by
> default. Every photo shows a camera support state from Lucent's qualification of 40 test files
> ([support table](docs/qualification/support-table.md)): 16 are Verified (exact model and capture
> mode; colour checked against a colour chart's published values where the file shows one, otherwise
> as agreement with the decoder's camera calibration), 14 Compatible, unverified, 7 Limited and 3
> Unsupported; any other camera that decodes cleanly is Compatible, unverified. It runs from a source build or from an
> unsigned per-user installer built from source (see [Installing](#installing)). The next section
> describes the product Lucent is being built to become, from its blueprint, not what it can do
> today.

## What Lucent is meant to be

Lucent is planned as a desktop application for photographers. You choose photographs or an input
folder and an output folder, and Lucent produces a separately optimised, editable result for
every supported image. Opening a single photograph gives detailed controls driven by the same
processing engine. Original files are never modified. The installed application runs offline and
needs no terminal, Python installation or account.

The design, from
[the blueprint](docs/blueprint/lucent-desktop-raw-processing-blueprint.md):

- **Camera support as a capability contract.** RAW decoding goes through a replaceable adapter
  (LibRaw first). Each camera model and capture mode must earn *Verified* status through tests.
  Everything else is reported as *Compatible, unverified*, *Limited* or *Unsupported*, never
  guessed from a brand or file extension.
- **One explicit, colour-managed pipeline.** Float32 scene-linear working data in Rec.2020
  primaries with a D65 white point. Every image carries its colour encoding as data. Export writes
  JPEG and 16-bit TIFF with embedded ICC profiles (Little CMS).
- **Automatic settings for every photograph.** A bounded, deterministic per-image optimiser
  ("Natural v1") proposes ordinary editable settings: exposure, white balance, tone and colour.
  Learned methods remain optional until they beat the deterministic baseline on held-out
  photographs.
- **Durable batches.** Isolated worker processes, memory-aware scheduling, resumable jobs, verified
  no-clobber publication of outputs, and a completion report.
- **Forward-compatible data.** Immutable, versioned recipes. Independently versioned catalog
  schema, worker protocol and asset manifests.
- **Native delivery.** A C++20 engine, a Qt 6 Quick interface, a local SQLite catalog, and a signed
  per-user installer that launches `Lucent.exe` with a double-click.

## Current status

Development proceeds in increments listed in the [roadmap](docs/development/roadmap.md):

| Increment | Scope | Status |
|---|---|---|
| W0 | Toolchain and repository scaffolding | Done |
| W1 | `core`, `image` and `raw` modules, LibRaw probe adapter, `lucent-probe` developer tool, camera fixture manifest and downloader, rawpy reference-parity script, ADRs | Done |
| W2 | `engine` render path (sensor normalisation, white balance, demosaicing, camera colour, Rec.2020 working space, provisional display rendering, orientation), `export` (verified JPEG / 16-bit TIFF with ICC profiles), `lucent-render` developer tool, rawpy render gates | Done |
| W3 | Qt Quick `Lucent.exe` (Library / Edit / Exports), `application` services (import, session library, background previews and camera previews, export), UI adapters (ADR 0007) | Done |
| W4 | `catalog` module (SQLite, migrations, backups), persistent library, import in place or as verified copies, source identity (SHA-256 and file identity), duplicates, missing files and relinking, disk thumbnail cache (ADR 0008) | Done |
| W5 | Per-user Inno Setup installer with Qt deployment, dependency audit, licences and SBOM (ADR 0009) | Done except the clean-machine test |
| W6 | Develop settings v1 and editing operations (white balance, exposure, guided-filter highlights and shadows, contrast, tone curve, saturation and vibrance, base look "neutral" v1), two-stage interactive rendering, versioned recipes with immutable revisions (catalog schema v2), undo/redo, copy/paste, the Edit view (ADR 0010) | Done, pending commit and merge |
| W7 | Durable batch export: `lucent-worker.exe` and its versioned pipe protocol, memory-budgeted scheduling, catalog job journal (schema v3 and v4) with leases, a publication journal and receipts, verified no-clobber publication, restart recovery, pause / cancel / resume and destination attentions, EXIF capture metadata with an explicit GPS policy, completion reports, the Export batch dialog and the Exports view (ADR 0012) | Done, pending commit and merge |
| W8 | Natural v1 automatic settings (`lucent_analysis`: metric contract, proposals, bounded deterministic search with rechecks, confidence and review flags, related-shot consistency; ADR 0011) and their integration: the editor's Auto with a full-resolution final check and conservative fallbacks, automatic revisions with analysis records (catalog schema v5), automatic batches (Auto, Analyse only, related shots kept consistent) analysed in the worker (protocol v2), the Automatic settings panel and Review badges (ADR 0013) | Done; the blind preference test and the calibration of Natural v1 are pending |
| W9 | DNG opcode lists 1-3 at their specified stages (ADR 0014); crop, straighten, quarter turns and mirror, brush, linear-gradient and radial masks, guided chroma denoise, recipes version 2 (ADR 0015); lens corrections | Done (the lens corrections with W13); masks, geometry and denoise pending commit and merge |
| W10 | Multi-brand qualification: the `lucent-qualify` harness, cited expectations and the support table (ADR 0016); the camera's image area (ADR 0017); the camera support database, a versioned and verified data asset generated from the qualification, and the blueprint's four support states in the library, the editor, batch exports and reports, with Help > About (`lucent_assets`, ADR 0018) | Done (phases 1 to 3 and the closing review), pending commit and merge; an independent colour reference, a rotated test file per image-area source and regenerating the qualification report and database from a clean committed build are deferred to W12 |
| W11 | Learned appearance research: Image-Adaptive-3DLUT reproduced, native LUT inference accepted (ADR 0019) | Research done; product integration waits for a model with clear redistribution rights |
| W12 | Release hardening. Part 1: the camera-correctness gate (ADR 0020), the workers' hard aggregate memory limit and Auto's final check by bands (ADR 0012, 0013 amendments), the performance harness and baseline (ADR 0021), build-machine paths kept out of the package, verified signing hooks, the offline-first Check for updates over a signed release manifest and catalog fixtures of every schema version (ADR 0009 amendment, ADR 0022), and their closing review | Part 1 done, pending commit and merge; the licence and release phase (licence inventory, LGPL source provision, release signing, the first update channel, the clean-machine installer test) follows W13 |
| W13 | Lensfun lens corrections: distortion, lateral chromatic aberration and vignetting from the bundled, verified Lensfun database (an asset with its CC BY-SA 3.0 licence), applied in stage two before camera colour for exact camera and lens matches only and never doubled with a correction the file's DNG opcodes make; recipes version 3; the Lens corrections panel with a profile choice; the qualification with lens corrections; Lensfun and its GLib runtime as replaceable LGPL DLLs, two of them patched to keep build-machine folders out (ADR 0023) | Done with its closing review, pending commit and merge; the qualification report and camera support database are regenerated with lens corrections from a clean build |

`Lucent.exe` is built with the `dev` preset and run from the build tree
([setup](docs/development/setup.md#running-lucentexe-from-a-build-tree)). The library is saved in
`%LOCALAPPDATA%\Lucent\data\lucent.catalog` (`--data-root` points a developer session elsewhere);
edits are stored there as recipe revisions, so a library opened by Lucent W6 can no longer be
opened by an earlier build, W7 adds the batch journal (catalog schema v4) and W8 the automatic
settings' job states and analysis records (catalog schema v5); earlier builds open such a library
read-only. W9 writes edits as "lucent.develop" version 2 recipes (crop, masks, noise reduction),
which earlier builds show read-only; version-1 edits open and are upgraded. W13 writes version 3
(lens corrections), which earlier builds show read-only; edits made before W13 keep rendering
without lens corrections until you turn them on. Photos are rendered with their recipe and the base look "neutral" v1.
W10 shows each photo's camera support state (Verified, Compatible, unverified, Limited,
Unsupported) from the camera support database installed with Lucent (`assets\camera-support`,
verified against its manifest and the hash built into `Lucent.exe`; Help > About shows its version,
and the header says when no database is in use); an earlier build opening the same library shows
Verified photos as Compatible, unverified. `Lucent.exe` writes its log to
`%LOCALAPPDATA%\Lucent\cache\logs\lucent.log`.
The developer command-line tools `lucent-probe`, `lucent-render`, `lucent-auto` and
`lucent-qualify` remain; they are not installed.

## Installing

There is no published release yet. `tools\package.ps1` builds the installer from source,
`Lucent-Setup-<version>-x64.exe`, and beside it `Lucent-<version>-corresponding-source.zip`, the
source of the LGPL libraries it contains ([setup](docs/development/setup.md#packaging-the-installer),
[ADR 0009](docs/adr/0009-windows-packaging.md), [ADR 0024](docs/adr/0024-release-and-licensing.md)).
A release publishes both, after the gates of the [release checklist](docs/release/checklist.md).

> **The first release waits for its clean-machine test.** Installers are published only with their
> corresponding-source archive and only after every gate of the release checklist has passed; the
> test on a clean, offline Windows (in Windows Sandbox, which is not enabled on the development
> machine yet) has not run, so do not hand out installers built today.

- **Per user, no administrator rights.** Lucent installs into `%LOCALAPPDATA%\Programs\Lucent`
  with a Start Menu shortcut and, optionally, a Desktop shortcut. Windows asks for permission only
  if the computer lacks a recent Microsoft Visual C++ Runtime, which Setup then installs with
  Microsoft's own installer (included, so no download is needed).
- **Optional "Open with Lucent"** for camera RAW files. It never changes which program opens a
  file when you double-click it: Lucent is added to the "Open with" menu of RAW types that already
  have a default program, and offered under "Open with" > "Choose another app" for the others.
- **x64 Windows 10 (1809) or 11 only.** Windows on Arm is not supported yet.
- **Uninstall** from Windows Settings (Apps) removes the program, its shortcuts and registry
  entries. Your library (catalog and backups), settings and caches in `%LOCALAPPDATA%\Lucent` are
  kept; delete that folder yourself if you no longer need them. Photographs are never touched.
- **Unsigned for now.** Releases are unsigned until Lucent has a code-signing certificate, so
  Windows SmartScreen may warn before the installer runs; compare the installer's SHA-256 with the
  one published beside it. Unsigned releases do not offer updates through **Check for updates**:
  install the next version over the previous one.
- **Licences.** The installation folder's `licenses` subfolder holds the third-party notices, the
  LGPL information for Qt, LibRaw, Lensfun, GLib, GNU libintl and GNU libiconv (your right to
  replace their DLLs, and the name, size and SHA-256 of the corresponding-source archive published
  with the installer), every licence text, the build recipes of the two libraries Lucent patches,
  the Lensfun lens database's CC BY-SA 3.0 licence, and a software bill of materials (SPDX).

## Repository layout

```text
apps/lucent/         Lucent.exe: the Qt Quick shell and Edit view, its view models and the adapters to the application services (W3, W4, W6)
apps/lucent-probe/   Developer CLI that prints RAW decoder probe results as JSON (W1)
apps/lucent-render/  Developer CLI that renders a RAW file and exports JPEG / 16-bit TIFF (W2)
apps/lucent-worker/  lucent-worker.exe: the isolated process that renders, verifies and exports one batch job, or analyses one photo (W7, W8)
apps/lucent-auto/    Developer CLI that runs and evaluates the automatic settings over files (W8)
apps/lucent-qualify/ Developer CLI that qualifies camera modes over the fixtures and exports the camera support database (W10)
assets/              Data assets with their manifests, installed next to Lucent.exe: the camera support database (W10) and the Lensfun lens database (W13)
cmake/               CMake helpers, the x64-windows-lucent vcpkg overlay triplet and overlay ports (gettext, gettext-libintl, glib)
docs/adr/            Architecture decision records
docs/architecture/   Module layering and dependency rules
docs/blueprint/      Product blueprint: the product authority
docs/development/    Engineering conventions, roadmap, developer setup
docs/legal/          Third-party licence ledger (development ledger)
installer/           Inno Setup script, packaging audit and licence/SBOM scripts, installer test (W5)
fixtures/            Camera fixture manifest and reference data; downloads go to fixtures/cache/ (git-ignored)
research/            Python research environment (rawpy reference decoding); development only
src/core/            lucent_core: error model, hashing, files, text, logging, cancellation, versioning
src/image/           lucent_image: pixel containers, CFA descriptors, colour-encoding contracts
src/raw/             lucent_raw: RawDecoder interface, decoded-RAW data contract, LibRaw adapter
src/engine/          lucent_engine: the render and develop pipeline from decoded RAW to display-referred Rec.2020, editing operations, Lensfun lens corrections (W13)
src/export/          lucent_export: output profiles, JPEG/TIFF encoding, verified no-clobber export, cache thumbnails
src/catalog/         lucent_catalog: SQLite catalog of sources, items, imports and recipe revisions; migrations, backups, restore
src/analysis/        lucent_analysis: Natural v1 automatic settings: metrics, proposals, bounded search, confidence, related shots (W8)
src/assets/          lucent_assets: versioned asset manifests and their verification, the mode key and the camera support database (W10), the lens database (W13)
src/workers/         lucent_workers: the worker process protocol, host and server (W7, W8)
src/application/     lucent_application: import, session and catalog libraries, source maintenance, task executor, recipes, preview, develop-preview, export, batch and automatic-settings services (no Qt)
tests/               Cross-module integration tests (unit tests live in src/<module>/tests/)
tools/               bootstrap-dev.ps1, dev-env.ps1, build.ps1, fetch-fixtures.ps1, package.ps1
```

Modules are layered: a module may depend only on lower layers. `core` is the lowest, then `image`,
then `raw`, `engine`, `export` and `catalog`; `analysis`, `assets` and `workers` sit above those
and below `application`, with the user interface on top. See
[docs/architecture/overview.md](docs/architecture/overview.md).

## Developer quick start

You need 64-bit Windows (Windows 11 is the reference platform) and PowerShell 5.1 or later. The
reference machine has 12 GB of RAM. All commands run from the repository root.

```powershell
git clone https://github.com/FanZhu1998/Lucent.git
cd Lucent

# Check prerequisites (read-only; exits 1 if something required is missing)
powershell -NoProfile -File tools\bootstrap-dev.ps1

# Install what is missing: VS 2022 Build Tools (one UAC prompt), Python 3.12, Ninja,
# pinned CMake, and vcpkg at the project baseline
powershell -NoProfile -File tools\bootstrap-dev.ps1 -Install

# Build and run the tests (the definition-of-done command)
powershell -NoProfile -File tools\build.ps1 -Preset dev-core -Test

# Optional: download the smoke set of CC0 camera fixtures for fixture-dependent tests
powershell -NoProfile -File tools\fetch-fixtures.ps1 -Tier smoke
```

- The first configure installs the vcpkg dependencies: built from source on a new machine,
  restored from the local binary cache afterwards.
- Use `-Jobs 2` to `-Jobs 4` on machines with 12–16 GB of RAM.
- The `dev-core` preset does not need Qt.
- Always build through `tools\build.ps1` or a shell that has dot-sourced `tools\dev-env.ps1`. It
  pins `VCPKG_ROOT` to the project's vcpkg checkout; the copy bundled with Visual Studio defeats
  the binary cache.
- Fixture-dependent tests skip cleanly when fixtures are absent.

[docs/development/setup.md](docs/development/setup.md) has the exact versions, manual installation
steps, presets, the research environment and troubleshooting.

## Documentation

| Document | Contents |
|---|---|
| [Blueprint](docs/blueprint/lucent-desktop-raw-processing-blueprint.md) | Product definition, pipeline, camera contract, release gates |
| [Architecture overview](docs/architecture/overview.md) | Modules, layering and dependency rules |
| [Engineering conventions](docs/development/conventions.md) | Coding, error, path, numeric, versioning and test rules |
| [Roadmap](docs/development/roadmap.md) | Increments and the definition of done |
| [Developer setup](docs/development/setup.md) | Toolchain, build, test, fixtures, troubleshooting |
| [Architecture decision records](docs/adr/README.md) | Why the code is built the way it is |
| [Third-party licence ledger](docs/legal/third-party-notices.md) | Dependencies, licences and obligations |
| [Research environment](research/README.md) | Python reference tools (development only) |

## Contributing

- Follow [the engineering conventions](docs/development/conventions.md). Warnings are errors, and
  fallible functions return `lucent::Expected` ([ADR 0004](docs/adr/0004-error-handling.md)).
- Record decisions with lasting consequences as an ADR ([ADR 0001](docs/adr/0001-record-architecture-decisions.md)).
- Adding or changing a dependency updates the [licence ledger](docs/legal/third-party-notices.md)
  in the same change ([ADR 0003](docs/adr/0003-linking-and-licensing.md)).
- One logical change per commit, with an imperative subject such as
  `raw: add LibRaw probe adapter`.

## Licence

Lucent is free and open-source software: its own source code is released under the
[MIT License](LICENSE), and Lucent is distributed free of charge, not sold. Third-party components
keep their own licences. The libraries Lucent uses under the GNU LGPL (Qt and LibRaw, and Lensfun
with GLib, GNU libintl and GNU libiconv) are separate DLLs that Lucent loads dynamically and that
you may replace with your own builds; every release publishes their complete corresponding source,
`Lucent-<version>-corresponding-source.zip`, beside its installer, with instructions to rebuild
them. The bundled Lensfun lens database is used unmodified under CC BY-SA 3.0, and the installer is
made with Inno Setup under its non-commercial terms. See
[ADR 0003](docs/adr/0003-linking-and-licensing.md), [ADR 0024](docs/adr/0024-release-and-licensing.md)
and the [licence ledger](docs/legal/third-party-notices.md).
