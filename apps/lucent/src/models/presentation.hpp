#pragma once

// User-facing text for library items and export jobs, shared by the list models and CurrentItem.
// Everything here is a pure function of its arguments (locale included) and is safe to call from
// any thread. Strings go through QCoreApplication::translate (context "lucent::ui::presentation")
// so they can be localised later.

#include <QLocale>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

#include "services/service_types.hpp"

namespace lucent::ui::presentation {

// The file name; "IMG_0001.ARW (image 2 of 3)" for an item of a multi-image file.
[[nodiscard]] QString file_name(const LibraryItem& item);
[[nodiscard]] QString folder_path(const LibraryItem& item);  // native separators

// "Canon EOS R5". A model that already starts with the make's first word is shown alone
// ("NIKON CORPORATION" + "NIKON Z 6" is "NIKON Z 6"). "Unknown camera" when neither is known.
[[nodiscard]] QString camera_label(const LibraryItem& item);
[[nodiscard]] QString lens_label(const LibraryItem& item);  // empty when unknown

// Date and time in the locale's short format; "Capture time unknown" when unknown.
[[nodiscard]] QString capture_time_label(const LibraryItem& item, const QLocale& locale);

// "6000 × 4000 · 24.0 MP"; empty when unknown.
[[nodiscard]] QString dimensions_label(const LibraryItem& item, const QLocale& locale);

// "1/250 s · f/4 · ISO 400 · 35 mm", leaving out unknown parts; empty when nothing is known.
[[nodiscard]] QString exposure_label(const LibraryItem& item, const QLocale& locale);

// "1/250 s" when 1/seconds is within 5 % of an integer and seconds < 1, otherwise seconds with at
// most one decimal ("0.3 s", "2 s", "1.3 s"). Empty for a non-positive or non-finite value.
[[nodiscard]] QString shutter_label(double seconds, const QLocale& locale);
// "f/4", "f/5.6". Empty for a non-positive or non-finite value.
[[nodiscard]] QString aperture_label(double f_number, const QLocale& locale);

// Size with binary multiples and traditional unit names ("38.8 MB"), as Windows Explorer shows
// it; empty when negative (unknown).
[[nodiscard]] QString file_size_label(qint64 bytes, const QLocale& locale);

// Stable key for styling in QML: "unknown", "verified", "unverified", "limited", "unsupported".
[[nodiscard]] QString support_key(SupportStatus status);
// Blueprint section 3 wording: "Verified", "Compatible, unverified", "Limited", "Unsupported";
// "Not checked" for Unknown.
[[nodiscard]] QString support_label(SupportStatus status);
// Compact badge text: "Verified", "Unverified", "Limited", "Unsupported", "Not checked".
[[nodiscard]] QString support_short_label(SupportStatus status);
// What a state means for the photographer (blueprint section 3; W10, ADR 0018): what Lucent
// checked, and what it does with such photos (automatic exports, review notes, skipping).
[[nodiscard]] QString support_meaning(SupportStatus status);
// The four states for Help > About, best first: {key, label, meaning} maps.
[[nodiscard]] QVariantList support_states();
// Help > About's camera support database (W10, ADR 0018) as QML reads it: available, version,
// qualifiedOn, decoder, location, bundled, modes, verified, compatibleUnverified, limited,
// unsupported, verifiedEntriesHold, problem, notes, and summary: one sentence ("Version 1.0.0,
// qualified 2026-10-08: 34 camera modes, 14 verified, ...") or the problem.
[[nodiscard]] QVariantMap camera_support_map(const CameraSupportInfo& info);
// Help > About's lens database (W13, ADR 0023) as QML reads it: available, version, library,
// location, cameras, lenses, problem, and summary: one sentence ("Version 1.0.0 of the Lensfun lens
// database (Lensfun 0.3.4): 1571 lenses of 1057 cameras.") or the problem.
[[nodiscard]] QVariantMap lens_database_map(const LensDatabaseInfo& info);

[[nodiscard]] QString processing_label(ProcessingStatus status);

// True when Lucent's latest render of `item` lacks a mandatory interpretation step
// (RenderInfo::incomplete): it is shown, but never as an accurate result.
[[nodiscard]] bool render_incomplete(const LibraryItem& item);

// Stable key for styling in QML: "notprocessed", "queued", "processing", "processed", "failed",
// and "incomplete" for a processed item whose render is incomplete.
[[nodiscard]] QString processing_key(const LibraryItem& item);
// processing_label(item.processing), or "Incomplete render" for an incomplete render.
[[nodiscard]] QString processing_label(const LibraryItem& item);
// item.processing_detail, or for an incomplete render a sentence saying what that means.
[[nodiscard]] QString processing_detail(const LibraryItem& item);

// One label per set flag, in bit order.
[[nodiscard]] QStringList flag_labels(ItemFlags flags);

// Stable keys of the source badges for styling in QML, most urgent first: "missing", "changed",
// "duplicate", "removable" (ReviewRecommended has its own "Review" badge on the card, W8).
[[nodiscard]] QStringList flag_keys(ItemFlags flags);
// Compact badge text for a flag key: "Missing", "Changed", "Duplicate", "Removable".
[[nodiscard]] QString flag_badge_label(const QString& key);
// The tooltip of a badge: what the state means and what to do about it.
[[nodiscard]] QString flag_badge_detail(const QString& key);

// "none", "picked", "rejected".
[[nodiscard]] QString pick_key(PickState pick);
// "Picked", "Rejected"; empty for None.
[[nodiscard]] QString pick_label(PickState pick);

// One or two sentences about the item's file for the details panel: missing or changed on disk,
// on removable media, a verified copy and its original, content duplicates. Empty when there is
// nothing to say.
[[nodiscard]] QString source_note(const LibraryItem& item);

// A sentence for screen readers describing a contact-sheet cell.
[[nodiscard]] QString accessible_label(const LibraryItem& item, const QLocale& locale);

// "sRGB", "Display P3".
[[nodiscard]] QString color_space_label(ExportColorSpace color_space);
// "JPEG · sRGB · quality 95", "TIFF 16-bit · Display P3".
[[nodiscard]] QString export_options_label(const ExportOptions& options);

// "Neutral v0 (provisional) · rendered in 0.95 s".
[[nodiscard]] QString render_label(const RenderInfo& info, const QLocale& locale);

// Stable key for styling in QML: "queued", "running", "completed", "failed", "cancelled".
[[nodiscard]] QString job_state_key(ExportJobState state);
[[nodiscard]] QString job_state_label(ExportJobState state);

}  // namespace lucent::ui::presentation
