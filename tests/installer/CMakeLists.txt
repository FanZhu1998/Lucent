# Self-tests of the installer's scripts that need no build (W12; label "installer"):
#
#   lucent_release_manifest_tests  installer\scripts\LucentUpdate.psm1, the update channel's release
#                                  tooling (signing keys, release manifests and their signatures; ADR
#                                  0022), with an ephemeral key: no key store is touched
#   lucent_builder_path_scan_tests installer\scripts\LucentPackaging.psm1's scan for build-machine paths
#                                  in shipped files (ADR 0009, decision 12)
#
# Run just these with:  ctest -L installer

find_program(LUCENT_WINDOWS_POWERSHELL powershell.exe REQUIRED)

add_test(NAME lucent_release_manifest_tests
    COMMAND "${LUCENT_WINDOWS_POWERSHELL}" -NoProfile -NonInteractive -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/installer/tests/Test-ReleaseManifest.ps1")
set_tests_properties(lucent_release_manifest_tests PROPERTIES LABELS "installer;update" TIMEOUT 120)

add_test(NAME lucent_builder_path_scan_tests
    COMMAND "${LUCENT_WINDOWS_POWERSHELL}" -NoProfile -NonInteractive -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/installer/tests/Test-BuilderPathScan.ps1")
set_tests_properties(lucent_builder_path_scan_tests PROPERTIES LABELS "installer;tools" TIMEOUT 120)
