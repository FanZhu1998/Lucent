# lucent_program_resource_script_tests (tests/branding/CMakeLists.txt; ADR 0025, review finding BRD-07):
# a program's generated resource script (lucent_add_program_resources, cmake/LucentBranding.cmake) names
# its icon by an absolute path, and must compile when a folder of that path has a name that is not ASCII,
# as in a checkout under such a folder. The test copies the icon into a folder of WORK_DIR named with an
# o-umlaut and a Chinese character, points a copy of the script at it and compiles that with the build's
# resource compiler.
#
#   cmake -DRC_COMPILER=<rc.exe> -DSCRIPT=<generated .rc> -DICON=<the icon it names>
#         -DWORK_DIR=<scratch folder> [-DINCLUDE_DIRS=<dir>|<dir>...] -P program_resources_test.cmake
#
# INCLUDE_DIRS, the configure step's INCLUDE ('|'-separated), lets RC find <winver.h> when CTest runs
# outside the developer environment.

cmake_minimum_required(VERSION 3.28)

foreach(name IN ITEMS RC_COMPILER SCRIPT ICON WORK_DIR)
    if(NOT DEFINED ${name} OR "${${name}}" STREQUAL "")
        message(FATAL_ERROR "program_resources_test.cmake: ${name} is not set")
    endif()
endforeach()
# Forward slashes, as in the generated script: RC reads a backslash in a string as an escape.
file(TO_CMAKE_PATH "${WORK_DIR}" WORK_DIR)

file(READ "${SCRIPT}" script)
string(FIND "${script}" "\"${ICON}\"" at)
if(at EQUAL -1)
    message(FATAL_ERROR "${SCRIPT} does not name the icon \"${ICON}\"")
endif()

# A Latin-1 and a CJK character, from JSON escapes so that this file stays ASCII.
string(JSON folder_name GET [=[{"name": "Lucent J\u00f6rg \u5f20"}]=] name)
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}/${folder_name}")
set(icon "${WORK_DIR}/${folder_name}/lucent.ico")
file(COPY_FILE "${ICON}" "${icon}")
string(REPLACE "\"${ICON}\"" "\"${icon}\"" script "${script}")
file(WRITE "${WORK_DIR}/program.rc" "${script}")

if(DEFINED INCLUDE_DIRS AND NOT INCLUDE_DIRS STREQUAL "")
    string(REPLACE "|" ";" include "${INCLUDE_DIRS}")
    set(ENV{INCLUDE} "${include};$ENV{INCLUDE}")
endif()
execute_process(
    COMMAND "${RC_COMPILER}" /nologo /fo "${WORK_DIR}/program.res" "${WORK_DIR}/program.rc"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE output)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "RC did not compile the resource script with its icon in '${WORK_DIR}/${folder_name}' "
                        "(${result}):\n${output}")
endif()
file(SIZE "${ICON}" icon_size)
file(SIZE "${WORK_DIR}/program.res" res_size)
if(res_size LESS icon_size)
    message(FATAL_ERROR "The compiled resources (${res_size} bytes) cannot hold the icon (${icon_size} bytes).")
endif()
message(STATUS "Compiled with the icon in '${WORK_DIR}/${folder_name}': ${res_size} bytes of resources.")
