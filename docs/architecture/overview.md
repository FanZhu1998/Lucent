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
| raw | `lucent_raw` | `RawDecoder` interface, decoded-RAW data contract, decoder registry, LibRaw adapter, DNG tag inspection | LibRaw (private) | W1 |
| engine | `lucent_engine` | Typed render graph, operation semantics, sensor normalisation, demosaicing routes, colour pipeline | Little CMS (private) | Planned |
| export | `lucent_export` | JPEG/TIFF encoding, ICC embedding, metadata, verification, durable publication | libjpeg-turbo, libtiff, Little CMS (private) | Planned |
| catalog | `lucent_catalog` | SQLite catalog, migrations, source identity, recipes, job journal | SQLite (private) | Planned |
| analysis | `lucent_analysis` | Metrics, deterministic proposals, optimiser, optional learned inference | ONNX Runtime (private, later) | Planned |
| assets | `lucent_assets` | Versioned camera/lens profiles, models, manifests, licences | — | Planned |
| workers | `lucent_workers` | Worker process lifecycle, protocol, memory admission, job isolation | Qt Core (later) | Planned |
| application | `lucent_application` | Import/export use cases, command history, recipe revisions | Qt Core | Planned |
| ui | `Lucent.exe` | Qt Quick Library / Edit / Exports views | Qt Quick | Planned |

Executables live in `apps/`: `Lucent.exe` (GUI, WIN32 subsystem), `lucent-worker.exe` (isolated
processing worker) and developer tools such as `lucent-probe.exe`.

## Dependency rules

- `core` and `image` are pure C++ with no knowledge of files formats, Qt or databases.
- Third-party libraries are private implementation details of exactly one module. LibRaw types
  never leave `lucent_raw`; SQLite never leaves `lucent_catalog`; Qt never appears below
  `workers`.
- `engine` depends on the decoded-RAW *data contract* from `raw`, not on any decoder adapter.
- The UI never decodes or renders on its thread; it talks to application services, which
  dispatch work to workers.

## Build and dependency management

- CMake presets (`CMakePresets.json`): `dev-core` (no Qt), `dev` (full app), `release`.
- vcpkg manifest mode (`vcpkg.json`) pinned by `vcpkg-configuration.json` to a single registry
  baseline. The overlay triplet `x64-windows-lucent` builds shared, release-only libraries with
  the dynamic CRT. See `docs/adr/0002-build-and-dependencies.md`.
- `tools/dev-env.ps1` enters the MSVC environment; `tools/build.ps1` configures, builds and tests.

## Pixel and colour conventions (summary)

- Working data is float32; scene-linear values may be negative or exceed 1.0.
- Every image edge declares channel layout, colour primaries, white point, transfer function,
  numeric range, orientation and scene/display reference (`lucent::image::ColorEncoding`).
- The initial common working space is scene-linear Rec.2020 primaries with a D65 white point
  (blueprint section 5).
