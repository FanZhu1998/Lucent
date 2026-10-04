# lucent_ui_tests: the shell's unit, adapter and QML tests (GoogleTest with Qt Test helpers).
#
# Runs on Qt's offscreen platform with the software scene graph (test_main.cpp), so no display or
# GPU is needed. Each test case is registered with CTest (label "ui"). It needs the build-tree Qt
# runtime (bin/qt.conf and the Qt DLLs) prepared by lucent_ui_runtime.
#
# lucent_ui_fakes are the simulated services the shell was first built against: a fast,
# deterministic test double for the QML tests. The adapter tests (application_*_tests.cpp) run the
# real application services on synthetic DNGs written with the raw module's test writers.

find_package(Qt6 6.11 REQUIRED COMPONENTS Test)

add_library(lucent_ui_fakes STATIC
    fakes/fake_edit_service.cpp
    fakes/fake_export_service.cpp
    fakes/fake_image_source.cpp
    fakes/fake_library_service.cpp
    fakes/fake_services.cpp
    fakes/fake_services.hpp
)
target_include_directories(lucent_ui_fakes PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(lucent_ui_fakes
    PUBLIC lucent_ui_services
    PRIVATE Lucent::compiler_options)
set_target_properties(lucent_ui_fakes PROPERTIES FOLDER "tests")

add_executable(lucent_ui_tests
    adapter_conversions_tests.cpp
    adapter_test_support.hpp
    app_controller_tests.cpp
    app_edit_service_tests.cpp
    application_adapters_tests.cpp
    application_shell_tests.cpp
    catalog_adapters_tests.cpp
    developer_options_tests.cpp
    edit_session_tests.cpp
    fake_services_tests.cpp
    image_provider_tests.cpp
    library_model_tests.cpp
    presentation_tests.cpp
    qml_shell_tests.cpp
    test_main.cpp
    test_support.cpp
    test_support.hpp
    volume_watcher_tests.cpp
)
target_include_directories(lucent_ui_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/src/raw/tests")
target_compile_definitions(lucent_ui_tests PRIVATE
    LUCENT_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    LUCENT_FIXTURE_DIR="${CMAKE_SOURCE_DIR}/fixtures")
target_link_libraries(lucent_ui_tests PRIVATE
    Lucent::compiler_options
    GTest::gtest
    lucent_ui
    lucent_ui_qmlplugin
    lucent_ui_fakes
    lucent_ui_adapters
    Qt6::Test)
set_target_properties(lucent_ui_tests PROPERTIES FOLDER "tests")
add_dependencies(lucent_ui_tests lucent_ui_runtime)

gtest_discover_tests(lucent_ui_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES
        LABELS "ui"
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
