# Engineering conventions

These rules apply to all Lucent C++ code. They exist so modules written at different times
compose cleanly and stay forward compatible. The blueprint
(`docs/blueprint/lucent-desktop-raw-processing-blueprint.md`) is the product and architecture
authority; this file covers how code is written.

## Language and build

- C++20, MSVC (`/W4 /permissive- /WX`), 64-bit Windows. No compiler extensions.
- Each module is a static library `lucent_<module>` (alias `Lucent::<module>`) created with
  `lucent_add_library()`; tests use `lucent_add_test()`; executables use `lucent_add_executable()`
  (see `cmake/LucentTargets.cmake`).
- Layout: public headers in `src/<module>/include/lucent/<module>/`, implementation and private
  headers in `src/<module>/src/`, unit tests in `src/<module>/tests/`.
- A module may depend only on the modules below it in `docs/architecture/overview.md`.
  Third-party types do not appear in public headers unless the dependency is deliberately part
  of the contract (tl-expected is; LibRaw, SQLite, Little CMS and Qt are not).
- Build with `tools\build.ps1` (loads the MSVC environment). Keep parallelism modest
  (`-Jobs 2..4`) on the 12 GB reference laptop.

## Naming and style

- Namespaces: `lucent::<module>` (for example `lucent::raw`). The error model lives directly in
  `lucent`.
- Types `PascalCase`; functions, variables and files `snake_case`; constants `kPascalCase`;
  enumerators `PascalCase` inside `enum class`; private data members end with `_`.
- `#pragma once`; headers are self-contained and include what they use; no `using namespace`
  in headers.
- Formatting follows `.clang-format` (4-space indent, 110 columns).
- Comments explain intent, invariants, units and contracts; they do not narrate the code.

## Errors

- Fallible functions return `lucent::Expected<T>` or `lucent::Status` (`lucent/core/error.hpp`).
  Return failures with `lucent::fail(code, message, where, native_code)`.
- `where` is a stable dotted location (`raw.libraw.unpack`), useful in logs and reports.
- Exceptions never cross a module boundary. Code that calls throwing third-party APIs catches at
  the boundary and converts to `Error` (`std::bad_alloc` becomes `ErrorCode::OutOfMemory`).
- Precondition violations that indicate a Lucent bug use `ErrorCode::Internal`; never silently
  clamp or ignore invalid state.

## Files, paths and text

- Paths are `std::filesystem::path` end to end. Never round-trip a path through a narrow string
  on Windows; use the wide-character APIs (`_wfopen`, `CreateFileW`, LibRaw's wide `open_file`).
- Strings in APIs, logs, JSON and the catalog are UTF-8 (`std::string`). Convert with the
  helpers in `lucent/core/text.hpp`.
- Original photographs are opened read-only with sharing that permits other readers. Nothing in
  Lucent writes to a source file.

## Numerics and images

- Working pixels are `float` (32-bit). Scene-linear data may be negative or above 1.0; never clip
  merely to fit a container. Clipping happens only in declared output encodings.
- Every image carries its colour encoding (primaries, white point, transfer, range, reference)
  as data; never infer it from context.
- Matrices are row-major and multiply column vectors: `out = M * in`.
- Distances such as filter radii are defined in full-resolution source pixels.
- Determinism: identical inputs, assets and versions produce identical outputs within documented
  tolerances. Do not let unordered-container iteration order or uninitialised memory reach
  output. Record any random seed.

## Versioned contracts and forward compatibility

- Anything persisted or exchanged between processes (recipes, catalog schema, worker protocol,
  model and asset manifests, job records) carries an explicit version.
- Readers reject newer unknown versions with `ErrorCode::VersionMismatch`; they never treat an
  unknown operation or field as a no-op when it affects pixels.
- Numeric values of persisted enums never change; append new values only.

## Tests

- GoogleTest. Test suite names are `PascalCase` and describe the unit (`ColorMath`,
  `LibRawAdapter`).
- Unit tests are deterministic and self-contained (synthetic data). Tests that need
  downloaded camera fixtures call `GTEST_SKIP()` with a clear message when a fixture is absent,
  so a fresh checkout still passes.
- Numerical tests compare against published reference values or an independent computation,
  and state the tolerance with a reason.

## Commits

- One logical change per commit, imperative subject (`raw: add LibRaw probe adapter`), body
  explaining why when it is not obvious.
