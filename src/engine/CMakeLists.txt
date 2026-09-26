# lucent_engine: sensor normalisation, demosaicing routes, camera colour and the render pipeline
# (blueprint section 5). It consumes the decoded-RAW data contract of lucent_raw, never a decoder
# adapter (docs/architecture/overview.md).
#
# Layout
#   include/lucent/engine/   public headers
#   src/                     implementation and private headers
#   tests/                   GoogleTest unit tests (synthetic data) and reference/fixture tests

set(LUCENT_ENGINE_SOURCES
    # Shared private helpers.
    src/parallel_rows.hpp
    src/sensor_internal.hpp

    # Sensor normalisation and pre-demosaic white balance (blueprint section 5, step 2).
    include/lucent/engine/sensor.hpp
    src/sensor.cpp
    src/sensor_white_balance.cpp

    # Demosaicing routes: Bayer Malvar-He-Cutler, X-Trans baseline, bypasses (section 5, step 3).
    include/lucent/engine/demosaic.hpp
    src/demosaic.cpp
    src/demosaic_bayer.cpp
    src/demosaic_xtrans.cpp

    # Camera colour: calibration paths, camera -> XYZ -> working space (section 5, step 5).
    src/pixel_ops.hpp
    include/lucent/engine/camera_color.hpp
    src/camera_color.cpp
    src/dng_color.cpp

    # Display rendering (Neutral v0, provisional), orientation and the render pipeline (ADR 0006).
    include/lucent/engine/display_rendering.hpp
    src/display_rendering.cpp
    include/lucent/engine/orientation.hpp
    src/orientation.cpp
    include/lucent/engine/render.hpp
    src/render.cpp
)

lucent_add_library(engine
    SOURCES
        ${LUCENT_ENGINE_SOURCES}
    PUBLIC_DEPS
        Lucent::core
        Lucent::image
        Lucent::raw
)

# ---------------------------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------------------------
# lucent_add_test() is a no-op when LUCENT_BUILD_TESTS is OFF.

# Synthetic, deterministic unit tests (no files needed).
set(LUCENT_ENGINE_TEST_SOURCES
    tests/sensor_test_support.hpp
    tests/sensor_test.cpp
    tests/sensor_white_balance_test.cpp
    tests/demosaic_test.cpp
    tests/camera_color_test.cpp
    tests/display_rendering_test.cpp
    tests/orientation_test.cpp
    tests/render_test.cpp
)

lucent_add_test(lucent_engine_tests
    SOURCES
        ${LUCENT_ENGINE_TEST_SOURCES}
    DEPS
        Lucent::engine
    LABELS
        engine unit
)

# Reference parity and camera-fixture tests. The Malvar-He-Cutler parity cases read the committed
# references in fixtures/reference/demosaic (research/tools/demosaic_reference.py) and always run;
# the cases that decode camera files skip when fixtures/cache is absent (tools\fetch-fixtures.ps1).
if(LUCENT_BUILD_TESTS)
    find_package(nlohmann_json CONFIG REQUIRED)
endif()

set(LUCENT_ENGINE_FIXTURE_TEST_SOURCES
    tests/sensor_test_support.hpp
    tests/demosaic_reference_test.cpp
    tests/demosaic_fixture_test.cpp
)

lucent_add_test(lucent_engine_fixture_tests
    SOURCES
        ${LUCENT_ENGINE_FIXTURE_TEST_SOURCES}
    DEPS
        Lucent::engine
        Lucent::raw
        nlohmann_json::nlohmann_json
    LABELS
        engine fixtures
)
