# Cross-module integration tests (fixture-based). Unit tests live beside each module.
#
# lucent_integration_tests decodes every cached camera fixture (fixtures/cache, downloaded by
# tools\fetch-fixtures.ps1) and compares it with the committed rawpy references
# (fixtures/reference). Cases whose fixture is not downloaded skip, so a fresh checkout passes.
#
# lucent_render_integration_tests renders fixtures end to end through the engine and the exporter,
# checks gross-error gates against committed rawpy statistics (integration/data/
# render_reference.json, research/tools/render_reference.py), exercises lucent-render.exe, and
# runs synthetic DNG files with opcodes through decoder and engine (ADR 0014; the drone fixtures'
# opcodes are compared with rawpy by tests/tools/dng_opcode_reference.py).
# Its tests hold the RESOURCE_LOCK lucent_full_resolution_render so that ctest never runs two of
# its full-resolution renders at once (about 1 GB each for a 45 MP file).
#
# Run just these with:  ctest -L fixtures
#
# qualification/ holds the camera qualification tests (W10, ADR 0016; label "qualification").

add_subdirectory(qualification)

if(NOT TARGET lucent_probe_report)
    message(STATUS "lucent_integration_tests skipped: it needs lucent-probe's report library (LUCENT_BUILD_TOOLS=ON)")
    return()
endif()

find_package(nlohmann_json CONFIG REQUIRED)

lucent_add_test(lucent_integration_tests
    SOURCES
        integration/fixture_manifest.cpp
        integration/fixture_manifest.hpp
        integration/raw_fixture_tests.cpp
    DEPS
        Lucent::raw
        lucent_probe_report
        nlohmann_json::nlohmann_json
    LABELS
        fixtures
        integration
)

if(NOT LUCENT_BUILD_TESTS OR NOT TARGET lucent_render_tool)
    return()
endif()

# libjpeg-turbo and libtiff decode the published files independently of the exporter's own
# verification (integration/image_readers.hpp).
find_package(JPEG REQUIRED)
find_package(TIFF REQUIRED)

# Created directly (not with lucent_add_test) to attach the RESOURCE_LOCK property.
add_executable(lucent_render_integration_tests
    integration/color_difference.cpp
    integration/color_difference.hpp
    integration/dng_opcode_synthetic_tests.cpp
    integration/fixture_manifest.cpp
    integration/fixture_manifest.hpp
    integration/image_readers.cpp
    integration/image_readers.hpp
    integration/render_fixture_tests.cpp
    integration/render_tool_tests.cpp
)
target_link_libraries(lucent_render_integration_tests PRIVATE
    Lucent::compiler_options
    GTest::gtest
    GTest::gtest_main
    Lucent::engine
    Lucent::export
    Lucent::raw
    lucent_render_tool
    nlohmann_json::nlohmann_json
    JPEG::JPEG
    TIFF::TIFF
)
target_compile_definitions(lucent_render_integration_tests PRIVATE
    LUCENT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    LUCENT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures")
# The synthetic DNG writer of the raw module's tests (integration/dng_opcode_synthetic_tests.cpp,
# ADR 0014).
target_include_directories(lucent_render_integration_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/raw/tests")
set_target_properties(lucent_render_integration_tests PROPERTIES FOLDER "tests")
# The CLI tests run bin/lucent-render.exe from the same output directory.
add_dependencies(lucent_render_integration_tests lucent-render)
gtest_discover_tests(lucent_render_integration_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES
        LABELS "fixtures;integration"
        RESOURCE_LOCK lucent_full_resolution_render)
