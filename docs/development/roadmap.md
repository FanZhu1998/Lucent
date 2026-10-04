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
| W5 | Installer slice | 2 | windeployqt deployment, Inno Setup per-user installer, VC++ runtime handling, clean-machine offline test | Done except the clean-machine gate, pending commit and merge (`tools\package.ps1` builds the unsigned `Lucent-Setup-0.1.0-x64.exe`: audited layout with QML and Qt-plugin checks and a smoke run, licence bundle with Qt's third-party notices, SPDX 2.3 SBOM validated by spdx-tools, list-based upgrades, "Open with" that never changes the default program; ADR 0009. `installer\tests\Test-InstalledPackage.ps1 -RuntimeOutcomes` passes 46/46 on the development machine. **Pending:** the same test on a clean Windows 10/11 machine or Windows Sandbox without the Visual C++ runtime, offline, including the real elevated redistributable; not for public distribution until W12 settles the LGPL source provision. Review findings PKG-1 to PKG-12 resolved, PDB paths in vcpkg DLLs deferred to W12) |
| W6 | Recipes and editor | 3 | Immutable recipes, operation versions, preview generations, undo/redo, exposure/WB/tone/colour controls, guided-filter local tone | Done, pending commit and merge (`DevelopSettings` v1 and its operations: white balance with DNG SDK temperature/tint, exposure, guided-filter highlights and shadows, contrast, tone curve, saturation and vibrance, base look "neutral" v1; two-stage rendering with quick-then-refine edit previews; "lucent.develop" v1 recipes (RFC 8785 canonical JSON, SHA-256 identity) as immutable revisions in catalog schema v2 (minimum reader v2); undo/redo, reset, revert, locks, copy/paste by category; recipe-aware previews, thumbnails and exports; the Edit view with histogram, clipping overlay, before/after, tone curve and history; ADR 0010. `dev`: 1266 tests, 1235 passed, 29 fixture renders skipped by their owner, 2 disabled timing tests; `dev-core`: 1106 tests, 1075 passed, 29 skipped, 2 disabled; review findings ENG-1 to ENG-7 and APP-01 to APP-09 resolved) |
| W7 | Durable batch | 4 | Worker process + protocol, memory admission, job journal, no-clobber publication, pause/cancel/resume, reports | Done, pending commit and merge (`lucent-worker.exe` behind the versioned pipe protocol "lucent.worker" v1 in Job Objects; a memory-budgeted coordinator with calibrated admission of a second worker; the catalog job journal (schema v3, v4: leases, events, publication journal, immutable receipts, interruption counts, report emission); held-handle verification and no-clobber publication; restart recovery that verifies outputs against receipts and waits for an unreachable destination instead of guessing; interruptions bounded apart from transient retries; pause / cancel / resume and destination attentions (full, unavailable, permission, a replaced folder with Use this folder); EXIF capture metadata with an explicit GPS policy; JSON and HTML completion reports; single exports as prioritised batches of one; the Export batch dialog and the Exports view with unfinished batches, paging, attentions outside the view and keyboard multi-selection; ADR 0012, ADR 0007 decision 14. `dev`: 1526 tests, 1495 passed, 29 fixture renders skipped by their owner, 2 disabled timing tests; `dev-core`: 1330 tests, 1299 passed, 29 skipped, 2 disabled; closing review findings DUR-1 to DUR-7 and UIX-01 to UIX-16 resolved, DUR-8 (a hard aggregate memory limit across worker processes) deferred to W12) |
| W8 | Natural v1 optimiser | 4 | Metric contract, bounded candidate search, confidence flags, related-shot consistency | Planned |
| W9 | Masks, geometry, optics | 3 | Brush/gradient/radial masks, crop/rotate/straighten, Lensfun corrections, guided chroma denoise | Planned |
| W10 | Multi-brand qualification | 5 | Qualification harness, per-model/mode support table, compatibility states in UI | Planned |
| W11 | Learned appearance research | 6 | Image-Adaptive-3DLUT reproduction, native trilinear LUT, ONNX parity, promotion gate | Planned |
| W12 | Release hardening | 7 | Signing, updates, migrations rehearsal, licence inventory/SBOM, corresponding-source provision for the LGPL libraries (from W5), builder paths out of the vcpkg DLLs (`/PDBALTPATH`, from W5), performance report (including whether worker processes get a hard aggregate memory limit, from W7) | Planned |

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
