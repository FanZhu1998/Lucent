# lucent-probe: developer CLI that prints lucent.probe-report (schema 1) JSON for RAW files,
# comparable with the rawpy reference (research/README.md). See main.cpp for usage.
#
# The report builder is a small static library so that the fixture integration tests (tests/)
# produce reports with exactly the code the tool uses.

find_package(nlohmann_json CONFIG REQUIRED)

add_library(lucent_probe_report STATIC
    probe_report.cpp
    probe_report.hpp
)
target_include_directories(lucent_probe_report PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(lucent_probe_report
    PUBLIC
        Lucent::core
        Lucent::raw
        nlohmann_json::nlohmann_json
    PRIVATE
        Lucent::compiler_options
)
set_target_properties(lucent_probe_report PROPERTIES FOLDER "apps")

lucent_add_executable(lucent-probe
    SOURCES
        main.cpp
    DEPS
        lucent_probe_report
)
