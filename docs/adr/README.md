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
| [0002](0002-build-and-dependency-management.md) | Build system and dependency management | Accepted | 2026-09-24 | W0, recorded in W1 |
| [0003](0003-linking-and-licensing.md) | Linking model and third-party licensing | Accepted | 2026-09-24 | W1 |
| [0004](0004-error-handling.md) | Error handling with `lucent::Expected` | Accepted | 2026-09-24 | W1 |
| [0005](0005-raw-decoder-boundary.md) | RAW decoder boundary and decoded-RAW data contract | Accepted | 2026-09-25 | W1 |
| [0006](0006-render-pipeline.md) | First render path: stage order, colour contracts and provisional display rendering | Accepted | 2026-09-26 | W2 |
| [0007](0007-application-shell.md) | Application shell: UI adapters over in-process application services | Accepted | 2026-09-27 | W3 |
| [0008](0008-catalog-and-source-identity.md) | Persistent library: catalog, source identity, duplicates, relinking and thumbnail cache | Accepted | 2026-10-03 | W4 |
| [0009](0009-windows-packaging.md) | Windows packaging: installed layout, dependency audit and per-user Inno Setup installer | Accepted | 2026-10-03 | W5 |
| [0010](0010-develop-pipeline-and-recipes.md) | Develop pipeline, recipes and interactive previews | Accepted | 2026-10-03 | W6 |
| [0011](0011-natural-v1-automatic-settings.md) | Natural v1 automatic settings: metric contract, proposals, bounded search and confidence | Accepted | 2026-10-03 | W8 |
| [0012](0012-durable-batch-and-workers.md) | Durable batch export: worker processes, memory-budgeted scheduling, journaled publication and recovery | Accepted | 2026-10-04 | W7 |
| [0013](0013-automatic-settings-integration.md) | Automatic settings in the application: where Auto runs, the full-resolution final check, revisions and records, and automatic batches | Accepted | 2026-10-04 | W8 application wiring |
| [0014](0014-dng-opcode-processing.md) | DNG opcode processing at the stages the specification defines (amends 0005 decision 8, 0006 sections 1 and 5) | Accepted | 2026-10-04 | W9 |

## Conventions

- File names are `NNNN-short-kebab-title.md`. Numbers are sequential and never reused.
- Status is one of `Proposed`, `Accepted`, `Superseded by ADR NNNN`, `Deprecated` or `Rejected`.
- An accepted decision is not rewritten. A new ADR supersedes it, and the old one gets a status
  update and a link to its successor.
- Add every new ADR to the index above in the same change that introduces it.
