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

    # DNG opcode lists 1-3 at their DNG stages (ADR 0014).
    include/lucent/engine/dng_opcode_processing.hpp
    src/dng_opcode_processing.cpp
    src/opcode_stages.hpp

    # Display rendering (Neutral v0, provisional), orientation and the render pipeline (ADR 0006).
    include/lucent/engine/display_rendering.hpp
    src/display_rendering.cpp
    include/lucent/engine/orientation.hpp
    src/orientation.cpp
    src/render_warnings.hpp
    include/lucent/engine/render.hpp
    src/render.cpp

    # Develop pipeline (W6): DevelopSettings v1, white-balance model, guided filter, develop
    # operations, interactive stage one / stage two and the export render.
    src/robertson_table.hpp
    src/scratch_pool.hpp
    src/octave_lut.hpp
    include/lucent/engine/develop_settings.hpp
    src/develop_settings.cpp
    include/lucent/engine/white_balance_model.hpp
    src/white_balance_model.cpp
    include/lucent/engine/guided_filter.hpp
    src/guided_filter.cpp
    include/lucent/engine/develop_operations.hpp
    src/tone_curves.cpp
    src/develop_operations.cpp
    include/lucent/engine/develop.hpp
    src/develop.cpp

    # DevelopSettings v2 (W9 part 2, ADR 0015): guided chroma denoising, masks and local
    # adjustments, geometry (straighten, crop, quarter turns) and the mask/output point mapping.
    include/lucent/engine/chroma_denoise.hpp
    src/chroma_denoise.cpp
    include/lucent/engine/local_adjustments.hpp
    src/local_adjustments.cpp
    include/lucent/engine/develop_geometry.hpp
    src/develop_geometry.cpp

    # The camera's image area (W10, ADR 0017): placement, geometry version 2, the W2 camera_area
    # stage.
    include/lucent/engine/image_area.hpp
    src/image_area.cpp
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
    # Develop pipeline (W6).
    tests/develop_test_support.hpp
    tests/develop_settings_test.cpp
    tests/white_balance_model_test.cpp
    tests/guided_filter_test.cpp
    tests/tone_curves_test.cpp
    tests/develop_operations_test.cpp
    tests/develop_test.cpp
    # DNG opcode processing (ADR 0014).
    tests/dng_opcode_test_support.hpp
    tests/dng_opcode_processing_test.cpp
    # DevelopSettings v2 (ADR 0015).
    tests/develop_geometry_test.cpp
    tests/local_adjustments_test.cpp
    tests/chroma_denoise_test.cpp
    tests/develop_v2_test.cpp
    # Geometry versions and the camera's image area (ADR 0017).
    tests/geometry_version_test.cpp
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

# The guided-filter parity cases read the committed OpenCV references in
# src/engine/tests/data/guided_filter (tests/tools/guided_filter_reference.py) and always run;
# the develop fixture cases skip when fixtures/cache is absent.
set(LUCENT_ENGINE_FIXTURE_TEST_SOURCES
    tests/sensor_test_support.hpp
    tests/develop_test_support.hpp
    tests/demosaic_reference_test.cpp
    tests/demosaic_fixture_test.cpp
    tests/guided_filter_reference_test.cpp
    tests/develop_fixture_test.cpp
    tests/dng_opcode_fixture_test.cpp
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
