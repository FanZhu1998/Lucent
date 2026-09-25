# Unit tests for lucent_core.

lucent_add_test(lucent_core_tests
    SOURCES
        cancellation_test.cpp
        error_test.cpp
        file_test.cpp
        hash_test.cpp
        log_test.cpp
        text_test.cpp
        test_support.hpp
        version_test.cpp
        win32_error_test.cpp
    DEPS
        Lucent::core
    LABELS
        core
)

# White-box tests reach private headers (for example the hash_file test seam in
# src/core/src/hash_internal.hpp). Nothing outside these tests may do so.
target_include_directories(lucent_core_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")
