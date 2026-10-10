# The generator of the committed catalog fixtures (W12, ADR 0022; the release gate "Update
# compatibility"): one catalog per historical schema version in src/catalog/tests/data/migration,
# made by driving older migration states (see generate_catalog_fixtures.cpp). It is a tool, not a
# test: the fixtures are committed, and the catalog's and application's tests migrate copies of them.
# Regenerate (only when a new schema version adds a fixture; existing fixtures never change):
#
#   build\<dir>\bin\lucent_catalog_fixture_generator.exe src\catalog\tests\data\migration
#   python tests\catalog_fixtures\check_historical_ddl.py
#
# It reaches private headers of the catalog (the migration list) and of the application (the batch
# documents and the support records' encoding), and the raw module's synthetic DNG writer.

find_package(nlohmann_json CONFIG REQUIRED)

add_executable(lucent_catalog_fixture_generator generate_catalog_fixtures.cpp)
target_link_libraries(lucent_catalog_fixture_generator PRIVATE
    Lucent::compiler_options
    Lucent::application
    Lucent::catalog
    nlohmann_json::nlohmann_json)
target_include_directories(lucent_catalog_fixture_generator PRIVATE
    "${CMAKE_SOURCE_DIR}/src/catalog/src"
    "${CMAKE_SOURCE_DIR}/src/application/src"
    "${CMAKE_SOURCE_DIR}/src/raw/tests")
set_target_properties(lucent_catalog_fixture_generator PROPERTIES FOLDER "tests")
