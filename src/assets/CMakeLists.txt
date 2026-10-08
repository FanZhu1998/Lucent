# lucent_assets: versioned, hash-verified data assets (W10 phase 3, ADR 0018). Pure C++.
#
#   asset_manifest.hpp   the asset manifest "lucent.asset-manifest" v1 (id, version, minimum Lucent
#                        version, content schema, SHA-256 and size per file)
#   asset_store.hpp      locations (per-user data packs, refused until signatures exist; the bundled
#                        folder next to the executable), lookup order and verification before activation
#   bundled_assets.hpp   the manifests this build was made with (pins, generated below)
#   mode_key.hpp         the qualification mode key (ADR 0016; moved from apps/lucent-qualify)
#   camera_support.hpp   the camera support database "lucent.camera-support" v1: mode-key patterns to
#                        the blueprint's four states, limitations, evidence and qualification basis
#
# The bundled assets themselves live in the source tree's assets/ folder (LUCENT_BUNDLED_ASSETS and
# lucent_asset_files() in cmake/LucentPackaging.cmake). For each one this file records the SHA-256 of
# its manifest in a generated source (the pin bundled_manifest_pin() returns) and copies the manifest
# and its files to <build>/bin/assets/<id>, where executables of the build tree find them exactly as
# the installed Lucent.exe finds <install>/assets/<id>. Editing a manifest reconfigures the build.

find_package(nlohmann_json CONFIG REQUIRED)

set(_lucent_asset_entries "")
set(_lucent_asset_outputs "")
list(LENGTH LUCENT_BUNDLED_ASSETS LUCENT_BUNDLED_ASSET_COUNT)
foreach(_lucent_asset IN LISTS LUCENT_BUNDLED_ASSETS)
    lucent_asset_files(${_lucent_asset} _lucent_asset_files _lucent_asset_sha256)
    string(APPEND _lucent_asset_entries "    BundledAsset{\"${_lucent_asset}\", \"${_lucent_asset_sha256}\"},\n")
    foreach(_lucent_file IN LISTS _lucent_asset_files)
        set(_lucent_source "${LUCENT_ASSETS_SOURCE_DIR}/${_lucent_asset}/${_lucent_file}")
        set(_lucent_copy "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/assets/${_lucent_asset}/${_lucent_file}")
        add_custom_command(
            OUTPUT "${_lucent_copy}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_lucent_source}" "${_lucent_copy}"
            DEPENDS "${_lucent_source}"
            COMMENT "Copying the bundled asset file ${_lucent_asset}/${_lucent_file}"
            VERBATIM)
        list(APPEND _lucent_asset_outputs "${_lucent_copy}")
    endforeach()
endforeach()
set(LUCENT_BUNDLED_ASSET_ENTRIES "${_lucent_asset_entries}")
configure_file(bundled_assets.cpp.in "${CMAKE_CURRENT_BINARY_DIR}/generated/bundled_assets.cpp" @ONLY)
add_custom_target(lucent_bundled_assets ALL DEPENDS ${_lucent_asset_outputs})
set_target_properties(lucent_bundled_assets PROPERTIES FOLDER "lucent/assets")

lucent_add_library(assets
    SOURCES
        include/lucent/assets/asset_manifest.hpp
        include/lucent/assets/asset_store.hpp
        include/lucent/assets/bundled_assets.hpp
        include/lucent/assets/camera_support.hpp
        include/lucent/assets/mode_key.hpp
        src/asset_manifest.cpp
        src/asset_store.cpp
        src/camera_support.cpp
        src/json_reader.hpp
        src/mode_key.cpp
        "${CMAKE_CURRENT_BINARY_DIR}/generated/bundled_assets.cpp"
    PUBLIC_DEPS
        Lucent::core
        Lucent::raw
        Lucent::engine
    PRIVATE_DEPS
        nlohmann_json::nlohmann_json
)
# Everything that links the library finds the bundled assets next to its executable.
add_dependencies(lucent_assets lucent_bundled_assets)

lucent_add_test(lucent_assets_tests
    SOURCES
        tests/asset_test_support.hpp
        tests/asset_manifest_test.cpp
        tests/asset_store_test.cpp
        tests/camera_support_test.cpp
        tests/bundled_camera_support_test.cpp
    DEPS
        Lucent::assets
    LABELS
        assets unit
)
