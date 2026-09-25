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
endif()
