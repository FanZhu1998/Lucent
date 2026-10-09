#pragma once

// The support rules (W10 phase 3, ADR 0018; blueprint section 3, "User-visible compatibility
// states"): how the application combines what a probe says about a file (the decoded-RAW
// contract's interpretation) with what the camera support database (lucent/assets/
// camera_support.hpp) says about its capture mode, and how stored assessments follow a new
// database.
//
// Rules version 2 (kSupportRulesVersion; version 1 lacked the engine limitations, the probe facts and
// the cautious handling of a missing database), for a probed image:
//   1. The live interpretation comes first. A missing mandatory step (RequiresUnsupportedOperation,
//      Ambiguous, NotDecodable) or a failed probe is always Unsupported, whatever the database says;
//      optional operations the file asks for and Lucent lacks (OptionalOperationsSkipped) make it
//      at least Limited, with those reasons.
//   2. Engine limitations the probe shows (AST-03), whatever the database says: X-Trans mosaic data
//      (demosaiced with the baseline route, "xtrans_baseline_demosaic") and a file with a Dual Pixel
//      component image ("dual_pixel_not_processed") are at least Limited, with an EngineLimitation
//      reason worded like the database's (assets::standard_limitation_text()).
//   3. The database's answer for the probe's mode key (assets::CameraSupportDatabase::lookup()):
//        - not listed (an unknown camera or mode)  -> Compatible, unverified: it decodes cleanly,
//          but Lucent has no representative quality fixtures for it;
//        - Verified                                -> Verified, while the database's qualification
//          basis is this build's (assets::CameraSupport::verified_entries_hold()) AND the probe's
//          image area and DNG opcodes are the qualified file's (assets::derive_mode_variant(),
//          QUA-08); otherwise Compatible, unverified;
//        - Compatible, unverified, Limited, Unsupported -> that state. A Limited or Unsupported
//          entry adds a CameraLimitation reason per limitation (worded by the database; a code an
//          engine limitation already names is not repeated), an Unsupported one a CameraUnsupported
//          reason.
//      Without an active database every newly probed mode is "not listed".
//   4. The result is the worst of 1, 2 and 3 (assets::worse()).
// So Verified needs a Verified entry for the exact mode and file variant AND a complete
// interpretation, and no database answer can make a file with a missing mandatory step anything but
// Unsupported.
//
// Re-assessment. An assessment records how it was made (SupportAssessment::camera: the probe's
// mode key, variant and interpretation status, the database id, version and manifest SHA-256, the
// basis check). When the active database changes (an update brings a new version or content),
// refresh_support() assesses every library item again from those records, without reading a file,
// and stores the results (Library::set_support). When no database is active (the bundled one is
// missing or fails verification), a re-assessment never improves an item (AST-01): the answer of the
// database it was last assessed with is kept (its Limited and Unsupported reasons stay), except that
// Verified becomes Compatible, unverified, which nothing can confirm without the database. Items
// assessed before these rules have no usable record and are probed again by source maintenance
// (probe_outdated()); until then a stored Verified is shown and stored as Compatible, unverified.
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

// What the probe knows about the file beyond the image's metadata (the importer's subimage list).
struct SupportFileFacts {
    // The file holds a Dual Pixel component image (raw::SubimageRole::DualPixelComponent) besides
    // the photograph: Lucent does not process it ("dual_pixel_not_processed").
    bool dual_pixel_component = false;
};

// The assessment of a probed image (see the file comment): the interpretation's assessment
// (assess_interpretation()), the engine limitations of `metadata` and `file`, combined with the
// database's answer for assets::derive_mode_key(metadata). `database` may be null.
[[nodiscard]] SupportAssessment assess_support(const raw::RawMetadata& metadata,
                                               const assets::CameraSupport* database,
                                               const SupportFileFacts& file = {});

// The assessment of a stored item against `database`, from its record (the probe's mode key,
// variant and interpretation, and the live reasons), without reading the file. With a null
// `database` the answer of the database the record names is kept, Verified capped to Compatible,
// unverified (see the file comment). std::nullopt when the assessment has no record (a failed probe,
// or an assessment made before ADR 0018).
[[nodiscard]] std::optional<SupportAssessment> reassess_support(const SupportAssessment& stored,
                                                                const assets::CameraSupport* database);

// True when `support` was made by these rules against exactly `database` (the same asset id,
// version and manifest SHA-256, or none for null) with the same basis check: re-assessing it would
// change nothing.
[[nodiscard]] bool assessed_with(const SupportAssessment& support,
                                 const assets::CameraSupport* database) noexcept;

// What `item`'s support is with `database` active, as refresh_support() would store it; std::nullopt
// when it would not change. Items whose probe is outdated (probe_outdated()) are not re-assessed from
// their record: only a stored Verified is capped to Compatible, unverified until they are probed
// again. The shell shows this for a read-only catalog, which refresh_support() cannot write (AST-06).
[[nodiscard]] std::optional<SupportAssessment> refreshed_support(const SourceItem& item,
                                                                 const assets::CameraSupport* database);

// The support to show for `item` with `database` active: refreshed_support() when it differs, the
// stored assessment otherwise.
[[nodiscard]] SupportAssessment current_support(const SourceItem& item, const assets::CameraSupport* database);

struct SupportRefresh {
    std::uint64_t items = 0;        // library items looked at
    std::uint64_t reassessed = 0;   // assessed again: their record named another database
    std::uint64_t changed = 0;      // of those, items whose status changed
    std::uint64_t needs_probe = 0;  // probed items without a usable record (source maintenance re-reads them)
};

// Stores refreshed_support() of every library item where it differs, with Library::set_support()
// (one change). Errors: those of set_support().
[[nodiscard]] Expected<SupportRefresh> refresh_support(Library& library,
                                                       const assets::CameraSupport* database);

}  // namespace lucent::app
