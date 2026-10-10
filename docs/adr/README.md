# Architecture Decision Records

This directory records Lucent's architecturally significant decisions: why the code is built the
way it is and which alternatives were rejected. The process is described in
[ADR 0001](0001-record-architecture-decisions.md). New records start from
[template.md](template.md).

The product blueprint (`../blueprint/lucent-desktop-raw-processing-blueprint.md`) remains the
product authority. The current module structure is described in
[`../architecture/overview.md`](../architecture/overview.md) and the coding rules in
[`../development/conventions.md`](../development/conventions.md).

## Index

| ADR | Title | Status | Date | Workflow |
|---|---|---|---|---|
| [0001](0001-record-architecture-decisions.md) | Record architecture decisions | Accepted | 2026-09-24 | W1 |
| [0002](0002-build-and-dependency-management.md) | Build system and dependency management | Accepted (amended for build-machine paths in the binaries, W12) | 2026-09-24 | W0, recorded in W1 |
| [0003](0003-linking-and-licensing.md) | Linking model and third-party licensing | Accepted | 2026-09-24 | W1 |
| [0004](0004-error-handling.md) | Error handling with `lucent::Expected` | Accepted | 2026-09-24 | W1 |
| [0005](0005-raw-decoder-boundary.md) | RAW decoder boundary and decoded-RAW data contract | Accepted | 2026-09-25 | W1 |
| [0006](0006-render-pipeline.md) | First render path: stage order, colour contracts and provisional display rendering | Accepted | 2026-09-26 | W2 |
| [0007](0007-application-shell.md) | Application shell: UI adapters over in-process application services | Accepted | 2026-09-27 | W3 |
| [0008](0008-catalog-and-source-identity.md) | Persistent library: catalog, source identity, duplicates, relinking and thumbnail cache | Accepted (amended for pre-update backups and catalog fixtures of every schema version, see 0022) | 2026-10-03 | W4 |
| [0009](0009-windows-packaging.md) | Windows packaging: installed layout, dependency audit and per-user Inno Setup installer | Accepted (amended for build-machine paths and the signing rehearsal, W12; the update handoff and rehearsal, see 0022) | 2026-10-03 | W5 |
| [0010](0010-develop-pipeline-and-recipes.md) | Develop pipeline, recipes and interactive previews | Accepted (amended for DevelopSettings v2, see 0015) | 2026-10-03 | W6 |
| [0011](0011-natural-v1-automatic-settings.md) | Natural v1 automatic settings: metric contract, proposals, bounded search and confidence | Accepted | 2026-10-03 | W8 |
| [0012](0012-durable-batch-and-workers.md) | Durable batch export: worker processes, memory-budgeted scheduling, journaled publication and recovery | Accepted (amended for the aggregate worker memory limit, W12; batches frozen before an update, see 0022) | 2026-10-04 | W7 |
| [0013](0013-automatic-settings-integration.md) | Automatic settings in the application: where Auto runs, the full-resolution final check, revisions and records, and automatic batches | Accepted (amended for the W8 integration closure and for geometry and masks, W9 part 2) | 2026-10-04 | W8 application wiring |
| [0014](0014-dng-opcode-processing.md) | DNG opcode processing at the stages the specification defines (amends 0005 decision 8, 0006 sections 1 and 5) | Accepted | 2026-10-04 | W9 |
| [0015](0015-geometry-masks-and-chroma-denoise.md) | Geometry, masks and guided chroma denoising: DevelopSettings v2, coordinate frames, operation order and preview tolerances (amends 0010) | Accepted (amended for phase 2: recipes, paste, editor services, Auto; and for geometry version 2, see 0017) | 2026-10-05 | W9 part 2 |
| [0016](0016-camera-qualification.md) | Camera qualification: mode keys, evidence rules, proposed states and the support table | Accepted (phase 1: harness, expectations, table; amended for the camera support database, see 0018, by the closing review: colour evidence, regression net, provenance, and for the camera correctness gate, see 0020) | 2026-10-05 | W10 |
| [0017](0017-camera-image-area.md) | The camera's image area: decoded-RAW contract 1.4, geometry version 2 and the W2 camera_area stage (amends 0005, 0006, 0014, 0015, 0016) | Accepted (amended by the closing review: Canon aspect-ratio settings, lone DNG crop tags, the editor's frame, batches frozen before the update) | 2026-10-07 | W10 |
| [0018](0018-camera-support-data.md) | Camera support data: versioned asset manifests, the camera support database and the four support states (amends 0008, 0009, 0012, 0016) | Accepted (amended by the closing review: database schema 2, support rules 2; what an update does to camera support data, see 0022) | 2026-10-08 | W10 |
| [0019](0019-learned-appearance-research.md) | Learned appearance research: reproduce Image-Adaptive-3DLUT, keep its weights and data out of Lucent, and implement the method natively | Accepted | 2026-10-09 | W11 (phase 1: research) |
| [0020](0020-camera-correctness-gate.md) | Camera correctness gate: an independent colour reference from colour charts, and rotated fixtures for every image-area source (amends 0016) | Accepted | 2026-10-09 | W12 |
| [0021](0021-performance-baseline.md) | Performance baseline: a reproducible harness over the shipped programs, the first baseline, and regression budgets only where the variance allows | Accepted | 2026-10-09 | W12 |
| [0022](0022-updates-and-migrations.md) | Updates and migrations: an offline-first "Check for updates" over a signed release manifest, catalog fixtures of every schema version, and the update rehearsal (amends 0008, 0009, 0012, 0018) | Accepted | 2026-10-09 | W12 |

## Conventions

- File names are `NNNN-short-kebab-title.md`. Numbers are sequential and never reused.
- Status is one of `Proposed`, `Accepted`, `Superseded by ADR NNNN`, `Deprecated` or `Rejected`.
- An accepted decision is not rewritten. A new ADR supersedes it, and the old one gets a status
  update and a link to its successor.
- Add every new ADR to the index above in the same change that introduces it.
