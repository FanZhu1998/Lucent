#pragma once

// The assets this build bundles (ADR 0018). When the build is configured, src/assets/CMakeLists.txt
// reads assets/<id>/manifest.json for every bundled asset (LUCENT_BUNDLED_ASSETS in
// cmake/LucentPackaging.cmake), records the SHA-256 of the manifest's bytes in a generated source
// file and copies the asset next to the build's executables (<build>\bin\assets\<id>); the
// installer ships the same folder (<install>\assets\<id>). The recorded hash is the pin that
// verify_asset_folder() checks the bundled manifest against (asset_store.hpp), so the bundled data
// is the data the executable was built with. Editing a manifest reconfigures the build.
//
// Thread-safety: constant data.

#include <optional>
#include <span>
#include <string_view>

#include <lucent/core/hash.hpp>

namespace lucent::assets {

struct BundledAsset {
    std::string_view id;               // e.g. "camera-support"
    std::string_view manifest_sha256;  // 64 lower-case hex digits
};

// Every bundled asset, sorted by id.
[[nodiscard]] std::span<const BundledAsset> bundled_assets() noexcept;

// The pinned manifest hash of bundled asset `id`; std::nullopt when this build bundles no such
// asset.
[[nodiscard]] std::optional<core::Digest256> bundled_manifest_pin(std::string_view id) noexcept;

}  // namespace lucent::assets
