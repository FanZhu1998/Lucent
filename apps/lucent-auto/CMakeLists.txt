# lucent-auto: developer CLI that runs Natural v1 automatic settings over files/folders and writes evaluation reports (W8).
#
# Not shipped. Usage and output layout: auto_tool.hpp. The argument handling, discovery, reports
# and sheets live in a small static library so that the tests (tests/) exercise exactly the code
# the tool uses; the side-file publication and JSON helpers are lucent-render's (lucent_render_tool).

find_package(nlohmann_json CONFIG REQUIRED)

add_library(lucent_auto_tool STATIC
    auto_tool.cpp
    auto_tool.hpp
    auto_report.cpp
    auto_report.hpp
)
target_include_directories(lucent_auto_tool PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(lucent_auto_tool
    PUBLIC
        Lucent::core
        Lucent::analysis
        Lucent::export
        Lucent::application
        lucent_render_tool
        nlohmann_json::nlohmann_json
    PRIVATE
        Lucent::compiler_options
)
set_target_properties(lucent_auto_tool PROPERTIES FOLDER "apps")

lucent_add_executable(lucent-auto
    SOURCES
        main.cpp
    DEPS
        lucent_auto_tool
)

# Tests: argument handling, discovery and output protection (always run), and the tool on the
# smallest smoke fixture (skips when it is not downloaded).
lucent_add_test(lucent_auto_tests
    SOURCES
        tests/lucent_auto_tests.cpp
    DEPS
        lucent_auto_tool
    LABELS
        tools fixtures
)
if(TARGET lucent_auto_tests)
    # The output-layout test runs bin/lucent-auto.exe from the same output directory.
    add_dependencies(lucent_auto_tests lucent-auto)
endif()
