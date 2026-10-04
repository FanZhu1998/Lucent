# lucent_raw: RAW container inspection, the RawDecoder interface, the decoded-RAW data contract
# and decoder adapters. See docs/architecture/overview.md for the layering rules and
# docs/adr/0005-raw-decoder-boundary.md for the decoder boundary.
#
# Layout
#   include/lucent/raw/   public headers (no third-party types)
#   src/                  implementation and private headers
#   tests/                GoogleTest unit tests (synthetic data; fixture-based cases skip when the
#                         fixture cache is absent)
#
# LibRaw (the thread-safe libraw_r build) is linked PRIVATE: only src/libraw_*.cpp include its
# headers, so no LibRaw type leaks into the public headers.

find_package(libraw CONFIG REQUIRED)

set(LUCENT_RAW_SOURCES
    # TIFF / DNG container inspection (bounds-checked; no third-party code).
    include/lucent/raw/dng_opcodes.hpp
    include/lucent/raw/dng_tags.hpp
    include/lucent/raw/dng_version.hpp
    include/lucent/raw/tiff_inspector.hpp
    src/dng_opcodes.cpp
    src/dng_tags.cpp
    src/tiff_inspector.cpp
    src/tiff_reader.cpp
    src/tiff_reader.hpp
    src/tiff_source.cpp

    # Decoder boundary: data contract, interface, registry, file source.
    include/lucent/raw/decoded_raw.hpp
    include/lucent/raw/decoder_registry.hpp
    include/lucent/raw/file_source.hpp
    include/lucent/raw/raw_decoder.hpp
    src/decoded_raw.cpp
    src/decoder_registry.cpp
    src/file_source.cpp

    # LibRaw adapter.
    include/lucent/raw/libraw_decoder.hpp
    src/libraw_decoder.cpp
    src/libraw_errors.cpp
    src/libraw_metadata.cpp
    src/libraw_stream.cpp
    src/libraw_stream.hpp
    src/libraw_support.hpp
    src/container_extent.cpp
    src/container_extent.hpp
    src/tiff_extent.cpp
    src/tiff_extent.hpp

    # Embedded camera previews (LibRaw thumbnail list; never unpacks the RAW data).
    include/lucent/raw/embedded_preview.hpp
    src/jpeg_header.cpp
    src/jpeg_header.hpp
    src/libraw_preview.cpp

    # Capture metadata for exports: Lucent's own bounded EXIF reader (no third-party code).
    include/lucent/raw/capture_metadata.hpp
    src/capture_metadata.cpp
)

lucent_add_library(raw
    SOURCES
        ${LUCENT_RAW_SOURCES}
    PUBLIC_DEPS
        Lucent::core
        Lucent::image
    PRIVATE_DEPS
        libraw::raw_r
)

# ---------------------------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------------------------
# lucent_add_test() is a no-op when LUCENT_BUILD_TESTS is OFF.

# TIFF/DNG inspector: synthetic streams built by tests/tiff_test_writer.hpp; no fixtures needed.
lucent_add_test(lucent_raw_tiff_tests
    SOURCES
        tests/tiff_test_writer.hpp
        tests/capture_metadata_test.cpp
        tests/dng_opcodes_test.cpp
        tests/tiff_corrupt_test.cpp
        tests/tiff_inspector_test.cpp
        tests/tiff_mutation_test.cpp
    DEPS
        Lucent::raw
    LABELS
        raw unit
)

# Decoder boundary and LibRaw adapter. Synthetic DNGs are written by tests/synthetic_dng.hpp;
# the cases in tests/libraw_fixture_test.cpp use downloaded camera fixtures and skip when they
# are absent. The test links LibRaw and sees src/ to check the error mapping against LibRaw's
# constants and to test the private datastream (src/libraw_stream.hpp) directly.
lucent_add_test(lucent_raw_decoder_tests
    SOURCES
        tests/tiff_test_writer.hpp
        tests/synthetic_dng.hpp
        tests/raw_test_support.hpp
        tests/decoded_raw_test.cpp
        tests/decoder_registry_test.cpp
        tests/file_source_test.cpp
        tests/libraw_errors_test.cpp
        tests/libraw_stream_test.cpp
        tests/tiff_extent_test.cpp
        tests/container_extent_test.cpp
        tests/libraw_metadata_test.cpp
        tests/libraw_decoder_test.cpp
        tests/libraw_fixture_test.cpp
        tests/capture_metadata_fixture_test.cpp
        tests/test_jpegs.hpp
        tests/jpeg_header_test.cpp
        tests/embedded_preview_test.cpp
    DEPS
        Lucent::raw
        libraw::raw_r
    LABELS
        raw unit
)
if(TARGET lucent_raw_decoder_tests)
    target_include_directories(lucent_raw_decoder_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
endif()
