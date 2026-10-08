#pragma once

// The support rules (W10 phase 3, ADR 0018; blueprint section 3, "User-visible compatibility
// states"): how the application combines what a probe says about a file (the decoded-RAW
// contract's interpretation) with what the camera support database (lucent/assets/
// camera_support.hpp) says about its capture mode, and how stored assessments follow a new
// database.
//
// Rules version 1 (kSupportRulesVersion), for a probed image:
//   1. The live interpretation comes first. A missing mandatory step (RequiresUnsupportedOperation,
//      Ambiguous, NotDecodable) or a failed probe is always Unsupported, whatever the database says;
//      optional operations the file asks for and Lucent lacks (OptionalOperationsSkipped) make it
//      at least Limited, with those reasons.
//   2. The database's answer for the probe's mode key (assets::CameraSupportDatabase::lookup()):
//        - not listed (an unknown camera or mode)  -> Compatible, unverified: it decodes cleanly,
//          but Lucent has no representative quality fixtures for it;
//        - Verified                                -> Verified, while the database's qualification
//          basis is this build's decoder, contract and pipelines (assets::CameraSupport::
//          verified_entries_hold()); otherwise Compatible, unverified;
//        - Compatible, unverified, Limited, Unsupported -> that state. A Limited or Unsupported
//          entry adds a CameraLimitation reason per limitation (worded by the database), an
//          Unsupported one a CameraUnsupported reason.
//      Without an active database every mode is "not listed".
//   3. The result is the worse of 1 and 2 (assets::worse()).
// So Verified needs a Verified entry for the exact mode AND a complete interpretation, and no
// database answer can make a file with a missing mandatory step anything but Unsupported.
//
// Re-assessment. An assessment records how it was made (SupportAssessment::camera: the probe's
// mode key and interpretation status, the database id and version, the basis check). When the
// active database changes (an update brings a new version, or the bundled asset fails
// verification so that none is active), refresh_support() assesses every library item again from
// those records, without reading a file, and stores the results (Library::set_support). Items
// assessed before the database existed have no record and are probed again by source maintenance
// (probe_outdated()).
//
// Thread-safety: pure functions, except refresh_support(), which is as thread-safe as the library.

#include <cstdint>
#include <memory>
#include <optional>

#include <lucent/application/library.hpp>
#include <lucent/application/source_item.hpp>
#include <lucent/assets/camera_support.hpp>
#include <lucent/core/error.hpp>
#include <lucent/raw/decoded_raw.hpp>

namespace lucent::app {

// The camera support database a session uses (null: none is active).
using CameraSupportRef = std::shared_ptr<const assets::CameraSupport>;

// The assessment of a probed image (see the file comment): the interpretation's assessment
// (assess_interpretation()) combined with the database's answer for assets::derive_mode_key(
// metadata). `database` may be null.
[[nodiscard]] SupportAssessment assess_support(const raw::RawMetadata& metadata,
                                               const assets::CameraSupport* database);

// The assessment of a stored item against `database`, from its record (the probe's mode key and
// interpretation, and the interpretation reasons), without reading the file. std::nullopt when the
// assessment has no record (a failed probe, or an assessment made before ADR 0018).
[[nodiscard]] std::optional<SupportAssessment> reassess_support(const SupportAssessment& stored,
                                                                const assets::CameraSupport* database);

// True when `support` was made by these rules against exactly `database` (the same asset id and
// version, or none for null) with the same basis check: re-assessing it would change nothing.
[[nodiscard]] bool assessed_with(const SupportAssessment& support,
                                 const assets::CameraSupport* database) noexcept;

struct SupportRefresh {
    std::uint64_t items = 0;        // library items looked at
    std::uint64_t reassessed = 0;   // assessed again: their record named another database
    std::uint64_t changed = 0;      // of those, items whose status changed
    std::uint64_t needs_probe = 0;  // probed items without a record (source maintenance re-reads them)
};

// Re-assesses every library item whose record names another database than `database` and stores
// the new assessments with Library::set_support() (one change). Errors: those of set_support().
[[nodiscard]] Expected<SupportRefresh> refresh_support(Library& library,
                                                       const assets::CameraSupport* database);

}  // namespace lucent::app
