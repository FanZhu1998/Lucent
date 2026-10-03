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

## Conventions

- File names are `NNNN-short-kebab-title.md`. Numbers are sequential and never reused.
- Status is one of `Proposed`, `Accepted`, `Superseded by ADR NNNN`, `Deprecated` or `Rejected`.
- An accepted decision is not rewritten. A new ADR supersedes it, and the old one gets a status
  update and a link to its successor.
- Add every new ADR to the index above in the same change that introduces it.
