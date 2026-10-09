# The performance harness's own tests (W12, ADR 0021; tools/bench, docs/performance). Label "bench".
#
# Test-LucentBench.ps1 checks the harness's shared functions without a build or fixtures: command
# lines that round-trip through CommandLineToArgvW, the statistics, the parsers of Lucent.exe's
# "measure:" lines (apps/lucent/src/developer_automation.hpp), the batch report check and the
# process runner with its sampler (on cmd.exe and ping.exe).
#
# Run just these with:  ctest -L bench

find_program(LUCENT_WINDOWS_POWERSHELL powershell.exe REQUIRED)

add_test(NAME lucent_bench_harness_tests
    COMMAND "${LUCENT_WINDOWS_POWERSHELL}" -NoProfile -NonInteractive -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/Test-LucentBench.ps1")
set_tests_properties(lucent_bench_harness_tests PROPERTIES LABELS "bench;tools" TIMEOUT 300)
