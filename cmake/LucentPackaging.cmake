# Installed layout of Lucent.exe for the Windows package (W5, docs/adr/0009-windows-packaging.md).
#
#   cmake --install <build dir> --prefix <folder> --component Lucent
#
# produces the folder the installer ships:
#
#   Lucent.exe                 the application (WIN32 subsystem)
#   lucent-worker.exe          the isolated processing worker Lucent.exe starts for exports and
#                              batches (W7, ADR 0012; WIN32 subsystem, no window); it must sit next to
#                              Lucent.exe (workers::default_worker_executable)
#   *.dll                      Qt and every other runtime DLL, unmodified from the vcpkg installation
#   plugins\<type>\*.dll       Qt plugins (platform, image formats, icon engines)
#   qml\...                    the QML modules Lucent imports, deployed by windeployqt
#   qt.conf                    points Qt at plugins\ and qml\ relative to the executable
#   assets\<id>\...            the bundled data assets (W10, ADR 0018; LUCENT_BUNDLED_ASSETS below):
#                              each asset's manifest.json and exactly the files it lists, copied
#                              unchanged from the source tree's assets\<id> (the camera support
#                              database first). Lucent.exe verifies them against the manifest hash
#                              compiled into it; tools/package.ps1 audits the same hashes.
#
# Deployment runs at install time:
#   1. windeployqt deploys the Qt libraries, plugins and the QML imports found by scanning
#      apps/lucent/qml (--no-patchqt keeps Qt6Core.dll byte-identical to the vcpkg build). The Qt
#      plugins Lucent cannot run without (LUCENT_PACKAGE_REQUIRED_PLUGINS) must be among them.
#   2. QML modules Lucent never loads are removed (the unused Qt Quick Controls styles).
#   3. qt.conf is written with paths relative to the executable only.
#   4. file(GET_RUNTIME_DEPENDENCIES) resolves the imports of Lucent.exe, the helper executables
#      (lucent-worker.exe) and every plugin against
#      the folder and the vcpkg bin directory. Missing DLLs (zlib, FreeType, LibRaw, ...) are copied
#      from vcpkg; DLLs that nothing imports any more (those of the removed styles) are deleted.
#      Windows system DLLs and the Visual C++ runtime are never copied: the runtime is installed by
#      Microsoft's redistributable (installer/lucent.iss).
#   5. A second pass resolves everything against the folder alone and fails on anything missing.
#
# tools/package.ps1 then audits the folder independently (dumpbin), adds licences and the SBOM,
# scans every shipped file for the build machine's folders (the user profile, the repository, the
# build tree and the vcpkg folders recorded below; W12), and compiles the installer. Developer tools
# (lucent-probe, lucent-render) and tests have no install rules and are never part of the layout.
#
# This file is included twice: by the root CMakeLists.txt at configure time (it defines
# lucent_install_application) and by the generated install script, where only
# lucent_deploy_runtime is needed (LUCENT_PACKAGING_INSTALL_TIME is set).

include_guard(GLOBAL)

# file(GET_RUNTIME_DEPENDENCIES) compares normalized (forward-slash) paths with the regexes below.
if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
endif()

# Builds a regular expression that matches the start of `path` regardless of letter case and of the
# separator style, for comparing Windows paths reported by different tools.
function(_lucent_path_prefix_regex path out_var)
    file(TO_CMAKE_PATH "${path}" path)
    string(REGEX REPLACE "/+$" "" path "${path}")
    string(LENGTH "${path}" length)
    set(regex "^")
    if(length GREATER 0)
        math(EXPR last "${length} - 1")
        foreach(index RANGE ${last})
            string(SUBSTRING "${path}" ${index} 1 char)
            string(TOLOWER "${char}" lower)
            string(TOUPPER "${char}" upper)
            if(char STREQUAL "/")
                string(APPEND regex "[/\\\\]")
            elseif(NOT lower STREQUAL upper)
                string(APPEND regex "[${lower}${upper}]")
            elseif(char MATCHES "[][.*+?^$(){}|\\\\]")
                string(APPEND regex "\\${char}")
            else()
                string(APPEND regex "${char}")
            endif()
        endforeach()
    endif()
    string(APPEND regex "[/\\\\]")
    set(${out_var} "${regex}" PARENT_SCOPE)
endfunction()

# Install time. See the header for the steps.
#
# lucent_deploy_runtime(EXECUTABLE <installed exe> [HELPERS <installed exe>...]
#                       WINDEPLOYQT <windeployqt.exe> DUMPBIN <dumpbin.exe>
#                       QML_SOURCE_DIR <dir> LOG_FILE <file>
#                       SEARCH_DIRECTORIES <dir>... SKIPPED_PLUGIN_TYPES <type>...
#                       REQUIRED_PLUGINS <plugins-relative file>...
#                       UNUSED_QML_MODULES <qml-relative dir>... VC_RUNTIME_DLLS <name>...)
function(lucent_deploy_runtime)
    cmake_parse_arguments(ARG ""
        "EXECUTABLE;WINDEPLOYQT;DUMPBIN;QML_SOURCE_DIR;LOG_FILE"
        "HELPERS;SEARCH_DIRECTORIES;SKIPPED_PLUGIN_TYPES;REQUIRED_PLUGINS;UNUSED_QML_MODULES;VC_RUNTIME_DLLS" ${ARGN})
    foreach(required EXECUTABLE WINDEPLOYQT DUMPBIN QML_SOURCE_DIR LOG_FILE SEARCH_DIRECTORIES)
        if(NOT ARG_${required})
            message(FATAL_ERROR "lucent_deploy_runtime: ${required} is not set")
        endif()
    endforeach()
    foreach(tool "${ARG_WINDEPLOYQT}" "${ARG_DUMPBIN}" "${ARG_EXECUTABLE}" ${ARG_HELPERS})
        if(NOT EXISTS "${tool}")
            message(FATAL_ERROR "lucent_deploy_runtime: ${tool} does not exist")
        endif()
    endforeach()
    get_filename_component(prefix "${ARG_EXECUTABLE}" DIRECTORY)

    # 1. Qt libraries, plugins and QML imports.
    string(REPLACE ";" "," skipped_types "${ARG_SKIPPED_PLUGIN_TYPES}")
    set(windeployqt_args
        --release
        --no-patchqt
        --no-translations
        --no-compiler-runtime
        --no-system-d3d-compiler
        --no-system-dxc-compiler
        --no-opengl-sw
        --qmldir "${ARG_QML_SOURCE_DIR}"
        --plugindir "${prefix}/plugins"
        --qml-deploy-dir "${prefix}/qml"
        --verbose 1)
    if(skipped_types)
        list(APPEND windeployqt_args --skip-plugin-types "${skipped_types}")
    endif()
    message(STATUS "Deploying Qt with windeployqt (log: ${ARG_LOG_FILE})")
    execute_process(
        COMMAND "${ARG_WINDEPLOYQT}" ${windeployqt_args} "${ARG_EXECUTABLE}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE output)
    file(WRITE "${ARG_LOG_FILE}" "${output}")
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "windeployqt failed (${result}):\n${output}")
    endif()

    # Qt loads plugins by name at run time, so no import table reveals a missing one.
    foreach(plugin IN LISTS ARG_REQUIRED_PLUGINS)
        if(NOT EXISTS "${prefix}/plugins/${plugin}")
            message(FATAL_ERROR "windeployqt did not deploy the required Qt plugin plugins/${plugin}")
        endif()
    endforeach()

    # 2. QML modules that Lucent never loads.
    foreach(module IN LISTS ARG_UNUSED_QML_MODULES)
        if(IS_DIRECTORY "${prefix}/qml/${module}")
            file(REMOVE_RECURSE "${prefix}/qml/${module}")
            message(STATUS "Removed unused QML module qml/${module}")
        endif()
    endforeach()

    # 3. qt.conf: relative to the folder of the executable, no build-tree or vcpkg paths.
    file(WRITE "${prefix}/qt.conf"
"; Written by cmake/LucentPackaging.cmake for the installed layout of Lucent.
; Paths are relative to this folder.
[Paths]
Prefix = .
Plugins = plugins
QmlImports = qml
")

    # 4. Runtime dependencies of the executable and of every plugin.
    set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM "windows+pe")
    set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL "dumpbin")
    set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND "${ARG_DUMPBIN}")
    set(windows_dir "$ENV{SystemRoot}")
    if(NOT windows_dir)
        set(windows_dir "C:/Windows")
    endif()
    file(TO_CMAKE_PATH "${windows_dir}" windows_dir)
    _lucent_path_prefix_regex("${windows_dir}" windows_regex)
    _lucent_path_prefix_regex("${prefix}" prefix_regex)
    # Resolve the way a clean machine would: only Windows' own folders after the given directories,
    # never the developer PATH (Visual Studio, Git, Python, ... carry DLLs of the same names).
    set(saved_path "$ENV{PATH}")
    set(ENV{PATH} "${windows_dir}/System32;${windows_dir}")
    # Dependency names are lower case here. API sets are Windows; the Visual C++ runtime DLLs come from
    # Microsoft's redistributable, never from this folder.
    set(pre_exclude "^api-ms-" "^ext-ms-")
    foreach(name IN LISTS ARG_VC_RUNTIME_DLLS)
        string(TOLOWER "${name}" name)
        string(REPLACE "." "\\." name "${name}")
        list(APPEND pre_exclude "^${name}$")
    endforeach()

    file(GLOB_RECURSE modules LIST_DIRECTORIES false "${prefix}/plugins/*.dll" "${prefix}/qml/*.dll")
    file(GET_RUNTIME_DEPENDENCIES
        RESOLVED_DEPENDENCIES_VAR resolved
        UNRESOLVED_DEPENDENCIES_VAR unresolved
        CONFLICTING_DEPENDENCIES_PREFIX conflicting
        EXECUTABLES "${ARG_EXECUTABLE}" ${ARG_HELPERS}
        MODULES ${modules}
        DIRECTORIES "${prefix}" ${ARG_SEARCH_DIRECTORIES}
        PRE_EXCLUDE_REGEXES ${pre_exclude}
        POST_EXCLUDE_REGEXES "${windows_regex}")
    if(unresolved)
        message(FATAL_ERROR "Unresolved runtime dependencies: ${unresolved}")
    endif()
    if(conflicting_FILENAMES)
        set(details "")
        foreach(name IN LISTS conflicting_FILENAMES)
            string(APPEND details "\n  ${name}: ${conflicting_${name}}")
        endforeach()
        message(FATAL_ERROR "Runtime dependencies resolve to more than one file:${details}")
    endif()

    set(search_regexes "")
    foreach(directory IN LISTS ARG_SEARCH_DIRECTORIES)
        _lucent_path_prefix_regex("${directory}" regex)
        list(APPEND search_regexes "${regex}")
    endforeach()

    set(reachable "")
    set(copied "")
    foreach(dependency IN LISTS resolved)
        get_filename_component(name "${dependency}" NAME)
        string(TOLOWER "${name}" lower_name)
        if(dependency MATCHES "${prefix_regex}")
            list(APPEND reachable "${lower_name}")
            continue()
        endif()
        set(from_search_directory FALSE)
        foreach(regex IN LISTS search_regexes)
            if(dependency MATCHES "${regex}")
                set(from_search_directory TRUE)
                break()
            endif()
        endforeach()
        if(NOT from_search_directory)
            message(FATAL_ERROR "${name} resolved outside the Qt/vcpkg runtime directories: ${dependency}")
        endif()
        file(COPY "${dependency}" DESTINATION "${prefix}")
        list(APPEND reachable "${lower_name}")
        list(APPEND copied "${name}")
    endforeach()
    list(SORT copied)
    string(REPLACE ";" ", " copied_text "${copied}")
    message(STATUS "Copied runtime DLLs: ${copied_text}")

    # DLLs in the folder that neither the executables nor any remaining plugin imports.
    file(GLOB root_dlls LIST_DIRECTORIES false "${prefix}/*.dll")
    set(removed "")
    foreach(dll IN LISTS root_dlls)
        get_filename_component(name "${dll}" NAME)
        string(TOLOWER "${name}" lower_name)
        if(NOT lower_name IN_LIST reachable)
            file(REMOVE "${dll}")
            list(APPEND removed "${name}")
        endif()
    endforeach()
    if(removed)
        string(REPLACE ";" ", " removed_text "${removed}")
        message(STATUS "Removed DLLs that nothing imports: ${removed_text}")
    endif()

    # windeployqt leaves empty folders for QML modules it skipped; the installer would recreate them.
    file(GLOB_RECURSE directories LIST_DIRECTORIES true "${prefix}/plugins/*" "${prefix}/qml/*")
    list(SORT directories ORDER DESCENDING)  # children before their parents
    foreach(directory IN LISTS directories)
        if(IS_DIRECTORY "${directory}")
            file(GLOB entries "${directory}/*")
            if(NOT entries)
                file(REMOVE_RECURSE "${directory}")
            endif()
        endif()
    endforeach()

    # 5. Everything must now resolve inside the folder (or to Windows).
    file(GLOB_RECURSE modules LIST_DIRECTORIES false "${prefix}/plugins/*.dll" "${prefix}/qml/*.dll")
    file(GET_RUNTIME_DEPENDENCIES
        RESOLVED_DEPENDENCIES_VAR resolved
        UNRESOLVED_DEPENDENCIES_VAR unresolved
        EXECUTABLES "${ARG_EXECUTABLE}" ${ARG_HELPERS}
        MODULES ${modules}
        DIRECTORIES "${prefix}"
        PRE_EXCLUDE_REGEXES ${pre_exclude}
        POST_EXCLUDE_REGEXES "${windows_regex}")
    foreach(dependency IN LISTS resolved)
        if(NOT dependency MATCHES "${prefix_regex}")
            list(APPEND unresolved "${dependency}")
        endif()
    endforeach()
    if(unresolved)
        message(FATAL_ERROR "The installed layout is incomplete; not found in ${prefix}: ${unresolved}")
    endif()
    set(ENV{PATH} "${saved_path}")
endfunction()

if(LUCENT_PACKAGING_INSTALL_TIME)
    return()
endif()

# --- Configure time -------------------------------------------------------------------------------

# Bundled data assets (W10 phase 3, ADR 0018): folders of the source tree's assets/ directory, each
# with a manifest.json ("lucent.asset-manifest" v1, src/assets/include/lucent/assets/asset_manifest.hpp)
# and the files it lists. src/assets/CMakeLists.txt pins the SHA-256 of each manifest into the build
# and copies the asset next to the build's executables; lucent_install_application() installs it into
# <prefix>/assets/<id> and records it in lucent-package.json for the packaging audit.
set(LUCENT_ASSETS_SOURCE_DIR "${CMAKE_SOURCE_DIR}/assets")
set(LUCENT_BUNDLED_ASSETS camera-support)

# lucent_asset_files(<id> <files_var> <sha256_var>)
#
# The files of bundled asset <id>, relative to its folder: manifest.json first, then every file the
# manifest lists, in its order; and the SHA-256 of the manifest. Fails the configure step when the
# manifest is missing or lists no files. The manifest becomes a configure dependency, so editing it
# re-pins the build.
function(lucent_asset_files id files_var sha256_var)
    set(manifest "${LUCENT_ASSETS_SOURCE_DIR}/${id}/manifest.json")
    if(NOT EXISTS "${manifest}")
        message(FATAL_ERROR "The bundled asset '${id}' has no manifest (${manifest}).")
    endif()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${manifest}")
    file(READ "${manifest}" text)
    string(JSON count ERROR_VARIABLE json_error LENGTH "${text}" files)
    if(json_error OR NOT count GREATER 0)
        message(FATAL_ERROR "The manifest of the bundled asset '${id}' lists no files (${manifest}).")
    endif()
    set(files manifest.json)
    math(EXPR last "${count} - 1")
    foreach(index RANGE ${last})
        string(JSON path GET "${text}" files ${index} path)
        if(path MATCHES "(^|/)\\.\\.?(/|$)" OR path MATCHES "^/|\\\\|:")
            message(FATAL_ERROR "The manifest of the bundled asset '${id}' lists an unsafe path '${path}'.")
        endif()
        list(APPEND files "${path}")
    endforeach()
    file(SHA256 "${manifest}" sha256)
    set(${files_var} "${files}" PARENT_SCOPE)
    set(${sha256_var} "${sha256}" PARENT_SCOPE)
endfunction()

# dumpbin.exe sits next to the MSVC linker; file(GET_RUNTIME_DEPENDENCIES) and the audit in
# tools/package.ps1 use it.
get_filename_component(_lucent_linker_dir "${CMAKE_LINKER}" DIRECTORY)
find_program(LUCENT_DUMPBIN dumpbin HINTS "${_lucent_linker_dir}" DOC "MSVC dumpbin.exe used for packaging")

# Microsoft's Visual C++ Redistributable (x64), bundled by the installer, and the folder of the
# runtime DLLs it installs (the audit accepts exactly these names as runtime imports). Both come from
# the Visual Studio environment that tools/dev-env.ps1 enters.
if(NOT LUCENT_VC_REDIST)
    foreach(_lucent_candidate
            "$ENV{VCToolsRedistDir}/vc_redist.x64.exe"
            "$ENV{VCINSTALLDIR}/Redist/MSVC/v143/vc_redist.x64.exe")
        file(TO_CMAKE_PATH "${_lucent_candidate}" _lucent_candidate)
        if(EXISTS "${_lucent_candidate}")
            set(LUCENT_VC_REDIST "${_lucent_candidate}" CACHE FILEPATH
                "Microsoft Visual C++ Redistributable (x64) bundled by the installer" FORCE)
            break()
        endif()
    endforeach()
endif()
set(LUCENT_VC_RUNTIME_DLLS "")
if(DEFINED ENV{VCToolsRedistDir})
    file(TO_CMAKE_PATH "$ENV{VCToolsRedistDir}" _lucent_redist_dir)
    # Microsoft.VC143.CRT, .CXXAMP, .OpenMP, ...: the DLLs vc_redist.x64.exe installs into System32.
    file(GLOB _lucent_vc_runtime_files LIST_DIRECTORIES false "${_lucent_redist_dir}/x64/Microsoft.VC*/*.dll")
    foreach(_lucent_file IN LISTS _lucent_vc_runtime_files)
        get_filename_component(_lucent_name "${_lucent_file}" NAME)
        string(TOLOWER "${_lucent_name}" _lucent_name)
        list(APPEND LUCENT_VC_RUNTIME_DLLS "${_lucent_name}")
    endforeach()
    list(REMOVE_DUPLICATES LUCENT_VC_RUNTIME_DLLS)
    list(SORT LUCENT_VC_RUNTIME_DLLS)
endif()
if(LUCENT_INSTALL_RUNTIME AND (NOT LUCENT_VC_REDIST OR NOT LUCENT_VC_RUNTIME_DLLS))
    message(WARNING "The Visual C++ redistributable was not found (VCToolsRedistDir); packaging will fail. "
                    "Configure through tools\\build.ps1 or set LUCENT_VC_REDIST.")
endif()

# QML modules windeployqt deploys for `import QtQuick.Controls` that Lucent never loads: Lucent
# selects the Fusion style in code (apps/lucent/src/shell.cpp, QQuickStyle::setStyle), which falls
# back to Basic, so the other styles, the native style they build on, and the other styles' variants
# of the non-native dialogs are never selected. Paths are relative to the qml folder. The DLLs only
# these modules import are removed with them (step 4).
set(LUCENT_PACKAGE_UNUSED_QML_MODULES
    QtQuick/Controls/FluentWinUI3
    QtQuick/Controls/Imagine
    QtQuick/Controls/Material
    QtQuick/Controls/Universal
    QtQuick/Controls/Windows
    QtQuick/NativeStyle
    QtQuick/Dialogs/quickimpl/qml/+Imagine
    QtQuick/Dialogs/quickimpl/qml/+Material
    QtQuick/Dialogs/quickimpl/qml/+Universal)

# Qt plugin types Lucent does not use: QML debugging (opens a debug server when enabled), TUIO
# network touch input, network reachability and TLS backends (Lucent works offline).
set(LUCENT_PACKAGE_SKIPPED_PLUGIN_TYPES qmltooling generic networkinformation tls)

# Qt plugins Lucent cannot work without, relative to the plugins folder. Qt loads plugins by name
# at run time, so neither the import closure nor the audit's import check would notice a missing
# one: the window needs the Windows platform plugin, and camera previews and thumbnails are decoded
# through QImageReader's JPEG plugin (apps/lucent/src/adapters/conversions.cpp). Deployment and
# the packaging audit both fail when one is missing.
set(LUCENT_PACKAGE_REQUIRED_PLUGINS platforms/qwindows.dll imageformats/qjpeg.dll)

# QML modules Lucent loads without importing them in apps/lucent/qml: the Qt Quick Controls style
# selected in code. The audit requires these and every module the QML sources import, each with
# the plugin DLL its qmldir names and the modules that qmldir imports in turn.
set(LUCENT_PACKAGE_REQUIRED_QML_MODULES QtQuick.Controls.Fusion)

# Executables installed next to the application (W7, ADR 0012): Lucent.exe starts lucent-worker.exe
# from its own folder for every export and batch, and the two must be the same build (the worker
# protocol's handshake refuses a mixed pair). Building the application builds them too, so a build
# tree's Lucent.exe always finds its worker.
set(LUCENT_PACKAGE_HELPER_TARGETS lucent-worker)

# lucent_install_application(<target> QML_SOURCE_DIR <dir> COMPONENT <component>)
#
# Installs <target> and the helper executables (LUCENT_PACKAGE_HELPER_TARGETS) at the root of the
# prefix and, with LUCENT_INSTALL_RUNTIME, deploys their runtime (see the header). Writes
# <build dir>/packaging/lucent-package.json, which tools/package.ps1 reads. Must be called where the
# Qt6 package has been found.
function(lucent_install_application target)
    cmake_parse_arguments(ARG "" "QML_SOURCE_DIR;COMPONENT" "" ${ARGN})
    if(NOT ARG_QML_SOURCE_DIR OR NOT ARG_COMPONENT)
        message(FATAL_ERROR "lucent_install_application: QML_SOURCE_DIR and COMPONENT are required")
    endif()
    if(NOT TARGET Qt6::windeployqt)
        message(FATAL_ERROR "lucent_install_application: Qt6::windeployqt is missing (vcpkg qtbase feature 'windeployqt')")
    endif()

    install(TARGETS ${target} RUNTIME DESTINATION . COMPONENT ${ARG_COMPONENT})
    set(helper_installed "")
    set(helper_names_json "")
    set(helper_built_json "")
    foreach(helper IN LISTS LUCENT_PACKAGE_HELPER_TARGETS)
        if(NOT TARGET ${helper})
            message(FATAL_ERROR "lucent_install_application: the helper executable target ${helper} does not exist")
        endif()
        install(TARGETS ${helper} RUNTIME DESTINATION . COMPONENT ${ARG_COMPONENT})
        add_dependencies(${target} ${helper})
        list(APPEND helper_installed "\${CMAKE_INSTALL_PREFIX}/$<TARGET_FILE_NAME:${helper}>")
        if(helper_names_json)
            string(APPEND helper_names_json ", ")
            string(APPEND helper_built_json ", ")
        endif()
        string(APPEND helper_names_json "\"$<TARGET_FILE_NAME:${helper}>\"")
        string(APPEND helper_built_json "\"$<TARGET_FILE:${helper}>\"")
    endforeach()
    list(JOIN helper_installed "\" \"" helper_installed_text)

    # The bundled data assets (ADR 0018): each manifest and exactly the files it lists, unchanged.
    set(LUCENT_PACKAGE_ASSETS_JSON "")
    foreach(asset IN LISTS LUCENT_BUNDLED_ASSETS)
        lucent_asset_files(${asset} asset_files asset_sha256)
        set(files_json "")
        foreach(file IN LISTS asset_files)
            get_filename_component(subdirectory "${file}" DIRECTORY)
            set(destination "assets/${asset}")
            if(subdirectory)
                string(APPEND destination "/${subdirectory}")
            endif()
            install(FILES "${LUCENT_ASSETS_SOURCE_DIR}/${asset}/${file}"
                DESTINATION "${destination}" COMPONENT ${ARG_COMPONENT})
            if(files_json)
                string(APPEND files_json ", ")
            endif()
            string(APPEND files_json "\"${file}\"")
        endforeach()
        if(LUCENT_PACKAGE_ASSETS_JSON)
            string(APPEND LUCENT_PACKAGE_ASSETS_JSON ", ")
        endif()
        string(APPEND LUCENT_PACKAGE_ASSETS_JSON
            "{\"id\": \"${asset}\", \"manifestSha256\": \"${asset_sha256}\", "
            "\"sourceDir\": \"${LUCENT_ASSETS_SOURCE_DIR}/${asset}\", \"files\": [${files_json}]}")
    endforeach()

    set(qt_bin_dir "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}")
    set(packaging_dir "${CMAKE_BINARY_DIR}/packaging")
    if(LUCENT_INSTALL_RUNTIME)
        if(NOT LUCENT_DUMPBIN)
            message(FATAL_ERROR "LUCENT_INSTALL_RUNTIME needs dumpbin.exe (configure from the MSVC environment)")
        endif()
        install(CODE "
            set(LUCENT_PACKAGING_INSTALL_TIME TRUE)
            include(\"${CMAKE_CURRENT_FUNCTION_LIST_FILE}\")
            lucent_deploy_runtime(
                EXECUTABLE \"\${CMAKE_INSTALL_PREFIX}/$<TARGET_FILE_NAME:${target}>\"
                HELPERS \"${helper_installed_text}\"
                WINDEPLOYQT \"$<TARGET_FILE:Qt6::windeployqt>\"
                DUMPBIN \"${LUCENT_DUMPBIN}\"
                QML_SOURCE_DIR \"${ARG_QML_SOURCE_DIR}\"
                LOG_FILE \"${packaging_dir}/windeployqt.log\"
                SEARCH_DIRECTORIES \"${qt_bin_dir}\"
                SKIPPED_PLUGIN_TYPES ${LUCENT_PACKAGE_SKIPPED_PLUGIN_TYPES}
                REQUIRED_PLUGINS ${LUCENT_PACKAGE_REQUIRED_PLUGINS}
                UNUSED_QML_MODULES ${LUCENT_PACKAGE_UNUSED_QML_MODULES}
                VC_RUNTIME_DLLS ${LUCENT_VC_RUNTIME_DLLS})
            "
            COMPONENT ${ARG_COMPONENT})
    endif()

    # Facts about this build for tools/package.ps1 (forward slashes: no JSON escaping needed).
    set(LUCENT_PACKAGE_TARGET "${target}")
    set(LUCENT_PACKAGE_HELPERS_JSON "${helper_names_json}")
    set(LUCENT_PACKAGE_BUILT_HELPERS_JSON "${helper_built_json}")
    set(LUCENT_PACKAGE_COMPONENT "${ARG_COMPONENT}")
    set(LUCENT_PACKAGE_VCPKG_INSTALLED "${VCPKG_INSTALLED_DIR}")
    set(LUCENT_PACKAGE_TRIPLET "${VCPKG_TARGET_TRIPLET}")
    file(RELATIVE_PATH LUCENT_PACKAGE_QT_BIN "${QT6_INSTALL_PREFIX}" "${qt_bin_dir}")
    set(LUCENT_PACKAGE_QT_PLUGINS "${QT6_INSTALL_PLUGINS}")
    set(LUCENT_PACKAGE_QT_QML "${QT6_INSTALL_QML}")
    if(LUCENT_INSTALL_RUNTIME)
        set(LUCENT_PACKAGE_RUNTIME_DEPLOYED "true")
    else()
        set(LUCENT_PACKAGE_RUNTIME_DEPLOYED "false")
    endif()
    set(LUCENT_PACKAGE_VC_RUNTIME_JSON "")
    foreach(name IN LISTS LUCENT_VC_RUNTIME_DLLS)
        if(LUCENT_PACKAGE_VC_RUNTIME_JSON)
            string(APPEND LUCENT_PACKAGE_VC_RUNTIME_JSON ", ")
        endif()
        string(APPEND LUCENT_PACKAGE_VC_RUNTIME_JSON "\"${name}\"")
    endforeach()
    set(LUCENT_PACKAGE_REQUIRED_PLUGINS_JSON "")
    foreach(name IN LISTS LUCENT_PACKAGE_REQUIRED_PLUGINS)
        if(LUCENT_PACKAGE_REQUIRED_PLUGINS_JSON)
            string(APPEND LUCENT_PACKAGE_REQUIRED_PLUGINS_JSON ", ")
        endif()
        string(APPEND LUCENT_PACKAGE_REQUIRED_PLUGINS_JSON "\"${name}\"")
    endforeach()
    set(LUCENT_PACKAGE_REQUIRED_QML_JSON "")
    foreach(name IN LISTS LUCENT_PACKAGE_REQUIRED_QML_MODULES)
        if(LUCENT_PACKAGE_REQUIRED_QML_JSON)
            string(APPEND LUCENT_PACKAGE_REQUIRED_QML_JSON ", ")
        endif()
        string(APPEND LUCENT_PACKAGE_REQUIRED_QML_JSON "\"${name}\"")
    endforeach()

    # Folders of this build machine that no shipped file may name (schema 5, W12): the user profile
    # and the vcpkg folders this build used. The packaging audit adds the source and build trees.
    # The vcpkg buildtrees and packages folders are where the root CMakeLists.txt points vcpkg
    # (--x-buildtrees-root / --x-packages-root in VCPKG_INSTALL_OPTIONS), or inside the vcpkg root.
    set(LUCENT_PACKAGE_USER_PROFILE "")
    if(DEFINED ENV{USERPROFILE})
        file(TO_CMAKE_PATH "$ENV{USERPROFILE}" LUCENT_PACKAGE_USER_PROFILE)
    endif()
    set(LUCENT_PACKAGE_VCPKG_ROOT "")
    if(Z_VCPKG_ROOT_DIR)
        file(TO_CMAKE_PATH "${Z_VCPKG_ROOT_DIR}" LUCENT_PACKAGE_VCPKG_ROOT)
    endif()
    set(LUCENT_PACKAGE_VCPKG_BUILDTREES "")
    set(LUCENT_PACKAGE_VCPKG_PACKAGES "")
    set(LUCENT_PACKAGE_VCPKG_DOWNLOADS "")
    if(LUCENT_PACKAGE_VCPKG_ROOT)
        set(LUCENT_PACKAGE_VCPKG_BUILDTREES "${LUCENT_PACKAGE_VCPKG_ROOT}/buildtrees")
        set(LUCENT_PACKAGE_VCPKG_PACKAGES "${LUCENT_PACKAGE_VCPKG_ROOT}/packages")
        set(LUCENT_PACKAGE_VCPKG_DOWNLOADS "${LUCENT_PACKAGE_VCPKG_ROOT}/downloads")
    endif()
    # As vcpkg resolves them: the environment's downloads folder, then the command-line options.
    if(DEFINED ENV{VCPKG_DOWNLOADS})
        file(TO_CMAKE_PATH "$ENV{VCPKG_DOWNLOADS}" LUCENT_PACKAGE_VCPKG_DOWNLOADS)
    endif()
    foreach(option IN LISTS VCPKG_INSTALL_OPTIONS)
        if(option MATCHES "^--x-buildtrees-root=(.+)$")
            file(TO_CMAKE_PATH "${CMAKE_MATCH_1}" LUCENT_PACKAGE_VCPKG_BUILDTREES)
        elseif(option MATCHES "^--x-packages-root=(.+)$")
            file(TO_CMAKE_PATH "${CMAKE_MATCH_1}" LUCENT_PACKAGE_VCPKG_PACKAGES)
        elseif(option MATCHES "^--downloads-root=(.+)$")
            file(TO_CMAKE_PATH "${CMAKE_MATCH_1}" LUCENT_PACKAGE_VCPKG_DOWNLOADS)
        endif()
    endforeach()
    foreach(variable USER_PROFILE VCPKG_ROOT VCPKG_BUILDTREES VCPKG_PACKAGES VCPKG_DOWNLOADS)
        if(LUCENT_PACKAGE_${variable} MATCHES "[\"\\\\]")
            message(FATAL_ERROR "lucent_install_application: unexpected character in ${LUCENT_PACKAGE_${variable}}")
        endif()
    endforeach()

    # The update channel compiled into Lucent.exe (src/application/CMakeLists.txt, ADR 0022): recorded
    # for tools/package.ps1, which checks it against the staged Lucent.exe's own report and the
    # expected channels of installer/update-channels.json, and records it in the package manifest
    # (schema 6, W12 review SEC-02). The values were validated where they are defined.
    set(LUCENT_PACKAGE_UPDATE_KEYS_JSON "")
    foreach(key IN LISTS LUCENT_UPDATE_PUBLIC_KEYS)
        string(STRIP "${key}" key)
        string(TOLOWER "${key}" key)
        if(key)
            if(LUCENT_PACKAGE_UPDATE_KEYS_JSON)
                string(APPEND LUCENT_PACKAGE_UPDATE_KEYS_JSON ", ")
            endif()
            string(APPEND LUCENT_PACKAGE_UPDATE_KEYS_JSON "\"${key}\"")
        endif()
    endforeach()
    set(LUCENT_PACKAGE_UPDATE_THUMBPRINTS_JSON "")
    foreach(thumbprint IN LISTS LUCENT_UPDATE_PUBLISHER_THUMBPRINTS)
        string(STRIP "${thumbprint}" thumbprint)
        string(TOLOWER "${thumbprint}" thumbprint)
        if(thumbprint)
            if(LUCENT_PACKAGE_UPDATE_THUMBPRINTS_JSON)
                string(APPEND LUCENT_PACKAGE_UPDATE_THUMBPRINTS_JSON ", ")
            endif()
            string(APPEND LUCENT_PACKAGE_UPDATE_THUMBPRINTS_JSON "\"${thumbprint}\"")
        endif()
    endforeach()
    if(LUCENT_UPDATE_CHANNEL MATCHES "[\"\\\\]" OR LUCENT_UPDATE_MANIFEST_URL MATCHES "[\"\\\\]")
        message(FATAL_ERROR "lucent_install_application: unexpected character in the update channel")
    endif()

    file(CONFIGURE OUTPUT "${packaging_dir}/lucent-package.json.in" CONTENT [[{
  "schema": 6,
  "name": "Lucent",
  "version": "@CMAKE_PROJECT_VERSION@",
  "versionMajor": @CMAKE_PROJECT_VERSION_MAJOR@,
  "versionMinor": @CMAKE_PROJECT_VERSION_MINOR@,
  "versionPatch": @CMAKE_PROJECT_VERSION_PATCH@,
  "description": "@CMAKE_PROJECT_DESCRIPTION@",
  "homepage": "@CMAKE_PROJECT_HOMEPAGE_URL@",
  "gitCommitAtConfigure": "@LUCENT_GIT_COMMIT@",
  "buildType": "@CMAKE_BUILD_TYPE@",
  "sourceDir": "@CMAKE_SOURCE_DIR@",
  "binaryDir": "@CMAKE_BINARY_DIR@",
  "installComponent": "@LUCENT_PACKAGE_COMPONENT@",
  "executable": "$<TARGET_FILE_NAME:@LUCENT_PACKAGE_TARGET@>",
  "builtExecutable": "$<TARGET_FILE:@LUCENT_PACKAGE_TARGET@>",
  "helperExecutables": [@LUCENT_PACKAGE_HELPERS_JSON@],
  "builtHelperExecutables": [@LUCENT_PACKAGE_BUILT_HELPERS_JSON@],
  "runtimeDeployed": @LUCENT_PACKAGE_RUNTIME_DEPLOYED@,
  "qtVersion": "@Qt6_VERSION@",
  "vcpkgInstalledDir": "@LUCENT_PACKAGE_VCPKG_INSTALLED@",
  "vcpkgTriplet": "@LUCENT_PACKAGE_TRIPLET@",
  "userProfile": "@LUCENT_PACKAGE_USER_PROFILE@",
  "vcpkgRoot": "@LUCENT_PACKAGE_VCPKG_ROOT@",
  "vcpkgBuildtreesDir": "@LUCENT_PACKAGE_VCPKG_BUILDTREES@",
  "vcpkgPackagesDir": "@LUCENT_PACKAGE_VCPKG_PACKAGES@",
  "vcpkgDownloadsDir": "@LUCENT_PACKAGE_VCPKG_DOWNLOADS@",
  "qtBinDir": "@LUCENT_PACKAGE_QT_BIN@",
  "qtPluginsDir": "@LUCENT_PACKAGE_QT_PLUGINS@",
  "qtQmlDir": "@LUCENT_PACKAGE_QT_QML@",
  "dumpbin": "@LUCENT_DUMPBIN@",
  "vcRedist": "@LUCENT_VC_REDIST@",
  "vcRuntimeDlls": [@LUCENT_PACKAGE_VC_RUNTIME_JSON@],
  "qmlSourceDir": "@ARG_QML_SOURCE_DIR@",
  "requiredPlugins": [@LUCENT_PACKAGE_REQUIRED_PLUGINS_JSON@],
  "requiredQmlModules": [@LUCENT_PACKAGE_REQUIRED_QML_JSON@],
  "assets": [@LUCENT_PACKAGE_ASSETS_JSON@],
  "updateChannel": {"name": "@LUCENT_UPDATE_CHANNEL@", "manifestUrl": "@LUCENT_UPDATE_MANIFEST_URL@",
                    "publicKeys": [@LUCENT_PACKAGE_UPDATE_KEYS_JSON@],
                    "publisherThumbprints": [@LUCENT_PACKAGE_UPDATE_THUMBPRINTS_JSON@]}
}
]] @ONLY)
    file(GENERATE
        OUTPUT "${packaging_dir}/lucent-package.json"
        INPUT "${packaging_dir}/lucent-package.json.in")
endfunction()
