#include <format>
#include <new>
#include <string>
#include <system_error>
#include <utility>

#include <windows.h>

#include <lucent/assets/asset_store.hpp>
#include <lucent/assets/bundled_assets.hpp>
#include <lucent/core/file.hpp>
#include <lucent/core/text.hpp>

namespace lucent::assets {

namespace {

constexpr std::string_view kWhereLoad = "assets.load";

std::string display(const std::filesystem::path& path) {
    return core::path_to_utf8_lossy(path);
}

std::string_view as_text(const std::vector<std::byte>& bytes) noexcept {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// A fresh error at "assets.load.<step>".
Unexpected refuse(ErrorCode code, std::string message, std::string_view step) {
    return fail(code, std::move(message), std::format("{}.{}", kWhereLoad, step));
}

// The relative manifest path of a file as a filesystem path (each '/' segment appended).
std::filesystem::path relative_path(std::string_view manifest_path) {
    std::filesystem::path out;
    std::size_t start = 0;
    while (start <= manifest_path.size()) {
        const std::size_t end = std::min(manifest_path.find('/', start), manifest_path.size());
        out /= core::utf8_to_wide_lossy(manifest_path.substr(start, end - start));
        start = end + 1;
    }
    return out;
}

bool folder_exists(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_directory(path, ec);
}

// The version a candidate's manifest declares, when it can be read (diagnostics only).
std::optional<core::SemanticVersion> declared_version(const std::filesystem::path& folder) {
    auto bytes =
        core::read_file_bytes(folder / std::filesystem::path(kAssetManifestFileName), kMaxManifestBytes);
    if (!bytes) {
        return std::nullopt;
    }
    auto manifest = parse_asset_manifest(as_text(*bytes));
    if (!manifest) {
        return std::nullopt;
    }
    return manifest->version;
}

}  // namespace

std::string_view to_string(AssetOrigin origin) noexcept {
    switch (origin) {
        case AssetOrigin::Bundled:
            return "bundled";
        case AssetOrigin::UserDataPack:
            return "user_data_pack";
    }
    return "invalid";
}

std::filesystem::path executable_directory() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            break;
        }
        if (buffer.size() >= 32768) {
            return {};
        }
        buffer.resize(buffer.size() * 2);
    }
    return std::filesystem::path(buffer).parent_path();
}

AssetLocations default_asset_locations(const core::AppDirectories* directories) {
    AssetLocations locations;
    const std::filesystem::path executable = executable_directory();
    if (!executable.empty()) {
        locations.bundled = executable / L"assets";
    }
    if (directories != nullptr && !directories->data.empty()) {
        locations.user = directories->data / L"assets";
    }
    return locations;
}

Expected<AssetRequest> bundled_asset_request(std::string_view id) {
    auto version = core::SemanticVersion::parse(core::build_info().version);
    if (!version) {
        return fail(ErrorCode::Internal,
                    "this build's version is not a semantic version: " + version.error().message,
                    std::string(kWhereLoad));
    }
    AssetRequest request;
    request.id = std::string(id);
    request.app_version = std::move(*version);
    request.pinned_manifest = bundled_manifest_pin(id);
    return request;
}

const std::string* VerifiedAsset::file(std::string_view path) const noexcept {
    const auto found = files.find(path);
    return found == files.end() ? nullptr : &found->second;
}

Expected<VerifiedAsset> verify_asset_folder(const std::filesystem::path& folder, AssetOrigin origin,
                                            const AssetRequest& request) {
    try {
        const std::filesystem::path manifest_path = folder / std::filesystem::path(kAssetManifestFileName);
        auto manifest_bytes = core::read_file_bytes(manifest_path, kMaxManifestBytes);
        if (!manifest_bytes) {
            if (manifest_bytes.error().code == ErrorCode::NotFound) {
                return refuse(
                    ErrorCode::NotFound,
                    std::format("the asset folder {} has no {}", display(folder), kAssetManifestFileName),
                    "missing");
            }
            Error error = std::move(manifest_bytes.error());
            error.message = std::format("cannot read {}: {}", display(manifest_path), error.message);
            return Unexpected(std::move(error));
        }
        auto digest = core::sha256(std::as_bytes(std::span(*manifest_bytes)));
        if (!digest) {
            return Unexpected(std::move(digest.error()));
        }
        if (request.pinned_manifest && *digest != *request.pinned_manifest) {
            return refuse(
                ErrorCode::Corrupt,
                std::format("{} is not the manifest this build of Lucent was made with (SHA-256 {}, "
                            "expected {}): the installed asset was changed or belongs to another build",
                            display(manifest_path), core::to_hex(*digest),
                            core::to_hex(*request.pinned_manifest)),
                "pin");
        }
        auto manifest = parse_asset_manifest(as_text(*manifest_bytes));
        if (!manifest) {
            Error error = std::move(manifest.error());
            error.message = std::format("{}: {}", display(manifest_path), error.message);
            return Unexpected(std::move(error));
        }
        if (manifest->id != request.id) {
            return refuse(ErrorCode::Corrupt,
                          std::format("{} describes asset '{}', not '{}'", display(manifest_path),
                                      manifest->id, request.id),
                          "id");
        }
        if (request.app_version < manifest->minimum_app_version) {
            return refuse(
                ErrorCode::VersionMismatch,
                std::format("asset '{}' {} needs Lucent {} or later; this is Lucent {}", manifest->id,
                            manifest->version.to_string(), manifest->minimum_app_version.to_string(),
                            request.app_version.to_string()),
                "app_version");
        }

        VerifiedAsset asset;
        asset.origin = origin;
        asset.folder = folder;
        asset.manifest_sha256 = *digest;
        for (const AssetFile& file : manifest->files) {
            const std::filesystem::path path = folder / relative_path(file.path);
            auto bytes = core::read_file_bytes(path, kMaxAssetFileBytes);
            if (!bytes) {
                if (bytes.error().code == ErrorCode::NotFound) {
                    return refuse(ErrorCode::NotFound,
                                  std::format("asset '{}' {} lists {}, which is missing", manifest->id,
                                              manifest->version.to_string(), display(path)),
                                  "missing");
                }
                Error error = std::move(bytes.error());
                error.message = std::format("cannot read {}: {}", display(path), error.message);
                return Unexpected(std::move(error));
            }
            auto file_digest = core::sha256(std::as_bytes(std::span(*bytes)));
            if (!file_digest) {
                return Unexpected(std::move(file_digest.error()));
            }
            if (bytes->size() != file.size || *file_digest != file.sha256) {
                return refuse(
                    ErrorCode::Corrupt,
                    std::format("{} does not match asset '{}' {}: {} bytes with SHA-256 {}, the manifest "
                                "lists {} bytes with SHA-256 {}",
                                display(path), manifest->id, manifest->version.to_string(), bytes->size(),
                                core::to_hex(*file_digest), file.size, core::to_hex(file.sha256)),
                    "hash");
            }
            asset.files.emplace(file.path, std::string(as_text(*bytes)));
        }
        asset.manifest = std::move(*manifest);
        return asset;
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::OutOfMemory, "verifying an asset ran out of memory", std::string(kWhereLoad));
    }
}

AssetLookup find_asset(const AssetLocations& locations, const AssetRequest& request) {
    AssetLookup lookup;
    try {
        if (!valid_asset_id(request.id)) {
            lookup.problem =
                Error{ErrorCode::InvalidArgument, std::format("'{}' is not a valid asset id", request.id),
                      std::string(kWhereLoad), 0};
            return lookup;
        }
        const std::filesystem::path id = core::utf8_to_wide_lossy(request.id);

        // 1. Per-user data packs: reserved for signed packs, refused in this version.
        if (!locations.user.empty()) {
            AssetCandidate user;
            user.origin = AssetOrigin::UserDataPack;
            user.folder = locations.user / id;
            user.present = folder_exists(user.folder);
            if (user.present) {
                user.version = declared_version(user.folder);
                user.refused =
                    Error{ErrorCode::Unsupported,
                          std::format(
                              "the data pack in {} was not used: this version of Lucent uses only the assets "
                              "installed with it, because it cannot verify a data pack's signature yet",
                              display(user.folder)),
                          std::format("{}.unsigned", kWhereLoad), 0};
            }
            lookup.candidates.push_back(std::move(user));
        }

        // 2. The bundled asset.
        AssetCandidate bundled;
        bundled.origin = AssetOrigin::Bundled;
        bundled.folder = locations.bundled.empty() ? std::filesystem::path() : locations.bundled / id;
        bundled.present = !bundled.folder.empty() && folder_exists(bundled.folder);
        if (bundled.present) {
            auto verified = verify_asset_folder(bundled.folder, AssetOrigin::Bundled, request);
            if (verified) {
                bundled.version = verified->manifest.version;
                lookup.asset = std::move(*verified);
            } else {
                bundled.version = declared_version(bundled.folder);
                bundled.refused = verified.error();
                lookup.problem = std::move(verified.error());
            }
        } else {
            lookup.problem =
                Error{ErrorCode::NotFound,
                      bundled.folder.empty()
                          ? std::format("the folder of Lucent's assets is unknown, so asset '{}' cannot "
                                        "be found",
                                        request.id)
                          : std::format("asset '{}' is not installed ({} does not exist)", request.id,
                                        display(bundled.folder)),
                      std::format("{}.missing", kWhereLoad), 0};
        }
        lookup.candidates.push_back(std::move(bundled));
    } catch (const std::bad_alloc&) {
        lookup.asset.reset();
        lookup.problem = Error{ErrorCode::OutOfMemory, "looking for an asset ran out of memory",
                               std::string(kWhereLoad), 0};
    }
    return lookup;
}

}  // namespace lucent::assets
