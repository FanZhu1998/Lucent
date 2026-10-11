# lucent-render: developer CLI that renders a RAW file through the engine's develop pipeline (ADR
# 0010; the W2 render path of ADR 0006 with --legacy-neutral-v0) and exports it. See main.cpp for
# usage.
#
# The float-TIFF dump, the side-file publication helpers and the JSON documents live in a small
# static library so that the integration tests (tests/) exercise exactly the code the tool uses.
# Recipes (--develop) are read with lucent_application's recipe reader (recipe.hpp); the lens database
# (ADR 0023) is the verified bundled asset (lucent_assets, lens_database.hpp).

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
        Lucent::application
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
        Lucent::assets  # the bundled lens database (ADR 0023)
)
