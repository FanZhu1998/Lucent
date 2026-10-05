# lucent-qualify: developer CLI that runs the camera qualification harness over the fixture corpus
# and writes the versioned qualification report and the support table (W10, ADR 0016). See
# main.cpp for usage.
#
# The check suite, mode keys, expectations, state rules and report writers live in the static
# library lucent_qualify so that the GTest wrapper (tests/qualification) asserts exactly what the
# tool computes. It reuses, rather than re-implements:
#   * lucent-probe's parity block and raw-buffer hash (lucent_probe_report),
#   * the worker's export-metadata mapping (lucent_worker_job), i.e. the product's export path,
#   * the CIEDE2000 / CIELAB helpers of the render integration tests
#     (tests/integration/color_difference.cpp), so the colour gates are the same as ADR 0006's.

find_package(nlohmann_json CONFIG REQUIRED)
find_package(JPEG REQUIRED)
find_package(TIFF REQUIRED)

add_library(lucent_qualify STATIC
    expectations.cpp
    expectations.hpp
    mode_key.cpp
    mode_key.hpp
    qualification.cpp
    qualification.hpp
    readback.cpp
    readback.hpp
    report.cpp
    report.hpp
    "${CMAKE_SOURCE_DIR}/tests/integration/color_difference.cpp"
    "${CMAKE_SOURCE_DIR}/tests/integration/color_difference.hpp"
)
target_include_directories(lucent_qualify
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}"
    PRIVATE "${CMAKE_SOURCE_DIR}/tests/integration")
target_link_libraries(lucent_qualify
    PUBLIC
        Lucent::core
        Lucent::raw
        Lucent::engine
        Lucent::export
        nlohmann_json::nlohmann_json
    PRIVATE
        Lucent::compiler_options
        lucent_probe_report
        lucent_worker_job
        JPEG::JPEG
        TIFF::TIFF
)
set_target_properties(lucent_qualify PROPERTIES FOLDER "apps")

lucent_add_executable(lucent-qualify
    SOURCES
        main.cpp
    DEPS
        lucent_qualify
)
# The default --root: the source tree the tool was built from.
target_compile_definitions(lucent-qualify PRIVATE LUCENT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
