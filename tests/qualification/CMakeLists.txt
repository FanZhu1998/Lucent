# Camera qualification tests (W10, ADR 0016). Label "qualification".
#
# lucent_qualification_tests: synthetic unit tests of the mode key, the expectations schema and its
# evidence rule, the state rules and the report writers, plus a check that the committed
# expectations and families files are valid and cover fixtures/manifest.json exactly. No fixtures
# needed.
#
# lucent_qualification_fixture_tests: the GTest wrapper of the qualification harness. For every
# cached fixture it runs lucent::qualify::qualify_fixture() (decode, rawpy parity, W2 render
# against the committed rawpy statistics, two develop renders, JPEG/TIFF export and read-back) and
# asserts the outcome recorded in fixtures/qualification/expectations.json: the proposed state,
# every check's status, the limitations, the mode key and where the camera's area is placed. A
# regression in camera handling therefore fails CI. Fixtures that are not downloaded skip; smoke
# fixtures and every fixture expected to be Verified always run when cached (QUA-07), the other
# qualification-tier fixtures only with LUCENT_TEST_FIXTURE_TIER=all (about 20 s each). The
# tests hold the full-resolution render lock (tests/CMakeLists.txt) so that at most one
# full-resolution render of the fixture suites runs at a time.
#
# Run just these with:  ctest -L qualification

if(NOT TARGET lucent_qualify)
    message(STATUS "qualification tests skipped: they need lucent-qualify's library (LUCENT_BUILD_TOOLS=ON)")
    return()
endif()

lucent_add_test(lucent_qualification_tests
    SOURCES
        qualification_unit_tests.cpp
    DEPS
        lucent_qualify
        Lucent::raw
    LABELS
        qualification
)

add_executable(lucent_qualification_fixture_tests qualification_fixture_tests.cpp)
target_link_libraries(lucent_qualification_fixture_tests PRIVATE
    Lucent::compiler_options
    GTest::gtest
    GTest::gtest_main
    lucent_qualify
)
target_compile_definitions(lucent_qualification_fixture_tests PRIVATE
    LUCENT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    LUCENT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures")
set_target_properties(lucent_qualification_fixture_tests PROPERTIES FOLDER "tests")
gtest_discover_tests(lucent_qualification_fixture_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES
        LABELS "qualification;fixtures"
        RESOURCE_LOCK lucent_full_resolution_render)
