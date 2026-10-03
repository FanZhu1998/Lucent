# lucent_application: import, the session and catalog-backed libraries, source maintenance
# (hashing, presence checks, relinking), embedded previews, background preview rendering, the
# on-disk thumbnail cache and single-image export services (W3, W4). Pure C++, no Qt: the user
# interface (apps/lucent) drives these services and marshals their callbacks to its own thread.
# See docs/architecture/overview.md and docs/adr/0007, 0008.
#
# Layout
#   include/lucent/application/   public headers
#   src/                          implementation and private headers (Win32 path helpers, catalog mapping)
#   tests/                        GoogleTest unit tests (synthetic DNGs) and fixture tests

find_package(nlohmann_json CONFIG REQUIRED)

lucent_add_library(application
    SOURCES
        # Library items, the session library and the catalog-backed library (W4).
        include/lucent/application/source_item.hpp
        include/lucent/application/library.hpp
        include/lucent/application/catalog_library.hpp
        src/source_item.cpp
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

        # Rendering seam, display conversion, previews and their caches.
        include/lucent/application/source_renderer.hpp
        include/lucent/application/display_image.hpp
        include/lucent/application/preview.hpp
        include/lucent/application/preview_cache.hpp
        include/lucent/application/preview_service.hpp
        include/lucent/application/thumbnail_cache.hpp
        src/source_renderer.cpp
        src/display_image.cpp
        src/preview.cpp
        src/preview_service.cpp
        src/thumbnail_cache.cpp

        # Single-item export.
        include/lucent/application/export_service.hpp
        src/export_service.cpp
    PUBLIC_DEPS
        Lucent::core
        Lucent::image
        Lucent::raw
        Lucent::engine
        Lucent::export
        Lucent::catalog
    PRIVATE_DEPS
        nlohmann_json::nlohmann_json
)

# ---------------------------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------------------------
# The tests build small DNGs with the raw module's test writers (src/raw/tests) and see src/ for
# the private path helpers and the catalog mapping.

set(LUCENT_APPLICATION_TEST_SOURCES
    tests/app_test_support.hpp
    tests/source_item_test.cpp
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
    tests/thumbnail_cache_test.cpp
    tests/export_service_test.cpp
)

lucent_add_test(lucent_application_tests
    SOURCES
        ${LUCENT_APPLICATION_TEST_SOURCES}
    DEPS
        Lucent::application
    LABELS
        application unit
)
if(TARGET lucent_application_tests)
    target_include_directories(lucent_application_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_SOURCE_DIR}/src/raw/tests")
endif()

# Camera-fixture tests (skipped when fixtures/cache is absent): import of the smoke fixtures,
# preview latency on the 45 MP Canon EOS R5 file, export, original preservation. Created directly
# (not with lucent_add_test) to hold the RESOURCE_LOCK that keeps ctest from running two
# full-resolution renders at once (see tests/CMakeLists.txt).
if(LUCENT_BUILD_TESTS)
    add_executable(lucent_application_fixture_tests
        tests/app_test_support.hpp
        tests/application_fixture_test.cpp
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
