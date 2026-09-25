# lucent_image: pixel containers, CFA descriptors, colour-encoding contracts and colour math.
# Pure C++ on top of lucent_core's error model; no third-party dependencies
# (docs/architecture/overview.md).

lucent_add_library(image
    SOURCES
        include/lucent/image/cfa.hpp
        include/lucent/image/color_encoding.hpp
        include/lucent/image/color_math.hpp
        include/lucent/image/geometry.hpp
        include/lucent/image/pixel_buffer.hpp
        src/cfa.cpp
        src/color_encoding.cpp
        src/color_math.cpp
        src/geometry.cpp
        src/pixel_buffer.cpp
    PUBLIC_DEPS
        Lucent::core
)

lucent_add_test(lucent_image_tests
    SOURCES
        tests/cfa_tests.cpp
        tests/color_encoding_tests.cpp
        tests/color_math_tests.cpp
        tests/geometry_tests.cpp
        tests/pixel_buffer_tests.cpp
    DEPS
        Lucent::image
    LABELS
        unit
        image
)
