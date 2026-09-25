# Helpers that give every Lucent module the same layout and settings.
#
# Module layout:
#   src/<module>/include/lucent/<module>/*.hpp   public headers
#   src/<module>/src/*.cpp, *.hpp                implementation and private headers
#   src/<module>/tests/*.cpp                     unit tests (GoogleTest)

# lucent_add_library(<name> SOURCES ... [PUBLIC_DEPS ...] [PRIVATE_DEPS ...])
# Creates the static library lucent_<name> with the alias Lucent::<name>.
function(lucent_add_library name)
    cmake_parse_arguments(ARG "" "" "SOURCES;PUBLIC_DEPS;PRIVATE_DEPS" ${ARGN})
    set(target "lucent_${name}")
    add_library(${target} STATIC ${ARG_SOURCES})
    add_library(Lucent::${name} ALIAS ${target})
    target_include_directories(${target}
        PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(${target}
        PUBLIC ${ARG_PUBLIC_DEPS}
        PRIVATE Lucent::compiler_options ${ARG_PRIVATE_DEPS})
    set_target_properties(${target} PROPERTIES FOLDER "lucent/${name}")
endfunction()

# lucent_add_test(<name> SOURCES ... [DEPS ...] [LABELS ...])
# Creates a GoogleTest executable and registers each test case with CTest.
# Tests receive LUCENT_SOURCE_DIR and LUCENT_FIXTURE_DIR as string macros.
function(lucent_add_test name)
    if(NOT LUCENT_BUILD_TESTS)
        return()
    endif()
    cmake_parse_arguments(ARG "" "" "SOURCES;DEPS;LABELS" ${ARGN})
    add_executable(${name} ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE Lucent::compiler_options GTest::gtest GTest::gtest_main ${ARG_DEPS})
    target_compile_definitions(${name} PRIVATE
        LUCENT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
        LUCENT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures")
    set_target_properties(${name} PROPERTIES FOLDER "tests")
    gtest_discover_tests(${name}
        DISCOVERY_MODE PRE_TEST
        PROPERTIES LABELS "${ARG_LABELS}")
endfunction()

# lucent_add_executable(<name> SOURCES ... [DEPS ...] [WIN32])
# WIN32 builds a GUI-subsystem executable that never opens a console window.
function(lucent_add_executable name)
    cmake_parse_arguments(ARG "WIN32" "" "SOURCES;DEPS" ${ARGN})
    if(ARG_WIN32)
        add_executable(${name} WIN32 ${ARG_SOURCES})
    else()
        add_executable(${name} ${ARG_SOURCES})
    endif()
    target_link_libraries(${name} PRIVATE Lucent::compiler_options ${ARG_DEPS})
    set_target_properties(${name} PROPERTIES FOLDER "apps")
endfunction()
