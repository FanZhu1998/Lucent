# Development roadmap

Lucent is built in increments, each delivered by one development workflow and merged to `main`
only when it builds cleanly and its tests pass. Increments map onto the milestones in section 10
of the blueprint. Order follows the blueprint's rule: basic multi-camera correctness and a working
installer come before sophisticated learned editing.

| # | Increment | Blueprint milestone | Main deliverables | Status |
|---|---|---|---|---|
| W0 | Toolchain | 1 | MSVC Build Tools, CMake, Ninja, Python, vcpkg, Qt (vcpkg source build), repository scaffolding | Done |
| W1 | Foundation and RAW probe | 1–2 | `core`, `image`, `raw` modules; LibRaw adapter (probe/decode); DNG tag inspection; `lucent-probe`; CC0 fixture manifest and downloader; rawpy reference-parity script; ADRs | Done (428 tests; 34/34 fixtures match rawpy parity) |
| W2 | First render path | 2 | `engine`: sensor normalisation, Bayer/X-Trans/linear routes, white balance, camera→XYZ→Rec.2020, orientation; `export`: JPEG + 16-bit TIFF with embedded ICC (Little CMS), write verification | Done (638 tests; 32/32 renderable fixtures within the rawpy gates, ADR 0006) |
| W3 | Application shell | 2 | Qt Quick `Lucent.exe` (WIN32), Library/Edit/Exports views, native file/folder pickers, background render of a preview; `application` services (import, session library, executor with memory admission, previews and camera previews, export) and UI adapters (ADR 0007). The `dev` preset restores Qt from the binary cache | Done (`dev`: 881 tests, 851 passed, 29 fixture renders skipped by their owner, 1 disabled timing test; `dev-core`: 765 tests, 735 passed, 29 skipped, 1 disabled; review findings REV-1 to REV-9 fixed) |
| W4 | Catalog and import | 2 | SQLite catalog with migrations, import-in-place/copy, source identity (SHA-256), relinking, recursion guards, contact sheet with support status | Done, pending commit and merge (`lucent_catalog` schema v1, persistent library, verified copies into a library folder, background hashing and presence checks, duplicates, missing files and relink, removable media, disk thumbnail cache; ADR 0008. `dev`: 1103 tests, 1073 passed, 29 fixture renders skipped by their owner, 1 disabled timing test; `dev-core`: 967 tests, 937 passed, 29 skipped, 1 disabled; review findings REV-1 to REV-10 fixed) |
| W5 | Installer slice | 2 | windeployqt deployment, Inno Setup per-user installer, VC++ runtime handling, clean-machine offline test | Planned |
| W6 | Recipes and editor | 3 | Immutable recipes, operation versions, preview generations, undo/redo, exposure/WB/tone/colour controls, guided-filter local tone | Planned |
| W7 | Durable batch | 4 | Worker process + protocol, memory admission, job journal, no-clobber publication, pause/cancel/resume, reports | Planned |
| W8 | Natural v1 optimiser | 4 | Metric contract, bounded candidate search, confidence flags, related-shot consistency | Planned |
| W9 | Masks, geometry, optics | 3 | Brush/gradient/radial masks, crop/rotate/straighten, Lensfun corrections, guided chroma denoise | Planned |
| W10 | Multi-brand qualification | 5 | Qualification harness, per-model/mode support table, compatibility states in UI | Planned |
| W11 | Learned appearance research | 6 | Image-Adaptive-3DLUT reproduction, native trilinear LUT, ONNX parity, promotion gate | Planned |
| W12 | Release hardening | 7 | Signing, updates, migrations rehearsal, licence inventory/SBOM, performance report | Planned |

## Definition of done for every increment

1. `tools\build.ps1 -Preset dev-core -Test` (and `-Preset dev` once the UI exists) succeeds with
   warnings treated as errors.
2. New public interfaces are documented in their headers; decisions with lasting consequences
   have an ADR.
3. Tests cover the new behaviour, including failure paths; fixture-dependent tests skip cleanly
   when fixtures are absent.
4. An independent review pass has checked correctness against the blueprint, and its confirmed
   findings are fixed.
5. The increment is committed on its own branch and merged into `main` locally.

## Reference environment

- Windows 11 Pro x64, AMD Ryzen 5 5500U (6 cores / 12 threads), 11.9 GB RAM, no discrete GPU.
- Note: the blueprint's reference laptop has 16 GB. Memory budgets (blueprint section 7) must be
  validated on this 12 GB machine as well.
