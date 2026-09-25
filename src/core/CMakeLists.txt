# lucent_core: foundation utilities shared by every module (errors, hashing, files,
# logging, cancellation, versioning). Depends only on the C++ standard library, Win32 and
# tl-expected.

find_package(tl-expected CONFIG REQUIRED)

lucent_add_library(core
    SOURCES
        src/error.cpp
    PUBLIC_DEPS
        tl::expected
)
