# Self-tests of the installer's scripts that need no build (W12; label "installer"):
#
#   lucent_release_manifest_tests  installer\scripts\LucentUpdate.psm1, the update channel's release
#                                  tooling (signing keys, release manifests and their signatures; ADR
#                                  0022), with an ephemeral key: no key store is touched
#   lucent_builder_path_scan_tests installer\scripts\LucentPackaging.psm1's scan for build-machine paths
#                                  in shipped files (ADR 0009, decision 12) and in what they compress:
#                                  zlib and gzip streams, Inno Setup's LZMA parts (W12 review SEC-07)
#   lucent_update_channel_policy_tests how tools\package.ps1 reads, compares and enforces the update
#                                  channel a package's Lucent.exe checks (W12 review SEC-02)
#   lucent_source_package_tests    the corresponding-source package of the LGPL libraries (W12, ADR
#                                  0024): the deterministic archive, the copyleft guard and the
#                                  independent check of an archive's contents, on synthetic data
#   lucent_clean_machine_harness_tests the clean-machine gate's harness (installer/sandbox,
#                                  tools/Test-CleanMachine.ps1): the Windows Sandbox configuration against
#                                  the documented schema and the gate's rules, the scripts' syntax, and
#                                  the host's check of a gate result; no Windows Sandbox needed
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

add_test(NAME lucent_update_channel_policy_tests
    COMMAND "${LUCENT_WINDOWS_POWERSHELL}" -NoProfile -NonInteractive -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/installer/tests/Test-UpdateChannelPolicy.ps1")
set_tests_properties(lucent_update_channel_policy_tests PROPERTIES LABELS "installer;update" TIMEOUT 120)

add_test(NAME lucent_source_package_tests
    COMMAND "${LUCENT_WINDOWS_POWERSHELL}" -NoProfile -NonInteractive -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/installer/tests/Test-SourcePackage.ps1")
set_tests_properties(lucent_source_package_tests PROPERTIES LABELS "installer;licences" TIMEOUT 120)

add_test(NAME lucent_clean_machine_harness_tests
    COMMAND "${LUCENT_WINDOWS_POWERSHELL}" -NoProfile -NonInteractive -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/installer/tests/Test-CleanMachineHarness.ps1")
set_tests_properties(lucent_clean_machine_harness_tests PROPERTIES LABELS "installer" TIMEOUT 120)
