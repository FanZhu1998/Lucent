# lucent_application: import, the session and catalog-backed libraries, source maintenance
# (hashing, presence checks, relinking), embedded previews, background preview rendering, the
# on-disk thumbnail cache and single-image export services (W3, W4); recipes, their revisions and
# editing commands, and interactive develop previews (W6); the four support states assessed with the
# camera support database (W10, ADR 0018); "Check for updates" over a signed release manifest (W12,
# ADR 0022). Pure C++, no Qt: the user interface
# (apps/lucent) drives these services and marshals their callbacks to its own thread.
# See docs/architecture/overview.md and docs/adr/0007, 0008, 0010.
#
# Layout
#   include/lucent/application/   public headers
#   src/                          implementation and private headers (Win32 path helpers, catalog mapping)
#   tests/                        GoogleTest unit tests (synthetic DNGs) and fixture tests

find_package(nlohmann_json CONFIG REQUIRED)

lucent_add_library(application
    SOURCES
        # Library items, the session library and the catalog-backed library (W4); the support rules
        # over the camera support database (W10, ADR 0018).
        include/lucent/application/source_item.hpp
        include/lucent/application/camera_support.hpp
        include/lucent/application/library.hpp
        include/lucent/application/catalog_library.hpp
        src/source_item.cpp
        src/camera_support.cpp
        src/library.cpp
        src/catalog_library.cpp
        src/catalog_mapping.cpp
        src/catalog_mapping.hpp
        src/observer_list.hpp

        # Import: discovery, exclusions, loop protection, probing, copies into a library folder.
        include/lucent/application/import.hpp
        src/import.cpp
        src/win32_paths.cpp
        src/win32_paths.hpp

        # Source maintenance (W4): hashing, duplicates, presence checks, relinking.
        include/lucent/application/source_maintenance.hpp
        include/lucent/application/maintenance_service.hpp
        src/source_maintenance.cpp
        src/maintenance_service.cpp

        # Three-lane task executor and memory admission.
        include/lucent/application/task_executor.hpp
        src/task_executor.cpp

        # Rendering seam, display conversion, previews and their caches; the lens database loaded off
        # the UI thread (W12, APL-06).
        include/lucent/application/lens_database_source.hpp
        include/lucent/application/source_renderer.hpp
        include/lucent/application/display_image.hpp
        include/lucent/application/preview.hpp
        include/lucent/application/preview_cache.hpp
        include/lucent/application/preview_service.hpp
        include/lucent/application/thumbnail_cache.hpp
        src/lens_database_source.cpp
        src/source_renderer.cpp
        src/display_image.cpp
        src/preview.cpp
        src/preview_service.cpp
        src/thumbnail_cache.cpp

        # Single-item export.
        include/lucent/application/export_service.hpp
        src/export_service.cpp

        # Recipes (W6): canonical documents, revision stores, the recipe service and edit sessions.
        include/lucent/application/recipe.hpp
        include/lucent/application/recipe_store.hpp
        include/lucent/application/recipe_service.hpp
        include/lucent/application/develop_preview.hpp
        src/recipe.cpp
        src/recipe_store.cpp
        src/recipe_service.cpp
        src/develop_preview.cpp

        # Durable batch export (W7, ADR 0012): destinations and names, the batch service (journal,
        # scheduler over lucent-worker processes, publication, recovery) and its reports.
        include/lucent/application/batch_destination.hpp
        include/lucent/application/batch_service.hpp
        src/batch_destination.cpp
        src/batch_documents.hpp
        src/batch_documents.cpp
        src/batch_service_impl.hpp
        src/batch_service.cpp
        src/batch_scheduler.cpp
        src/batch_report.cpp
        src/output_publication.hpp
        src/output_publication.cpp

        # Automatic settings (W8 application wiring, ADR 0013): the shared run (analysis, related-shot
        # regularisation, full-resolution final check and fallbacks), its documents and records, and
        # the editor's interactive "Auto".
        include/lucent/application/auto_develop.hpp
        include/lucent/application/auto_service.hpp
        src/auto_develop.cpp
        src/auto_documents.cpp
        src/auto_service.cpp

        # Updates (W12, ADR 0022): the channel compiled into the build, the signed release manifest,
        # https downloads (WinHTTP) and the update service (check, verified download, installation).
        include/lucent/application/http_client.hpp
        include/lucent/application/update_channel.hpp
        include/lucent/application/update_manifest.hpp
        include/lucent/application/update_service.hpp
        src/http_client.cpp
        src/update_channel.cpp
        src/update_channel_config.hpp
        src/update_channel_config.cpp.in
        src/update_manifest.cpp
        src/update_service.cpp
    PUBLIC_DEPS
        Lucent::core
        Lucent::image
        Lucent::raw
        Lucent::engine
        Lucent::export
        Lucent::catalog
        Lucent::analysis
        Lucent::assets
        Lucent::workers
    PRIVATE_DEPS
        nlohmann_json::nlohmann_json
        winhttp   # the update channel's downloads
        advapi32  # the update folder's owner and permissions (SetSecurityInfo)
)

# ---------------------------------------------------------------------------------------------
# The update channel (W12, ADR 0022), compiled into the build. All empty (the default): updates are
# not configured and Lucent never opens a network connection. Set them (or the environment variables
# of the same names before the first configure) for a release build that checks a channel:
#   LUCENT_UPDATE_CHANNEL                channel name, [a-z0-9] words joined by '-' ("stable")
#   LUCENT_UPDATE_MANIFEST_URL           https URL of the signed release manifest (signature: <URL>.sig)
#   LUCENT_UPDATE_PUBLIC_KEYS            ';'-separated ECDSA P-256 public keys (130 hex digits, "04...")
#   LUCENT_UPDATE_PUBLISHER_THUMBPRINTS  optional ';'-separated SHA-1 or SHA-256 thumbprints of the
#                                        certificates the installer must be Authenticode-signed with
# ---------------------------------------------------------------------------------------------
set(LUCENT_UPDATE_CHANNEL "$ENV{LUCENT_UPDATE_CHANNEL}" CACHE STRING
    "Update channel name; empty: updates are not configured (ADR 0022)")
set(LUCENT_UPDATE_MANIFEST_URL "$ENV{LUCENT_UPDATE_MANIFEST_URL}" CACHE STRING
    "https URL of the update channel's signed release manifest (ADR 0022)")
set(LUCENT_UPDATE_PUBLIC_KEYS "$ENV{LUCENT_UPDATE_PUBLIC_KEYS}" CACHE STRING
    "';'-separated ECDSA P-256 public keys that sign release manifests (ADR 0022)")
set(LUCENT_UPDATE_PUBLISHER_THUMBPRINTS "$ENV{LUCENT_UPDATE_PUBLISHER_THUMBPRINTS}" CACHE STRING
    "Optional ';'-separated Authenticode signer thumbprints the installer must carry (ADR 0022)")

if(LUCENT_UPDATE_CHANNEL OR LUCENT_UPDATE_MANIFEST_URL OR LUCENT_UPDATE_PUBLIC_KEYS OR
   LUCENT_UPDATE_PUBLISHER_THUMBPRINTS)
    if(NOT LUCENT_UPDATE_CHANNEL OR NOT LUCENT_UPDATE_MANIFEST_URL OR NOT LUCENT_UPDATE_PUBLIC_KEYS)
        message(FATAL_ERROR "LUCENT_UPDATE_CHANNEL, LUCENT_UPDATE_MANIFEST_URL and LUCENT_UPDATE_PUBLIC_KEYS "
                            "must be set together (ADR 0022)")
    endif()
    string(LENGTH "${LUCENT_UPDATE_CHANNEL}" _lucent_channel_length)
    if(NOT LUCENT_UPDATE_CHANNEL MATCHES "^[a-z0-9]+(-[a-z0-9]+)*$" OR _lucent_channel_length GREATER 32)
        message(FATAL_ERROR "LUCENT_UPDATE_CHANNEL '${LUCENT_UPDATE_CHANNEL}' is not a channel name")
    endif()
    if(NOT LUCENT_UPDATE_MANIFEST_URL MATCHES "^https://[A-Za-z0-9._~:/?!$&()*+,=%-]+$" OR
       LUCENT_UPDATE_MANIFEST_URL MATCHES "^https://[^/]*@")
        message(FATAL_ERROR "LUCENT_UPDATE_MANIFEST_URL must be an https URL without user information, "
                            "spaces or quotes")
    endif()
    foreach(_lucent_key IN LISTS LUCENT_UPDATE_PUBLIC_KEYS)
        string(STRIP "${_lucent_key}" _lucent_key)
        string(LENGTH "${_lucent_key}" _lucent_key_length)
        if(_lucent_key AND (NOT _lucent_key MATCHES "^04[0-9A-Fa-f]+$" OR NOT _lucent_key_length EQUAL 130))
            message(FATAL_ERROR "LUCENT_UPDATE_PUBLIC_KEYS: '${_lucent_key}' is not an uncompressed P-256 point "
                                "(130 hex digits starting with 04)")
        endif()
    endforeach()
    foreach(_lucent_thumbprint IN LISTS LUCENT_UPDATE_PUBLISHER_THUMBPRINTS)
        string(STRIP "${_lucent_thumbprint}" _lucent_thumbprint)
        string(LENGTH "${_lucent_thumbprint}" _lucent_thumbprint_length)
        if(_lucent_thumbprint AND (NOT _lucent_thumbprint MATCHES "^[0-9A-Fa-f]+$" OR
                                   NOT (_lucent_thumbprint_length EQUAL 40 OR _lucent_thumbprint_length EQUAL 64)))
            message(FATAL_ERROR "LUCENT_UPDATE_PUBLISHER_THUMBPRINTS: '${_lucent_thumbprint}' is not a SHA-1 or "
                                "SHA-256 thumbprint")
        endif()
    endforeach()
    # Shown in full at every configure (W12 review SEC-02): whoever holds one of these keys, or (with
    # no publisher thumbprint) signs any installer, controls the updates of everyone this build reaches.
    message(STATUS "Update channel '${LUCENT_UPDATE_CHANNEL}': ${LUCENT_UPDATE_MANIFEST_URL}")
    foreach(_lucent_key IN LISTS LUCENT_UPDATE_PUBLIC_KEYS)
        string(STRIP "${_lucent_key}" _lucent_key)
        if(_lucent_key)
            message(STATUS "  pinned release-manifest key: ${_lucent_key}")
        endif()
    endforeach()
    if(LUCENT_UPDATE_PUBLISHER_THUMBPRINTS)
        message(STATUS "  installer publishers (Authenticode): ${LUCENT_UPDATE_PUBLISHER_THUMBPRINTS}")
    else()
        message(STATUS "  installer publishers: none named, so the installer's Authenticode signature is not "
                       "checked (a release channel names them; installer/update-channels.json)")
    endif()
else()
    message(STATUS "Update channel: not configured (Lucent never opens a network connection)")
endif()
configure_file(src/update_channel_config.cpp.in
               "${CMAKE_CURRENT_BINARY_DIR}/generated/update_channel_config.cpp" @ONLY)
target_sources(lucent_application PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/update_channel_config.cpp")

# ---------------------------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------------------------
# The tests build small DNGs with the raw module's test writers (src/raw/tests) and see src/ for
# the private path helpers and the catalog mapping.

set(LUCENT_APPLICATION_TEST_SOURCES
    tests/app_test_support.hpp
    tests/source_item_test.cpp
    tests/camera_support_test.cpp
    tests/library_test.cpp
    tests/catalog_library_test.cpp
    tests/catalog_mapping_test.cpp
    tests/win32_paths_test.cpp
    tests/import_test.cpp
    tests/import_copy_test.cpp
    tests/source_maintenance_test.cpp
    tests/maintenance_service_test.cpp
    tests/task_executor_test.cpp
    tests/display_image_test.cpp
    tests/preview_cache_test.cpp
    tests/preview_service_test.cpp
    tests/recipe_test.cpp
    tests/recipe_service_test.cpp
    tests/develop_preview_test.cpp
    tests/lens_database_source_test.cpp
    tests/thumbnail_cache_test.cpp
    tests/export_service_test.cpp
    tests/auto_service_test.cpp
    tests/update_manifest_test.cpp
    tests/update_compatibility_test.cpp
)

lucent_add_test(lucent_application_tests
    SOURCES
        ${LUCENT_APPLICATION_TEST_SOURCES}
    DEPS
        Lucent::application
        nlohmann_json::nlohmann_json  # recipe documents are built and mutated in tests
    LABELS
        application unit
)
if(TARGET lucent_application_tests)
    target_include_directories(lucent_application_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_SOURCE_DIR}/src/raw/tests")
endif()

# Durable batches (W7, ADR 0012) with real lucent-worker processes over synthetic DNGs, and the
# helper coordinator process the recovery tests start and kill between publication steps.
if(LUCENT_BUILD_TESTS)
    add_executable(lucent_batch_test_coordinator tests/batch_coordinator_main.cpp)
    target_link_libraries(lucent_batch_test_coordinator PRIVATE Lucent::compiler_options Lucent::application)
    target_compile_definitions(lucent_batch_test_coordinator PRIVATE
        LUCENT_WORKER_EXE="$<TARGET_FILE:lucent-worker>")
    set_target_properties(lucent_batch_test_coordinator PROPERTIES FOLDER "tests")

    lucent_add_test(lucent_batch_tests
        SOURCES
            tests/batch_test_support.hpp
            tests/batch_destination_test.cpp
            tests/output_publication_test.cpp
            tests/batch_service_test.cpp
            tests/batch_recovery_test.cpp
            tests/batch_auto_test.cpp
            tests/batch_update_test.cpp
        DEPS
            Lucent::application
            nlohmann_json::nlohmann_json
        LABELS
            application workers integration
    )
    target_include_directories(lucent_batch_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_SOURCE_DIR}/src/raw/tests")
    target_compile_definitions(lucent_batch_tests PRIVATE
        LUCENT_WORKER_EXE="$<TARGET_FILE:lucent-worker>"
        LUCENT_BATCH_COORDINATOR_EXE="$<TARGET_FILE:lucent_batch_test_coordinator>")
    add_dependencies(lucent_batch_tests lucent-worker lucent_batch_test_coordinator)

    # The smoke fixtures through real workers: full-resolution renders, so they hold the render lock.
    add_executable(lucent_batch_fixture_tests
        tests/batch_test_support.hpp
        tests/batch_fixture_test.cpp
    )
    target_link_libraries(lucent_batch_fixture_tests PRIVATE
        Lucent::compiler_options
        GTest::gtest
        GTest::gtest_main
        Lucent::application
    )
    target_compile_definitions(lucent_batch_fixture_tests PRIVATE
        LUCENT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        LUCENT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures"
        LUCENT_WORKER_EXE="$<TARGET_FILE:lucent-worker>")
    target_include_directories(lucent_batch_fixture_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_SOURCE_DIR}/src/raw/tests")
    set_target_properties(lucent_batch_fixture_tests PROPERTIES FOLDER "tests")
    add_dependencies(lucent_batch_fixture_tests lucent-worker)
    gtest_discover_tests(lucent_batch_fixture_tests
        DISCOVERY_MODE PRE_TEST
        PROPERTIES
            LABELS "application;workers;fixtures"
            RESOURCE_LOCK lucent_full_resolution_render)
endif()

# Updates (W12, ADR 0022): the WinHTTP client and the update service against a loopback HTTP server
# (never another machine), with a stand-in installer program the service downloads, verifies and
# starts.
if(LUCENT_BUILD_TESTS)
    add_executable(lucent_update_test_installer tests/update_test_installer_main.cpp)
    target_link_libraries(lucent_update_test_installer PRIVATE Lucent::compiler_options)
    set_target_properties(lucent_update_test_installer PROPERTIES FOLDER "tests")

    lucent_add_test(lucent_update_tests
        SOURCES
            tests/loopback_http_server.cpp
            tests/loopback_http_server.hpp
            tests/http_client_test.cpp
            tests/update_service_test.cpp
        DEPS
            Lucent::application
            ws2_32  # the loopback server
        LABELS
            application update
    )
    target_include_directories(lucent_update_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_SOURCE_DIR}/src/raw/tests")
    target_compile_definitions(lucent_update_tests PRIVATE
        LUCENT_UPDATE_TEST_INSTALLER_EXE="$<TARGET_FILE:lucent_update_test_installer>")
    add_dependencies(lucent_update_tests lucent_update_test_installer)
endif()

# Camera-fixture tests (skipped when fixtures/cache is absent): import of the smoke fixtures,
# preview latency on the 45 MP Canon EOS R5 file, export, original preservation. Created directly
# (not with lucent_add_test) to hold the RESOURCE_LOCK that keeps ctest from running two
# full-resolution renders at once (see tests/CMakeLists.txt).
if(LUCENT_BUILD_TESTS)
    add_executable(lucent_application_fixture_tests
        tests/app_test_support.hpp
        tests/application_fixture_test.cpp
        tests/auto_large_photo_test.cpp
    )
    target_link_libraries(lucent_application_fixture_tests PRIVATE
        Lucent::compiler_options
        GTest::gtest
        GTest::gtest_main
        Lucent::application
    )
    target_compile_definitions(lucent_application_fixture_tests PRIVATE
        LUCENT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        LUCENT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures")
    target_include_directories(lucent_application_fixture_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_SOURCE_DIR}/src/raw/tests")
    set_target_properties(lucent_application_fixture_tests PROPERTIES FOLDER "tests")
    gtest_discover_tests(lucent_application_fixture_tests
        DISCOVERY_MODE PRE_TEST
        PROPERTIES
            LABELS "application;fixtures"
            RESOURCE_LOCK lucent_full_resolution_render)
endif()
