# Lucent.exe: the Qt Quick desktop application (blueprint sections 2, 4 and 9; W3).
#
# Targets
#   lucent_ui_services   UI-side service interfaces (library, images, exports) and ImageReply.
#                        Qt Core/Gui only; the seam the application services bind to.
#   lucent_ui_adapters   The UI-side services implemented over the application services
#                        (lucent_application): import, camera and Lucent previews, export. Binds
#                        them in src/service_factory.cpp (docs/adr/0007-application-shell.md).
#   lucent_ui            QML module "Lucent" (static library): QML views, view models, controller,
#                        asynchronous image providers, developer automation.
#   lucent_ui_qmlplugin  The module's static QML plugin, imported with Q_IMPORT_QML_PLUGIN.
#   Lucent               Lucent.exe: WIN32 subsystem (no console window), version resource.
#   lucent_ui_tests      GoogleTest + Qt Test, offscreen platform, software scene graph.
#   lucent_ui_runtime    Build-tree runtime (see below).
#
# Running from the build tree: Qt's plugins and QML imports are used in place from the vcpkg
# installation through a generated bin/qt.conf, and every vcpkg runtime DLL is copied next to the
# executables so that the dependencies of those plugins resolve through the application directory.
# This is a developer convenience only; the installer deploys with windeployqt (W5) and never ships
# this qt.conf.

find_package(Qt6 6.11 REQUIRED COMPONENTS Core Gui Qml Quick QuickControls2)
qt_standard_project_setup(REQUIRES 6.11)

# --- Service interfaces --------------------------------------------------------------------------

add_library(lucent_ui_services STATIC
    src/services/export_service.hpp
    src/services/image_reply.cpp
    src/services/image_source.hpp
    src/services/library_service.cpp
    src/services/library_service.hpp
    src/services/service_types.hpp
    src/services/services.hpp
)
target_include_directories(lucent_ui_services PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(lucent_ui_services
    PUBLIC Qt6::Core Qt6::Gui
    PRIVATE Lucent::compiler_options)
set_target_properties(lucent_ui_services PROPERTIES FOLDER "apps/lucent")

# --- Adapters over the application services -----------------------------------------------------

add_library(lucent_ui_adapters STATIC
    src/adapters/app_export_service.cpp
    src/adapters/app_export_service.hpp
    src/adapters/app_image_source.cpp
    src/adapters/app_image_source.hpp
    src/adapters/app_library_service.cpp
    src/adapters/app_library_service.hpp
    src/adapters/application_services.cpp
    src/adapters/application_services.hpp
    src/adapters/backend.cpp
    src/adapters/backend.hpp
    src/adapters/conversions.cpp
    src/adapters/conversions.hpp
    src/adapters/ui_dispatcher.hpp
)
target_link_libraries(lucent_ui_adapters
    PUBLIC lucent_ui_services Lucent::application Qt6::Core Qt6::Gui
    PRIVATE Lucent::compiler_options)
set_target_properties(lucent_ui_adapters PROPERTIES FOLDER "apps/lucent")

# --- QML module ----------------------------------------------------------------------------------

set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_library(lucent_ui STATIC)
qt_add_qml_module(lucent_ui
    URI Lucent
    VERSION 1.0
    PLUGIN_TARGET lucent_ui_qmlplugin
    CLASS_NAME LucentPlugin
    # Must end in the module's target path for qmllint and other QML tooling.
    OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/qml/Lucent"
    QML_FILES
        qml/AppHeader.qml
        qml/Badge.qml
        qml/EditView.qml
        qml/ExportDialog.qml
        qml/ExportJobRow.qml
        qml/ExportsView.qml
        qml/Filmstrip.qml
        qml/FocusRing.qml
        qml/Histogram.qml
        qml/ImportDialog.qml
        qml/LibraryEmptyState.qml
        qml/LibraryView.qml
        qml/Main.qml
        qml/MetadataPanel.qml
        qml/NavButton.qml
        qml/StyledButton.qml
        qml/StyledCheckBox.qml
        qml/StyledToolButton.qml
        qml/Theme.qml
        qml/ThumbnailCard.qml
        qml/ZoomableImage.qml
    SOURCES
        src/app_controller.cpp
        src/app_controller.hpp
        src/developer_automation.cpp
        src/developer_automation.hpp
        src/imaging/generation_registry.cpp
        src/imaging/generation_registry.hpp
        src/imaging/histogram_plot.cpp
        src/imaging/histogram_plot.hpp
        src/imaging/image_id.cpp
        src/imaging/image_id.hpp
        src/imaging/image_provider.cpp
        src/imaging/image_provider.hpp
        src/models/current_item.cpp
        src/models/current_item.hpp
        src/models/export_job_model.cpp
        src/models/export_job_model.hpp
        src/models/library_model.cpp
        src/models/library_model.hpp
        src/models/presentation.cpp
        src/models/presentation.hpp
        src/shell.cpp
        src/shell.hpp
        src/volume_watcher.cpp
        src/volume_watcher.hpp
)
target_link_libraries(lucent_ui
    PUBLIC
        lucent_ui_services
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
    PRIVATE
        Lucent::compiler_options)
target_compile_definitions(lucent_ui PRIVATE "LUCENT_APP_VERSION=\"${PROJECT_VERSION}\"")
# The generated QML type registrations include each QML_ELEMENT header by its file name.
target_include_directories(lucent_ui PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/imaging"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/models")
set_target_properties(lucent_ui lucent_ui_qmlplugin PROPERTIES FOLDER "apps/lucent")

# --- Lucent.exe ----------------------------------------------------------------------------------

set(LUCENT_RC_COMPANY "Lucent")
set(LUCENT_RC_DESCRIPTION "Lucent RAW photo developer")
set(LUCENT_RC_COPYRIGHT "Copyright (c) 2026 Fan Zhu. MIT License.")
configure_file(lucent.rc.in "${CMAKE_CURRENT_BINARY_DIR}/generated/lucent.rc" @ONLY)

qt_add_executable(Lucent WIN32
    src/main.cpp
    src/service_factory.cpp
    src/service_factory.hpp
    lucent.exe.manifest
    "${CMAKE_CURRENT_BINARY_DIR}/generated/lucent.rc"
)
target_link_libraries(Lucent PRIVATE
    Lucent::compiler_options
    lucent_ui
    lucent_ui_qmlplugin
    lucent_ui_adapters)
set_target_properties(Lucent PROPERTIES
    OUTPUT_NAME Lucent
    FOLDER "apps")

# --- Build-tree runtime --------------------------------------------------------------------------

set(_lucent_qt_bin_dir "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}")
set(_lucent_qt_conf "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/qt.conf")
file(GLOB _lucent_runtime_dlls CONFIGURE_DEPENDS "${_lucent_qt_bin_dir}/*.dll")
add_custom_command(
    OUTPUT "${_lucent_qt_conf}"
    COMMAND "${CMAKE_COMMAND}"
        "-DSOURCE_DIR=${_lucent_qt_bin_dir}"
        "-DDESTINATION_DIR=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
        "-DQT_PREFIX=${QT6_INSTALL_PREFIX}"
        "-DQT_PLUGINS=${QT6_INSTALL_PLUGINS}"
        "-DQT_QML=${QT6_INSTALL_QML}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_tree_runtime.cmake"
    DEPENDS ${_lucent_runtime_dlls} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_tree_runtime.cmake"
    COMMENT "Preparing the build-tree Qt runtime (bin/qt.conf and runtime DLLs)"
    VERBATIM)
add_custom_target(lucent_ui_runtime DEPENDS "${_lucent_qt_conf}")
set_target_properties(lucent_ui_runtime PROPERTIES FOLDER "apps/lucent")
add_dependencies(Lucent lucent_ui_runtime)

if(LUCENT_BUILD_TESTS)
    add_subdirectory(tests)
endif()
