# lucent_core: foundation utilities shared by every module (errors, text conversion, Win32
# error mapping, read-only file access and identity, SHA-256 hashing, cancellation, logging,
# versioning). Depends only on the C++ standard library, Win32 and tl-expected.

find_package(tl-expected CONFIG REQUIRED)

# Build identity (see cmake/LucentVersion.cmake). The generated header is private to this
# library; other modules read it through lucent::core::build_info().
configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/src/build_config.hpp.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/build_config.hpp"
    @ONLY)

lucent_add_library(core
    SOURCES
        include/lucent/core/cancellation.hpp
        include/lucent/core/error.hpp
        include/lucent/core/file.hpp
        include/lucent/core/hash.hpp
        include/lucent/core/log.hpp
        include/lucent/core/text.hpp
        include/lucent/core/version.hpp
        include/lucent/core/win32_error.hpp
        src/cancellation.cpp
        src/error.cpp
        src/file.cpp
        src/hash.cpp
        src/hash_internal.hpp
        src/log.cpp
        src/text.cpp
        src/version.cpp
        src/win32.cpp
        src/win32.hpp
        src/win32_error.cpp
        src/build_config.hpp.in
    PUBLIC_DEPS
        tl::expected
    PRIVATE_DEPS
        bcrypt  # Windows CNG (SHA-256)
)

target_include_directories(lucent_core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
# $<CONFIG> also works with multi-config generators, which configure_file() cannot express.
target_compile_definitions(lucent_core PRIVATE "LUCENT_BUILD_TYPE=\"$<CONFIG>\"")

if(LUCENT_BUILD_TESTS)
    add_subdirectory(tests)
endif()
