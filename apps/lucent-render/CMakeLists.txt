# lucent-render: developer CLI that renders a RAW file through the engine's first render path and
# exports it (ADR 0006). See main.cpp for usage.
#
# The float-TIFF dump, the side-file publication helpers and the JSON documents live in a small
# static library so that the integration tests (tests/) exercise exactly the code the tool uses.

find_package(nlohmann_json CONFIG REQUIRED)

add_library(lucent_render_tool STATIC
    float_tiff.cpp
    float_tiff.hpp
    tool_files.cpp
    tool_files.hpp
    render_json.cpp
    render_json.hpp
)
target_include_directories(lucent_render_tool PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(lucent_render_tool
    PUBLIC
        Lucent::core
        Lucent::image
        Lucent::engine
        Lucent::export
        nlohmann_json::nlohmann_json
    PRIVATE
        Lucent::compiler_options
)
set_target_properties(lucent_render_tool PROPERTIES FOLDER "apps")

lucent_add_executable(lucent-render
    SOURCES
        main.cpp
    DEPS
        lucent_render_tool
        lucent_probe_report
        Lucent::raw
)
