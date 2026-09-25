# Cross-module integration tests (fixture-based). Unit tests live beside each module.
#
# lucent_integration_tests decodes every cached camera fixture (fixtures/cache, downloaded by
# tools\fetch-fixtures.ps1) and compares it with the committed rawpy references
# (fixtures/reference). Cases whose fixture is not downloaded skip, so a fresh checkout passes.
# Run just these with:  ctest -L fixtures

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
