# lucent_analysis: metric contract, proposals, bounded candidate search and confidence for Natural v1 automatic settings (W8). Pure C++.
#
# Blueprint section 6 ("Automatic settings for every photograph") and ADR 0011. The module consumes
# the decoded-RAW data contract (lucent_raw) and the develop pipeline (lucent_engine): one decode,
# an interactive stage one, a 1536-pixel analysis image and a 512-pixel search proxy, candidates
# rendered by stage two and judged by the metric contract, and deterministic native-resolution crops.
#
# Layout
#   include/lucent/analysis/   public headers
#   src/                       implementation and private headers
#   tests/                     GoogleTest unit tests (synthetic scenes) and the smoke-fixture sweep

set(LUCENT_ANALYSIS_SOURCES
    src/parallel_bands.hpp
    src/stage_one_copy.hpp

    # The metric contract: every metric's domain, mask, units, baseline, thresholds, valid-sample
    # minimum and missing-data behaviour.
    include/lucent/analysis/metric_contract.hpp
    src/metric_contract.cpp

    # Source diagnostics from the decoded RAW before stage one: per-channel saturation, the
    # saturation coverage of the analysis grids and the sensor noise model.
    include/lucent/analysis/source_diagnostics.hpp
    src/source_diagnostics.cpp

    # Measurements of rendered candidates against the baseline in the declared output encoding,
    # and of full-resolution renders band by band (the application's final check), sharing one
    # per-pixel kernel.
    include/lucent/analysis/render_metrics.hpp
    src/render_kernel.hpp
    src/render_metrics.cpp
    include/lucent/analysis/frame_measurement.hpp
    src/frame_measurement.cpp

    # Exposure/headroom and white-balance proposals with their evidence.
    include/lucent/analysis/proposals.hpp
    src/proposals.cpp

    # Natural v1: hard checks and the normalised heuristic objective.
    include/lucent/analysis/objective.hpp
    src/objective.cpp

    # Deterministic native-resolution crops (detail, shadows, highlights, edges) and their metrics.
    include/lucent/analysis/native_crops.hpp
    src/native_crops.cpp

    # Related-shot consistency: appearance, shot features, targets and the soft consistency term.
    include/lucent/analysis/consistency.hpp
    src/consistency.cpp

    # The automatic-settings controller: proposals, bounded search, recheck, confidence, and the
    # session that regularises a related-shot member towards its group's target.
    include/lucent/analysis/auto_settings.hpp
    src/auto_settings.cpp

    # Related-shot grouping (capture time, visual similarity, illumination splits, caller
    # overrides), group targets and the two-pass flow over files.
    include/lucent/analysis/related_shots.hpp
    src/related_shots.cpp
)

lucent_add_library(analysis
    SOURCES
        ${LUCENT_ANALYSIS_SOURCES}
    PUBLIC_DEPS
        Lucent::core
        Lucent::image
        Lucent::raw
        Lucent::engine
)

# ---------------------------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------------------------
# The synthetic scenes are built with the engine's test helpers (sensor_test_support.hpp,
# develop_test_support.hpp), included read-only from src/engine/tests.

set(LUCENT_ANALYSIS_TEST_SOURCES
    tests/analysis_test_support.hpp
    tests/metric_contract_test.cpp
    tests/source_diagnostics_test.cpp
    tests/render_metrics_test.cpp
    tests/frame_measurement_test.cpp
    tests/proposals_test.cpp
    tests/auto_settings_test.cpp
    tests/native_crops_test.cpp
    tests/related_shots_test.cpp
)

lucent_add_test(lucent_analysis_tests
    SOURCES
        ${LUCENT_ANALYSIS_TEST_SOURCES}
    DEPS
        Lucent::analysis
    LABELS
        analysis unit
)
if(TARGET lucent_analysis_tests)
    target_include_directories(lucent_analysis_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/engine/tests")
endif()

# The smoke-tier fixture sweep (skips when fixtures/cache is absent; tools\fetch-fixtures.ps1).
lucent_add_test(lucent_analysis_fixture_tests
    SOURCES
        tests/analysis_test_support.hpp
        tests/auto_fixture_test.cpp
        tests/related_fixture_test.cpp
    DEPS
        Lucent::analysis
        Lucent::raw
    LABELS
        analysis fixtures
)
if(TARGET lucent_analysis_fixture_tests)
    target_include_directories(lucent_analysis_fixture_tests PRIVATE "${PROJECT_SOURCE_DIR}/src/engine/tests")
endif()
