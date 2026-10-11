#pragma once

// The bundled lens database (ADR 0023): the asset "lens-database" (assets/lens-database in the
// source tree, assets\lens-database next to Lucent.exe), the Lensfun lens database in data format
// version 1 (content "lensfun.database" version 1), unmodified from the Lensfun project's
// published archive, with its licence text (CC BY-SA 3.0, COPYING.CC_BY-SA_3.0) and a provenance
// record (provenance.json: the archive's URL and SHA-256, the Lensfun commit it was generated from,
// the dates). tests/tools/lens_database_asset.py builds and checks the folder.
//
// Loading goes through the same verification as every bundled asset (asset_store.hpp): the
// manifest must hash to the pin compiled into the build and every listed file must have its size
// and SHA-256; a per-user data pack is refused (no signatures yet). Only then are the listed XML
// files, in the manifest's order, handed to the engine, which parses them with Lensfun
// (engine::LensDatabase::load): Lensfun never reads a file itself, so what it parses is exactly what
// was verified.
//
// Thread-safety: functions are reentrant; the database they return is immutable and shareable.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <lucent/assets/asset_store.hpp>
#include <lucent/core/error.hpp>
#include <lucent/engine/lens_correction.hpp>

namespace lucent::assets {

inline constexpr std::string_view kLensDatabaseAssetId = "lens-database";
inline constexpr std::string_view kLensDatabaseContentSchema = "lensfun.database";
inline constexpr std::uint32_t kLensDatabaseContentSchemaVersion = 1;

struct LensDatabaseLookup {
    // The active database; null when none could be activated (`problem` says why).
    std::shared_ptr<const engine::LensDatabase> database;
    // Where the asset was looked for (asset_store.hpp). The activated asset keeps its manifest; its
    // file bytes are released once parsed.
    AssetLookup lookup;
    std::optional<Error> problem;
};

// Finds, verifies and loads the bundled lens database at `locations` (default_asset_locations()).
// Problems: those of bundled_asset_request() and find_asset(), and those of lens_database_from().
[[nodiscard]] LensDatabaseLookup load_lens_database(const AssetLocations& locations);

// The lens database a qualification basis names (camera_support.hpp, QualificationBasis::
// lens_database): "lens-database (manifest <SHA-256 of the bundled manifest, 64 hex digits>)" for the
// asset this build bundles and pins (bundled_assets.hpp), "none" when it bundles none. The pin is the
// asset's identity (its manifest lists every file's SHA-256); loading verifies the asset against it.
[[nodiscard]] std::string lens_database_basis();

// The database of an already verified asset (tools and tests): VersionMismatch
// ("assets.lens.content") unless its content is "lensfun.database" version 1; Corrupt
// ("assets.lens.files") when it lists no XML file; the errors of engine::LensDatabase::load().
[[nodiscard]] Expected<std::shared_ptr<const engine::LensDatabase>> lens_database_from(
    const VerifiedAsset& asset);

}  // namespace lucent::assets
