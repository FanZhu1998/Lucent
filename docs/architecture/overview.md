# Architecture overview

The product blueprint (`docs/blueprint/lucent-desktop-raw-processing-blueprint.md`, revision 3)
defines what Lucent does. This document records how the code is organised and which module may
depend on which. Decisions with lasting consequences are captured as ADRs in `docs/adr/`.

## Layers

Modules are listed lowest first. A module may depend only on modules above it in this table
(earlier rows); circular dependencies are not allowed. "Planned" modules arrive in later
workflows (see `docs/development/roadmap.md`).

| Module | Library | Responsibility | Third-party dependencies | Status |
|---|---|---|---|---|
| core | `lucent_core` | Error model, hashing, file access, text conversion, logging, cancellation, versioning | tl-expected, Win32 | W1 |
| image | `lucent_image` | Pixel containers, CFA descriptors, colour-encoding contracts, colour math | — | W1 |
| raw | `lucent_raw` | `RawDecoder` interface, decoded-RAW data contract, decoder registry, LibRaw adapter, DNG tag inspection; DNG opcode parameter decoding and the opcode support table (which opcodes Lucent applies at which stage, ADR 0014) | LibRaw (private) | W1 |
| engine | `lucent_engine` | Sensor normalisation, white balance, demosaicing routes, camera colour, working space, provisional display rendering, orientation (W2, ADR 0006); `DevelopSettings` v1 (W6; v2 below), white-balance model (temperature/tint), develop operations (exposure, guided-filter highlights/shadows, contrast, tone curve, saturation/vibrance, base look "neutral" v1) and the two-stage develop pipeline: cached stage one, stage two per settings change, the export render (W6, ADR 0010); DNG opcode processing at the DNG stages (OpcodeList1 on stored samples, List 2 before white balance, List 3 after demosaicing), geometry records and the carried-lens-correction rule (W9, ADR 0014); `DevelopSettings` v2 (pipeline v3): geometry (straighten, crop, quarter turns, flip) through `GeometryMapping`, brush, linear and radial masks with local adjustments, guided chroma denoising, brush-raster and stage-two caches and settings-aware memory estimates (W9 part 2, ADR 0015) | — | W2 |
| export | `lucent_export` | Output profiles, JPEG/TIFF encoding, ICC embedding, verification, no-clobber publication (W2); reduced JPEG thumbnails for the cache (W4); metadata (later) | libjpeg-turbo, libtiff, Little CMS (private) | W2 |
| catalog | `lucent_catalog` | SQLite catalog (local disk only, WAL, one writer), migrations, verified backups and restore, source identity, relinking, import records (W4, ADR 0008); recipes (W6), job journal (W7); schema v5: automatic batches and job states (pending, analysed, frozen) with the one-time rebinding of a job's identity to its effective automatic revision, and the analysis records of automatic revisions (W8, ADR 0013) | SQLite (private) | W4 |
| analysis | `lucent_analysis` | Natural v1 automatic settings (W8, ADR 0011): metric contract, source diagnostics, proposals, bounded candidate search with recheck and conservative fallback, confidence and review flags, related-shot grouping and consistency; optional learned inference later | ONNX Runtime (private, later) | W8 |
| assets | `lucent_assets` | Versioned data assets (W10, ADR 0018): the asset manifest "lucent.asset-manifest" v1 (id, version, minimum Lucent version, content schema, size and SHA-256 per file), verification before activation (the bundled manifest against the hash pinned into the build, every file against the manifest, the minimum version), locations and lookup order (per-user data packs looked at first and refused until they can be signed; the bundled folder next to the executable), the qualification mode key (ADR 0016) and the camera support database "lucent.camera-support" v1 (mode-key patterns to the blueprint's four states, limitations with stable codes and words, evidence, the qualification basis); camera/lens profiles and models later | nlohmann-json (private) | W10 |
| workers | `lucent_workers` | Isolated worker processes (W7, ADR 0012): the versioned protocol "lucent.worker" over inherited anonymous pipes, the host (Job Objects with kill-on-close and commit limits, heartbeats, cooperative cancel then termination, crash classification, restarts), the worker-side server and the job's output files; `lucent-worker.exe` (apps) runs the develop render and verified export of one job; protocol version 2 adds analyze jobs, which run the automatic settings of one photo in the worker (W8, ADR 0013) | nlohmann-json (private); Win32 | W7 |
| application | `lucent_application` | Import (discovery, exclusions, refresh of changed files), session library, task executor with memory admission, preview and camera-preview service, single-item export (W3, ADR 0007); catalog-backed library, verified copies into a library folder, source maintenance (hashing, presence checks, relinking) in the background, disk thumbnail cache (W4, ADR 0008); recipe documents ("lucent.develop", canonical JSON), revision stores and the recipe service (gesture coalescing, undo/redo with branches, reset, revert, locks, copy/paste by category across cameras), edit sessions with a cached stage one, quick-then-refine previews, histograms and clipping masks, retained memory admission, recipe-aware previews, Lucent-rendered thumbnails and exports with recorded recipe identity (W6, ADR 0010); durable batch export: destinations and Windows-safe names, the batch service (catalog job journal, memory-budgeted scheduler over `lucent-worker` processes, handle-held verification and no-clobber publication with receipts, restart reconciliation), completion reports, single exports as batches of one (W7, ADR 0012); automatic settings (W8, ADR 0013): the shared run (analysis, related-shot regularisation, full-resolution final check and conservative fallbacks, locks report), automatic revisions with their analysis records, the editor's interactive Auto, and automatic batches (Auto, Analyze only, the durable related-shot pre-pass); recipe "lucent.develop" version 2 (version 1 read and upgraded), the paste categories detail, masks and geometry, per-session brush-raster and stage-two caches, frames with their geometry and mask outlines, Auto deciding without the geometry and masks (W9 part 2, ADR 0015); the blueprint's four support states (W10, ADR 0018): the camera support database's answer for the probe's mode key combined with the interpretation (a missing mandatory step is always Unsupported, an unknown camera that decodes cleanly Compatible, unverified), recorded with each assessment and kept in the catalog's existing support columns, re-assessed without reading files when the database changes, Limited exports with a review note naming the limitation, receipts and reports with the state | nlohmann-json (private); no Qt | W3 |
| ui | `Lucent.exe`, `lucent_ui_adapters` | Qt Quick Library / Edit / Exports views, view models, UI-side service interfaces, and the adapters that bind them to `lucent_application` (ADR 0007); Import dialog, Missing/Duplicate/Removable badges, relink, catalog backup, re-checks when a volume arrives or leaves (W4, ADR 0008); the crop and rotate tool, mask tools with outlines, handles and weight overlays, and colour noise reduction (W9 part 2, ADR 0007 decision 15); support badges with why and what the state means in Library cards, Edit details and batch reports, and Help > About with the camera support database (W10, ADR 0018) | Qt Quick | W3 |

Executables live in `apps/`: `Lucent.exe` (GUI, WIN32 subsystem), `lucent-worker.exe` (isolated
processing worker) and developer tools such as `lucent-probe.exe`, `lucent-render.exe` and
`lucent-qualify.exe` (camera qualification and the export of the camera support database). Data
assets live in `assets/<id>/` (manifest and files; the camera support database first) and are
installed next to `Lucent.exe` in `assets\`.

## Dependency rules

- `core` and `image` are pure C++ with no knowledge of files formats, Qt or databases.
- Third-party libraries are private implementation details of exactly one module. LibRaw types
  never leave `lucent_raw`; SQLite never leaves `lucent_catalog`; Qt never appears below
  `workers`.
- `engine` depends on the decoded-RAW *data contract* from `raw`, not on any decoder adapter.
- `application` is pure C++: no Qt, so workers and command-line tools can use the same services.
  Its callbacks run on its own executor threads; the UI adapters (`apps/lucent/src/adapters`)
  post them to the UI thread.
- The UI never decodes or renders on its thread; it talks to application services, which
  dispatch work to background threads: an in-process executor for previews and editing (ADR 0007),
  and from W7 isolated `lucent-worker` processes for every export and batch (ADR 0012), driven by
  `lucent_application` through `lucent_workers`; pixels never cross the process boundary.

## Build and dependency management

- CMake presets (`CMakePresets.json`): `dev-core` (no Qt), `dev` (full app), `release`.
- vcpkg manifest mode (`vcpkg.json`) pinned by `vcpkg-configuration.json` to a single registry
  baseline. The overlay triplet `x64-windows-lucent` builds shared, release-only libraries with
  the dynamic CRT. See `docs/adr/0002-build-and-dependency-management.md`.
- `tools/dev-env.ps1` enters the MSVC environment; `tools/build.ps1` configures, builds and tests.

## Pixel and colour conventions (summary)

- Working data is float32; scene-linear values may be negative or exceed 1.0.
- Every image edge declares channel layout, colour primaries, white point, transfer function,
  numeric range, orientation and scene/display reference (`lucent::image::ColorEncoding`).
- The initial common working space is scene-linear Rec.2020 primaries with a D65 white point
  (blueprint section 5).
