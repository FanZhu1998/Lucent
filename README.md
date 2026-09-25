# Lucent

Offline, camera-independent RAW photo development for Windows.

> **Status: pre-alpha, under active development.** Lucent is in its first development increment
> (W1, foundation and RAW probe). There is **no application, installer or user-facing feature
> yet**, and no camera model is verified as supported. The next section describes the product
> Lucent is being built to become, from its blueprint, not what it can do today.

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
| W1 | `core`, `image` and `raw` modules, LibRaw probe adapter, `lucent-probe` developer tool, camera fixture manifest and downloader, rawpy reference-parity script, ADRs | **In progress** |
| W2–W12 | Render path, application shell, catalog, installer, editor, batches, optimiser, masks and optics, qualification, learned appearance, release hardening | Planned |

W1 has no graphical interface. Its output is libraries, tests and a developer command-line tool.

## Repository layout

```text
apps/lucent-probe/   Developer CLI that prints RAW decoder probe results as JSON (W1)
cmake/               CMake helpers and the x64-windows-lucent vcpkg overlay triplet
docs/adr/            Architecture decision records
docs/architecture/   Module layering and dependency rules
docs/blueprint/      Product blueprint: the product authority
docs/development/    Engineering conventions, roadmap, developer setup
docs/legal/          Third-party licence ledger (development ledger)
fixtures/            Camera fixture manifest and reference data; downloads go to fixtures/cache/ (git-ignored)
research/            Python research environment (rawpy reference decoding); development only
src/core/            lucent_core: error model, hashing, files, text, logging, cancellation, versioning
src/image/           lucent_image: pixel containers, CFA descriptors, colour-encoding contracts
src/raw/             lucent_raw: RawDecoder interface, decoded-RAW data contract, LibRaw adapter
tests/               Cross-module integration tests (unit tests live in src/<module>/tests/)
tools/               bootstrap-dev.ps1, dev-env.ps1, build.ps1, fetch-fixtures.ps1
```

Modules are layered: a module may depend only on lower layers. `core` is the lowest, then `image`,
then `raw`, with the planned modules above them. See
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

Lucent's own source code is released under the [MIT License](LICENSE). Third-party components
keep their own licences. LibRaw (used under LGPL-2.1) and Qt (LGPL-3.0) are linked dynamically as
replaceable DLLs. See [ADR 0003](docs/adr/0003-linking-and-licensing.md) and the
[licence ledger](docs/legal/third-party-notices.md).
