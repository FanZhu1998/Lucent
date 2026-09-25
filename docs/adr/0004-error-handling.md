# ADR 0004: Error handling with `lucent::Expected`

- **Status:** Accepted
- **Date:** 2026-09-24
- **Workflow:** W1 (Foundation and RAW probe)

## Context

Failure is an ordinary result in Lucent, not an exceptional one. The blueprint requires:

- a `RawDecoder` interface with "cancellation, limits, and structured error reporting"
  (section 3);
- batches in which "an undecodable file or an image missing a mandatory interpretation step is
  skipped with a specific explanation; other files continue" (section 2);
- job records and completion reports that persist why a file failed, and recovery actions for
  disk-full, removed-drive, permission-loss and missing-source states (section 7);
- recipes and worker protocols that reject newer unknown versions explicitly (section 8).

Failures must therefore be values that can be inspected, logged, persisted and translated into
user-facing text. Their categories must stay stable across releases. Third-party libraries report
errors in their own ways: LibRaw uses return codes and can throw internally, SQLite uses result
codes, Win32 uses `GetLastError()`, Qt uses return values and signals, and the C++ standard library
throws (`std::bad_alloc`, `std::filesystem::filesystem_error`). Code is built with `/EHsc`, so
exceptions exist and must be contained.

The toolchain baseline is C++20 (ADR 0002). MSVC offers `std::expected` only in C++23 mode.

## Decision

The error model lives in `lucent_core`, header `src/core/include/lucent/core/error.hpp`, in
namespace `lucent`.

1. **Result types.** Every fallible function returns `lucent::Expected<T>` or `lucent::Status`
   (`Expected<void>`). `Expected<T>` is an alias for `tl::expected<T, lucent::Error>` (tl-expected
   1.3.1). Its interface matches C++23 `std::expected`, so the alias can switch to `std::expected`
   when the toolchain baseline moves to C++23.
2. **The error value.** `lucent::Error` is a plain struct:
   - `code`: a `lucent::ErrorCode` category. It defaults to `Internal`, so a default-constructed
     error is visibly a defect.
   - `message`: developer-facing English. The UI never shows it verbatim. User-visible text is
     produced from `code`, `where` and structured context.
   - `where`: a stable dotted location such as `raw.libraw.unpack` or `core.file.read`, used in
     logs, reports and tests.
   - `native_code`: the underlying LibRaw, Win32 or SQLite code, or 0. It is recorded for
     diagnostics only.
   - `describe()`: formats `where: message (code, native N)` for logs.
3. **Stable, persisted categories.** `ErrorCode` is an `enum class` with `std::uint16_t` storage.
   Its fifteen categories run from `Unknown = 1` to `Internal = 15` and include `NotFound`,
   `Corrupt`, `Unsupported`, `LimitExceeded`, `OutOfMemory`, `Cancelled`, `Timeout` and
   `VersionMismatch`. The numeric values are persisted in job records and batch reports, so
   enumerators are never renumbered or reused: new categories are appended only. `to_string(code)`
   returns a stable lowercase identifier (`not_found`) for logs and reports. Categories stay broad;
   detail goes into `where`, `message` and `native_code`, not into new codes.
4. **Construction helpers.** Failures are returned with
   `lucent::fail(code, message, where, native_code)`, which yields a `lucent::Unexpected`, and
   successes of `Status` with `lucent::ok()`. Both are `[[nodiscard]]`, as is `to_string`.
5. **Exceptions never cross a module boundary.** Code that calls throwing APIs (the standard
   library, third-party libraries) catches at that boundary and converts the exception to an
   `Error`: `std::bad_alloc` becomes `ErrorCode::OutOfMemory`, and other exceptions map to the most
   specific category, with `Internal` as the last resort. Exceptions stay reserved for programming
   errors and allocation failure inside third-party code; they are not used to report expected
   failures.
6. **Defects are not hidden.** A violated precondition that indicates a Lucent bug returns
   `ErrorCode::Internal`. Invalid state is never silently clamped or ignored.
7. **Rules for forward compatibility:**
   - Code names the Lucent aliases (`lucent::Expected`, `lucent::Status`, `lucent::Unexpected`,
     `lucent::fail`) rather than `tl::` names.
   - Code uses only the monadic operations that `std::expected` also has (`and_then`, `or_else`,
     `transform`, `transform_error`), never the tl-only `map` and `map_error`, so the switch to
     `std::expected` stays mechanical.
   - New fields may be appended to `Error` with default values, which keeps existing aggregate
     initialisation (as used by `fail`) valid. Existing fields are not reordered or removed.
   - tl-expected is a deliberate part of the public contract. It is the only third-party type
     allowed in public headers without further justification (`docs/development/conventions.md`).

## Consequences

- Failure paths are explicit in every signature and cannot be ignored silently (`[[nodiscard]]`).
  They are straightforward to unit test by checking `code` and `where`.
- The batch scheduler, reports and job records can handle one file's failure as data and continue
  with the next file. `Cancelled` and `Timeout` are ordinary results.
- Persisted codes keep their meaning across releases because values are append-only. `to_string`
  returns `unknown` for a value it does not recognise, for example one written by a newer release,
  instead of failing.
- Propagating errors needs more code than exceptions: `if (!r) return lucent::Unexpected(r.error());`
  or the monadic operations.
- Module authors must write catch-and-convert wrappers around throwing third-party calls, and review
  must check that no exception escapes a module's public functions.
- The single `Error` shape carries no structured context beyond `where`, `message` and
  `native_code`, such as a file path or a chain of causes. If reports need that, it is added as an
  appended field under the rules above, through a new ADR if it changes persisted records.
- The switch to C++23 `std::expected` changes an alias and `tl::unexpected` usage in one header,
  provided the forward-compatibility rules were followed.

## Alternatives considered

- **Exceptions throughout.** Idiomatic C++, but an exception crossing a DLL, worker-process or
  batch boundary is easy to lose. Exceptions are invisible in signatures, and they make "expected"
  failures such as unsupported cameras look exceptional. Rejected as the primary mechanism.
- **`std::error_code` and `std::error_category`.** Standard and extensible, but it carries no
  message or location, category identity is awkward to persist, and it still needs out-parameters
  or a result type. Rejected.
- **Return codes with out-parameters.** Error-prone, and easy to ignore without `[[nodiscard]]`
  discipline. Rejected.
- **`std::expected` now.** It needs C++23 mode (`/std:c++23preview` or `/std:c++latest` on MSVC
  14.44), which the project does not adopt yet. The alias makes the later switch cheap. Deferred.
- **Boost.Outcome, Boost.LEAF or `absl::Status`.** Capable, but each adds a large dependency to the
  public contract for little gain over tl-expected, which is header-only, CC0-licensed and mirrors
  the future standard type. Rejected.
- **Fine-grained error codes (one per failure site).** Unstable across releases and unsuitable for
  persistence. Broad categories plus `where` give the same diagnostic precision. Rejected.
