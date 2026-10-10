# Compiler settings applied to every Lucent-owned target by linking Lucent::compiler_options
# (done automatically by the helpers in LucentTargets.cmake). Third-party headers are consumed
# as external includes so their warnings never break the build.

add_library(lucent_compiler_options INTERFACE)
add_library(Lucent::compiler_options ALIAS lucent_compiler_options)

target_compile_features(lucent_compiler_options INTERFACE cxx_std_20)

if(MSVC)
    target_compile_options(lucent_compiler_options INTERFACE
        /W4
        /permissive-
        /utf-8
        /Zc:__cplusplus
        /Zc:preprocessor
        /Zc:inline
        /Zc:throwingNew
        /EHsc
        /bigobj
        /external:anglebrackets
        /external:W0
        # Selected off-by-default warnings that catch real defects (conversions,
        # non-virtual destructors, suspicious expressions, signed/unsigned mistakes).
        /w14242 /w14254 /w14263 /w14265 /w14287 /w14296 /w14311 /w14545 /w14546
        /w14547 /w14549 /w14555 /w14619 /w14640 /w14826 /w14905 /w14906 /w14928
        $<$<BOOL:${LUCENT_WARNINGS_AS_ERRORS}>:/WX>
    )
    target_compile_definitions(lucent_compiler_options INTERFACE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        UNICODE
        _UNICODE
        _WIN32_WINNT=0x0A00
        WINVER=0x0A00
    )
    target_link_options(lucent_compiler_options INTERFACE
        $<$<NOT:$<CONFIG:Debug>>:/OPT:REF /OPT:ICF>
    )

    # No path of the build machine in the release binaries Lucent ships (W12; ADR 0002 and ADR 0009;
    # the same options as cmake/triplets/x64-windows-lucent.cmake gives the vcpkg ports). Set for the
    # whole project rather than through Lucent::compiler_options so that the helper targets Qt
    # generates for Lucent.exe (QML cache and resource object libraries) get them too. Release only:
    # developer builds keep full paths for debuggers and test failure messages.
    # - /PDBALTPATH:%_PDB%: only the PDB's file name in the debug directory. The release preset links
    #   without /DEBUG today (no debug directory entry at all); this keeps it so if /DEBUG is added.
    # - /d1trimfile:<folder>/ (undocumented, honoured by MSVC 19.44; diagnostics and debug information
    #   keep full paths): __FILE__ and std::source_location relative to the vcpkg installed tree, the
    #   build tree or the source tree, the most specific first (the first matching prefix wins).
    #   C and C++ only: the resource compiler does not know the option.
    # tools\package.ps1 fails when any shipped file still names one of these folders.
    foreach(_lucent_folder IN ITEMS "${VCPKG_INSTALLED_DIR}" "${CMAKE_BINARY_DIR}" "${CMAKE_SOURCE_DIR}")
        if(_lucent_folder)
            file(TO_CMAKE_PATH "${_lucent_folder}" _lucent_folder)
            string(REGEX REPLACE "/+$" "" _lucent_folder "${_lucent_folder}")
            string(REPLACE "," "$<COMMA>" _lucent_folder "${_lucent_folder}")
            add_compile_options("$<$<AND:$<CONFIG:Release>,$<COMPILE_LANGUAGE:C,CXX>>:/d1trimfile:${_lucent_folder}/>")
        endif()
    endforeach()
    add_link_options("$<$<CONFIG:Release>:/PDBALTPATH:%_PDB%>")
    unset(_lucent_folder)
endif()
