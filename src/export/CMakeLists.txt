# lucent_export: output profiles (Little CMS), JPEG/TIFF encoders, write verification and
# no-clobber publication (W2). Namespace lucent::exporter (`export` is a C++ keyword).
#
# Layout
#   include/lucent/export/   public headers (no third-party types)
#   src/                     implementation and private headers
#   tests/                   GoogleTest unit tests (synthetic data only)
#
# Little CMS, libjpeg-turbo (libjpeg API) and libtiff are PRIVATE dependencies: only files in
# src/ include their headers. All three are permissively licensed (docs/adr/0003); the GPL
# Little CMS plugins (fastfloat, threaded) are not used.

find_package(lcms2 CONFIG REQUIRED)
find_package(JPEG REQUIRED)
find_package(TIFF REQUIRED)

lucent_add_library(export
    SOURCES
        include/lucent/export/encode.hpp
        include/lucent/export/export.hpp
        include/lucent/export/output_profile.hpp
        src/codecs.hpp
        src/encode.cpp
        src/export.cpp
        src/fault_injection.hpp
        src/file_util.cpp
        src/file_util.hpp
        src/icc_builder.cpp
        src/icc_builder.hpp
        src/jpeg_codec.cpp
        src/lcms_support.cpp
        src/lcms_support.hpp
        src/output_profile.cpp
        src/tiff_codec.cpp
    PUBLIC_DEPS
        Lucent::core
        Lucent::image
    PRIVATE_DEPS
        lcms2::lcms2
        JPEG::JPEG
        TIFF::TIFF
)

# White-box tests: they see src/ to drive the fault-injection seam (src/fault_injection.hpp) and
# the verification readers (src/codecs.hpp), and link Little CMS to parse the generated ICC
# profiles independently of the code under test.
lucent_add_test(lucent_export_tests
    SOURCES
        tests/export_test_support.hpp
        tests/encode_tests.cpp
        tests/export_tests.cpp
        tests/output_profile_tests.cpp
    DEPS
        Lucent::export
        lcms2::lcms2
    LABELS
        export unit
)
if(TARGET lucent_export_tests)
    target_include_directories(lucent_export_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
endif()
