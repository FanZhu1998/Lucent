# Lucent's logo in the build (ADR 0025). The branding asset assets/branding holds one master and the
# derivatives tools/branding/Update-Branding.ps1 makes from it; branding.json ("lucent.branding" v1)
# names each derivative with its role. The build finds files by role only, never by name or size, so a
# new logo (a new master, regenerated) or a new size needs no change here or in any consumer.
#
#   LUCENT_BRANDING_DIR        assets/branding
#   LUCENT_BRANDING_VERSION    the asset's version (manifest.json)
#   lucent_branding_files(<role> <out_var>)
#                              the absolute paths of the derivatives of a role, in branding.json's order:
#                                windows-icon   the Windows icon (.ico) of the shipped programs and Setup
#                                app-icon       the PNGs of Lucent.exe's window icon and About logo
#                                               (apps/lucent/CMakeLists.txt compiles them in)
#                              (setup-wizard-image and setup-wizard-small-image are used by
#                              tools/package.ps1, docs-logo by README.md)
#   lucent_add_program_resources(<target> DESCRIPTION <text>)
#                              the Windows resources of a shipped program (Lucent.exe,
#                              lucent-worker.exe), from the one template cmake/lucent-program.rc.in:
#                              the icon IDI_ICON1 (the windows-icon derivative; Explorer, the taskbar
#                              and shortcuts show it, and Qt's Windows platform plugin loads it by this
#                              name for the window class) and the version information. The icon is a
#                              dependency of the resource object, so a new icon rebuilds the program.
#                              The script is UTF-8 and says so to RC (#pragma code_page(65001)), so the
#                              icon's absolute path may hold any character; the target property
#                              LUCENT_RESOURCE_SCRIPT names it (CTest's
#                              lucent_program_resource_script_tests compiles it with the icon under a
#                              folder whose name is not ASCII).
#
# The manifest must be "lucent.asset-manifest" v1 for the asset "branding" with content "lucent.branding"
# v1, and branding.json "lucent.branding" v1: another schema or version stops the configure step (a
# newer branding asset needs a newer build). Every derivative the build can use must have the size and
# SHA-256 the manifest lists (a derivative edited by hand stops the configure step; CTest's
# lucent_branding_tests checks that each one is what the generator makes from the master). The two
# documents and the derivatives are configure dependencies, so regenerating the asset reconfigures.

include_guard(GLOBAL)

set(LUCENT_BRANDING_DIR "${CMAKE_SOURCE_DIR}/assets/branding")

# The version information of every shipped program (lucent-program.rc.in).
set(LUCENT_RC_COMPANY "Lucent")
set(LUCENT_RC_COPYRIGHT "Copyright (c) 2026 Fan Zhu. MIT License.")

function(_lucent_branding_read)
    set(manifest "${LUCENT_BRANDING_DIR}/manifest.json")
    set(document "${LUCENT_BRANDING_DIR}/branding.json")
    foreach(file IN ITEMS "${manifest}" "${document}")
        if(NOT EXISTS "${file}")
            message(FATAL_ERROR "The branding asset is incomplete: ${file} is missing (tools/branding/Update-Branding.ps1 makes it).")
        endif()
    endforeach()
    set(configure_depends "${manifest}" "${document}")

    file(READ "${manifest}" text)
    string(JSON schema ERROR_VARIABLE error GET "${text}" schema)
    string(JSON schema_version ERROR_VARIABLE error GET "${text}" schema_version)
    string(JSON id ERROR_VARIABLE error GET "${text}" id)
    string(JSON content_schema ERROR_VARIABLE error GET "${text}" content schema)
    string(JSON content_version ERROR_VARIABLE error GET "${text}" content schema_version)
    string(JSON version ERROR_VARIABLE error GET "${text}" version)
    if(error OR NOT schema STREQUAL "lucent.asset-manifest" OR NOT schema_version EQUAL 1 OR NOT id STREQUAL "branding"
       OR NOT content_schema STREQUAL "lucent.branding" OR NOT content_version EQUAL 1)
        message(FATAL_ERROR "${manifest} is '${schema}' v${schema_version} for '${id}' with content '${content_schema}' "
                            "v${content_version}; this build reads 'lucent.asset-manifest' v1 for 'branding' with "
                            "'lucent.branding' v1 only (ADR 0025).")
    endif()
    # Size and SHA-256 of every listed file, by path.
    string(JSON file_count ERROR_VARIABLE error LENGTH "${text}" files)
    if(error OR NOT file_count GREATER 0)
        message(FATAL_ERROR "${manifest} lists no files.")
    endif()
    math(EXPR last "${file_count} - 1")
    foreach(index RANGE ${last})
        string(JSON listed_path GET "${text}" files ${index} path)
        string(JSON listed_size GET "${text}" files ${index} size)
        string(JSON listed_sha256 GET "${text}" files ${index} sha256)
        string(SHA1 key "${listed_path}")
        set(listed_size_${key} "${listed_size}")
        set(listed_sha256_${key} "${listed_sha256}")
    endforeach()

    file(READ "${document}" text)
    string(JSON schema ERROR_VARIABLE error GET "${text}" schema)
    string(JSON schema_version ERROR_VARIABLE error GET "${text}" schema_version)
    if(error OR NOT schema STREQUAL "lucent.branding" OR NOT schema_version EQUAL 1)
        message(FATAL_ERROR "${document} is '${schema}' v${schema_version}; this build reads 'lucent.branding' v1 only (ADR 0025).")
    endif()
    string(JSON count ERROR_VARIABLE error LENGTH "${text}" derivatives)
    if(error OR NOT count GREATER 0)
        message(FATAL_ERROR "${document} names no derivatives.")
    endif()
    set(roles "")
    math(EXPR last "${count} - 1")
    foreach(index RANGE ${last})
        string(JSON path GET "${text}" derivatives ${index} path)
        string(JSON role GET "${text}" derivatives ${index} role)
        if(path MATCHES "(^|/)\\.\\.?(/|$)" OR path MATCHES "^/|\\\\|:")
            message(FATAL_ERROR "${document} names an unsafe path '${path}'.")
        endif()
        set(full "${LUCENT_BRANDING_DIR}/${path}")
        string(SHA1 key "${path}")
        if(NOT DEFINED listed_sha256_${key})
            message(FATAL_ERROR "${document} names ${path}, which ${manifest} does not list.")
        endif()
        if(NOT EXISTS "${full}")
            message(FATAL_ERROR "The branding asset is incomplete: ${full} is missing.")
        endif()
        file(SIZE "${full}" size)
        file(SHA256 "${full}" sha256)
        if(NOT size EQUAL listed_size_${key} OR NOT sha256 STREQUAL listed_sha256_${key})
            message(FATAL_ERROR "${full} is not the file ${manifest} lists (${size} bytes, SHA-256 ${sha256}; listed "
                                "${listed_size_${key}} bytes, ${listed_sha256_${key}}). Derivatives are never edited by "
                                "hand: regenerate them with tools/branding/Update-Branding.ps1 (assets/branding/README.md).")
        endif()
        list(APPEND configure_depends "${full}")
        set_property(GLOBAL APPEND PROPERTY "LUCENT_BRANDING_FILES_${role}" "${full}")
        list(APPEND roles "${role}")
    endforeach()
    list(REMOVE_DUPLICATES roles)
    set_property(GLOBAL PROPERTY LUCENT_BRANDING_ROLES "${roles}")
    set_property(DIRECTORY "${CMAKE_SOURCE_DIR}" APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${configure_depends})
    set(LUCENT_BRANDING_VERSION "${version}" PARENT_SCOPE)
endfunction()

_lucent_branding_read()
message(STATUS "Branding: assets/branding ${LUCENT_BRANDING_VERSION}")

# lucent_branding_files(<role> <out_var>)
function(lucent_branding_files role out_var)
    get_property(files GLOBAL PROPERTY "LUCENT_BRANDING_FILES_${role}")
    if(NOT files)
        message(FATAL_ERROR "The branding asset has no derivative with the role '${role}'.")
    endif()
    set(${out_var} "${files}" PARENT_SCOPE)
endfunction()

# lucent_add_program_resources(<target> DESCRIPTION <text>)
function(lucent_add_program_resources target)
    cmake_parse_arguments(ARG "" "DESCRIPTION" "" ${ARGN})
    if(NOT ARG_DESCRIPTION)
        message(FATAL_ERROR "lucent_add_program_resources: DESCRIPTION is required")
    endif()
    get_target_property(type ${target} TYPE)
    if(NOT type STREQUAL "EXECUTABLE")
        message(FATAL_ERROR "lucent_add_program_resources: ${target} is not an executable")
    endif()
    lucent_branding_files(windows-icon icons)
    list(LENGTH icons icon_count)
    if(NOT icon_count EQUAL 1)
        message(FATAL_ERROR "The branding asset must have exactly one windows-icon derivative, not ${icon_count}.")
    endif()
    # The program's file name: OUTPUT_NAME when the target sets one (set it before calling this).
    get_target_property(output_name ${target} OUTPUT_NAME)
    if(NOT output_name)
        set(output_name "${target}")
    endif()
    set(LUCENT_RC_ICON "${icons}")
    set(LUCENT_RC_DESCRIPTION "${ARG_DESCRIPTION}")
    set(LUCENT_RC_INTERNAL_NAME "${output_name}")
    set(LUCENT_RC_ORIGINAL_FILENAME "${output_name}.exe")
    foreach(value IN ITEMS "${LUCENT_RC_ICON}" "${LUCENT_RC_DESCRIPTION}" "${LUCENT_RC_INTERNAL_NAME}")
        if(value MATCHES "[\"\\\\]")
            message(FATAL_ERROR "lucent_add_program_resources: '${value}' cannot be written into a resource script")
        endif()
    endforeach()
    set(rc "${CMAKE_CURRENT_BINARY_DIR}/generated/${output_name}.rc")
    configure_file("${CMAKE_SOURCE_DIR}/cmake/lucent-program.rc.in" "${rc}" @ONLY)
    target_sources(${target} PRIVATE "${rc}")
    set_target_properties(${target} PROPERTIES LUCENT_RESOURCE_SCRIPT "${rc}")
    # RC's dependency scan sees #include files only, not the icon an ICON statement names.
    set_source_files_properties("${rc}" PROPERTIES OBJECT_DEPENDS "${icons}")
endfunction()
