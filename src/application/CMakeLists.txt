# lucent_application: import, embedded previews, background preview rendering and single-image
# export services (W3). Pure C++, no Qt: the user interface (apps/lucent) drives these services
# and marshals their callbacks to its own thread. See docs/architecture/overview.md.
#
# Layout
#   include/lucent/application/   public headers
#   src/                          implementation and private headers (Win32 path helpers)
#   tests/                        GoogleTest unit tests (synthetic DNGs) and fixture tests

lucent_add_library(application
    SOURCES
        # Library items and the session library (W4 adds a catalog-backed Library).
        include/lucent/application/source_item.hpp
        include/lucent/application/library.hpp
        src/source_item.cpp
        src/library.cpp
        src/observer_list.hpp

        # Import: discovery, exclusions, loop protection, probing.
        include/lucent/application/import.hpp
        src/import.cpp
        src/win32_paths.cpp
        src/win32_paths.hpp

        # Two-lane task executor and memory admission.
        include/lucent/application/task_executor.hpp
        src/task_executor.cpp

        # Rendering seam, display conversion, previews and their cache.
        include/lucent/application/source_renderer.hpp
        include/lucent/application/display_image.hpp
        include/lucent/application/preview.hpp
        include/lucent/application/preview_cache.hpp
        include/lucent/application/preview_service.hpp
        src/source_renderer.cpp
        src/display_image.cpp
        src/preview.cpp
        src/preview_service.cpp

        # Single-item export.
        include/lucent/application/export_service.hpp
        src/export_service.cpp
    PUBLIC_DEPS
        Lucent::core
        Lucent::image
        Lucent::raw
        Lucent::engine
        Lucent::export
)

# ---------------------------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------------------------
# The tests build small DNGs with the raw module's test writers (src/raw/tests) and see src/ for
# the private path helpers.

set(LUCENT_APPLICATION_TEST_SOURCES
    tests/app_test_support.hpp
    tests/source_item_test.cpp
    tests/library_test.cpp
    tests/win32_paths_test.cpp
    tests/import_test.cpp
    tests/task_executor_test.cpp
    tests/display_image_test.cpp
    tests/preview_cache_test.cpp
    tests/preview_service_test.cpp
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
# preview latency on the 45 MP Canon EOS R5 file, export. Created directly (not with
# lucent_add_test) to hold the RESOURCE_LOCK that keeps ctest from running two full-resolution
# renders at once (see tests/CMakeLists.txt).
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
