#pragma once

// The metadata an export carries, from the source's capture metadata and the user's rights text
// (blueprint section 7: "Preserve useful capture EXIF and copyright; provide an explicit GPS
// include/remove option"). The export settings' MetadataPolicy is applied later, by
// exporter::write_verified (prepare_metadata), so this mapping copies every fact.

#include <optional>
#include <string>

#include <lucent/export/metadata.hpp>
#include <lucent/raw/capture_metadata.hpp>

namespace lucent::worker_app {

// Copies the facts of `capture` into an ExportMetadata field by field. `artist` and `copyright`,
// when set, replace the source's own values (an empty string clears them).
[[nodiscard]] exporter::ExportMetadata make_export_metadata(const raw::CaptureMetadata& capture,
                                                            const std::optional<std::string>& artist,
                                                            const std::optional<std::string>& copyright);

}  // namespace lucent::worker_app
