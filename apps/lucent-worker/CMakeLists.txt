# lucent-worker.exe: isolated processing worker (decode, render, encode, verify) driven by
# lucent_workers (W7). A GUI-subsystem program without a window: starting it never shows a console.
# Shipped beside Lucent.exe, it carries the same icon and version resources (Task Manager and
# Explorer show it as Lucent's; ADR 0025).
#
# The jobs themselves (DevelopJobRunner: recipe, source identity, develop render, export metadata,
# verified write into the host's temporary path; and the analyze job of automatic batches,
# analysis_job.hpp) live in the small static library
# lucent_worker_job, so the tests run exactly the code the worker runs, in-process as well as in a
# spawned worker.

add_library(lucent_worker_job STATIC
    analysis_job.cpp
    analysis_job.hpp
    develop_job.cpp
    develop_job.hpp
    export_metadata.cpp
    export_metadata.hpp
)
target_include_directories(lucent_worker_job PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(lucent_worker_job
    PUBLIC
        Lucent::workers
        Lucent::application
        Lucent::raw
        Lucent::export
    PRIVATE
        Lucent::compiler_options
)
set_target_properties(lucent_worker_job PROPERTIES FOLDER "apps")

lucent_add_executable(lucent-worker WIN32
    SOURCES
        main.cpp
    DEPS
        lucent_worker_job
        shell32  # CommandLineToArgvW
)
lucent_add_program_resources(lucent-worker DESCRIPTION "Lucent processing worker")

if(NOT LUCENT_BUILD_TESTS)
    return()
endif()

# ---------------------------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------------------------
# lucent_worker_tests spawn the real lucent-worker.exe (synthetic DNGs from the raw module's test
# writers) and also run DevelopJobRunner in-process. lucent_worker_test_parent is a helper host
# process that the parent-death tests kill. lucent_worker_fixture_tests render the camera fixtures
# (skipped when they are absent) and hold the full-resolution render lock (tests/CMakeLists.txt).

add_executable(lucent_worker_test_parent tests/test_parent_main.cpp)
target_link_libraries(lucent_worker_test_parent PRIVATE Lucent::compiler_options Lucent::workers)
set_target_properties(lucent_worker_test_parent PROPERTIES FOLDER "tests")

set(LUCENT_WORKER_TEST_DEFINITIONS
    LUCENT_WORKER_EXE="$<TARGET_FILE:lucent-worker>"
    LUCENT_WORKER_TEST_PARENT_EXE="$<TARGET_FILE:lucent_worker_test_parent>")

lucent_add_test(lucent_worker_tests
    SOURCES
        tests/worker_test_support.hpp
        tests/develop_job_test.cpp
        tests/worker_process_test.cpp
    DEPS
        lucent_worker_job
    LABELS
        workers integration
)
target_include_directories(lucent_worker_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/raw/tests")
target_compile_definitions(lucent_worker_tests PRIVATE ${LUCENT_WORKER_TEST_DEFINITIONS})
add_dependencies(lucent_worker_tests lucent-worker lucent_worker_test_parent)

add_executable(lucent_worker_fixture_tests
    tests/worker_test_support.hpp
    tests/worker_fixture_test.cpp
)
target_link_libraries(lucent_worker_fixture_tests PRIVATE
    Lucent::compiler_options
    GTest::gtest
    GTest::gtest_main
    lucent_worker_job
)
target_include_directories(lucent_worker_fixture_tests PRIVATE "${CMAKE_SOURCE_DIR}/src/raw/tests")
target_compile_definitions(lucent_worker_fixture_tests PRIVATE
    ${LUCENT_WORKER_TEST_DEFINITIONS}
    LUCENT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    LUCENT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures")
set_target_properties(lucent_worker_fixture_tests PROPERTIES FOLDER "tests")
add_dependencies(lucent_worker_fixture_tests lucent-worker)
gtest_discover_tests(lucent_worker_fixture_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES
        LABELS "workers;fixtures"
        RESOURCE_LOCK lucent_full_resolution_render)
