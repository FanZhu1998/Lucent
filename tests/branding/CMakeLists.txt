# Lucent's branding (ADR 0025; label "branding"):
#
#   lucent_branding_tests      Test-Branding.ps1: the committed asset (assets/branding) is what
#                              tools/branding/Update-Branding.ps1 makes from the master (its check mode),
#                              the Windows icon and the Setup wizard images have the forms Windows and Inno
#                              Setup take, generating from scratch reproduces every file byte for byte,
#                              the generator finds tampering and refuses what it must (copies of the asset,
#                              synthetic masters), and the contact sheet for review draws; no build needed
#   lucent_program_icon_tests  Test-ProgramIcons.ps1: the built lucent-worker.exe, and Lucent.exe in a
#                              build with the user interface, carry the logo as their only icon group,
#                              IDI_ICON1, image for image, and Lucent's version information; the sheet of
#                              their icons for review draws
#   lucent_setup_branding_tests Test-SetupBranding.ps1: installer/lucent.iss compiled by Inno Setup
#                              with the branding defines tools/package.ps1 passes (and stand-ins for the
#                              installation folder) makes a Setup whose only icon group is the logo and
#                              whose setup data holds every wizard image; without the defines it does not
#                              compile. Skipped (exit code 77) where no ISCC.exe is found
#   lucent_program_resource_script_tests
#                              program_resources_test.cmake: lucent-worker's generated resource script
#                              compiles with its icon under a folder whose name is not ASCII, as in a
#                              checkout under such a folder (the script is UTF-8 and says so to RC;
#                              review finding BRD-07)
#
# The application's own check (the window icon has every size, the About dialog's logo loads, on whole
# device pixels and lined up with the text, also at 125 and 175 %) is in lucent_ui_tests
# (apps/lucent/tests/qml_shell_tests.cpp, apps/lucent/tests/CMakeLists.txt); the manifest's format is
# checked against the asset manifest reader in lucent_assets_tests; tools/package.ps1 checks the icons of
# the packaged programs and of the real Setup.
#
# Run just these with:  ctest -L branding

find_program(LUCENT_WINDOWS_POWERSHELL powershell.exe REQUIRED)

add_test(NAME lucent_branding_tests
    COMMAND "${LUCENT_WINDOWS_POWERSHELL}" -NoProfile -NonInteractive -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/Test-Branding.ps1")
set_tests_properties(lucent_branding_tests PROPERTIES LABELS "branding" TIMEOUT 600)

set(_lucent_program_icon_arguments -Worker "$<TARGET_FILE:lucent-worker>" -Version "${PROJECT_VERSION}")
if(TARGET Lucent)
    list(APPEND _lucent_program_icon_arguments -Application "$<TARGET_FILE:Lucent>")
endif()
add_test(NAME lucent_program_icon_tests
    COMMAND "${LUCENT_WINDOWS_POWERSHELL}" -NoProfile -NonInteractive -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/Test-ProgramIcons.ps1" ${_lucent_program_icon_arguments})
set_tests_properties(lucent_program_icon_tests PROPERTIES LABELS "branding" TIMEOUT 120)

add_test(NAME lucent_setup_branding_tests
    COMMAND "${LUCENT_WINDOWS_POWERSHELL}" -NoProfile -NonInteractive -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/Test-SetupBranding.ps1")
set_tests_properties(lucent_setup_branding_tests PROPERTIES LABELS "branding;installer" TIMEOUT 300 SKIP_RETURN_CODE 77)

if(CMAKE_RC_COMPILER)
    lucent_branding_files(windows-icon _lucent_windows_icon)
    # The configure step's INCLUDE (the developer environment's), so that RC finds <winver.h> also when
    # CTest runs outside that environment.
    string(REPLACE ";" "|" _lucent_rc_include "$ENV{INCLUDE}")
    add_test(NAME lucent_program_resource_script_tests
        COMMAND "${CMAKE_COMMAND}"
                "-DRC_COMPILER=${CMAKE_RC_COMPILER}"
                "-DSCRIPT=$<TARGET_PROPERTY:lucent-worker,LUCENT_RESOURCE_SCRIPT>"
                "-DICON=${_lucent_windows_icon}"
                "-DINCLUDE_DIRS=${_lucent_rc_include}"
                "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/resource-script"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/program_resources_test.cmake")
    set_tests_properties(lucent_program_resource_script_tests PROPERTIES LABELS "branding" TIMEOUT 60)
endif()
