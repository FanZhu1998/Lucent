cmake_minimum_required(VERSION 3.28)

# vcpkg builds some Qt sources below paths longer than MAX_PATH (260) when buildtrees live inside
# the vcpkg checkout. tools/dev-env.ps1 sets LUCENT_VCPKG_BUILDTREES to a short root; it must be
# applied before project(), where the vcpkg toolchain runs the manifest install.
if(DEFINED ENV{LUCENT_VCPKG_BUILDTREES} AND NOT VCPKG_INSTALL_OPTIONS)
    file(TO_CMAKE_PATH "$ENV{LUCENT_VCPKG_BUILDTREES}" _lucent_buildtrees)
    set(VCPKG_INSTALL_OPTIONS "--x-buildtrees-root=${_lucent_buildtrees}" CACHE STRING
        "Extra arguments for the vcpkg manifest install (set by Lucent)")
endif()

project(Lucent
    VERSION 0.1.0
    DESCRIPTION "Offline, camera-independent RAW photo development"
    HOMEPAGE_URL "https://github.com/FanZhu1998/Lucent"
    LANGUAGES C CXX)

if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Lucent's first platform is 64-bit Windows (see docs/architecture/overview.md).")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(LUCENT_BUILD_TESTS "Build unit and integration tests" ON)
option(LUCENT_BUILD_UI "Build the Qt Quick application (requires the vcpkg 'ui' feature)" OFF)
option(LUCENT_BUILD_TOOLS "Build developer tools such as lucent-probe" ON)
option(LUCENT_WARNINGS_AS_ERRORS "Treat compiler warnings in Lucent code as errors" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

# Every executable and DLL lands in one directory so tests and tools find their DLLs.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

include(LucentCompilerOptions)
include(LucentVersion)
include(LucentTargets)

if(LUCENT_BUILD_TESTS)
    enable_testing()
    find_package(GTest CONFIG REQUIRED)
    include(GoogleTest)
endif()

# Libraries, lowest layer first. Dependency rules: docs/architecture/overview.md.
add_subdirectory(src/core)
add_subdirectory(src/image)
add_subdirectory(src/raw)
add_subdirectory(src/engine)
add_subdirectory(src/export)
add_subdirectory(src/application)

if(LUCENT_BUILD_TOOLS)
    add_subdirectory(apps/lucent-probe)
    add_subdirectory(apps/lucent-render)
endif()

if(LUCENT_BUILD_UI)
    add_subdirectory(apps/lucent)
endif()

if(LUCENT_BUILD_TESTS)
    add_subdirectory(tests)
endif()
