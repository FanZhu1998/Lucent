# lucent_workers: the versioned worker protocol "lucent.worker" (protocol.hpp), its framed pipe
# channel, the worker command line and test hooks, the worker-side server loop (heartbeats,
# cancellation, leaving with the host) and the host that starts, supervises and restarts isolated
# lucent-worker.exe processes in Job Objects (W7), nested in a group's Job Object whose aggregate
# memory limit covers all of a scheduler's workers (W12, worker_group.hpp). Pure C++ and Win32; no
# Qt. Pixels never cross the protocol, so the library does not depend on the application services:
# the worker's job runner lives in apps/lucent-worker.
#
# Layout
#   include/lucent/workers/   public headers
#   src/                      implementation and private headers (strict JSON reading, Win32 helpers)
#   tests/                    GoogleTest unit tests (in-process server over real pipes; the worker
#                             group's tests start the real lucent-worker.exe)

find_package(nlohmann_json CONFIG REQUIRED)

lucent_add_library(workers
    SOURCES
        include/lucent/workers/protocol.hpp
        include/lucent/workers/channel.hpp
        include/lucent/workers/worker_launch.hpp
        include/lucent/workers/job_output.hpp
        include/lucent/workers/worker_server.hpp
        include/lucent/workers/worker_host.hpp
        include/lucent/workers/worker_group.hpp
        src/strict_json.hpp
        src/strict_json.cpp
        src/protocol.cpp
        src/channel.cpp
        src/worker_launch.cpp
        src/job_output.cpp
        src/worker_server.cpp
        src/worker_host.cpp
        src/worker_group.cpp
        src/win32_util.hpp
        src/win32_util.cpp
    PUBLIC_DEPS
        Lucent::core
        Lucent::raw
        Lucent::export
    PRIVATE_DEPS
        nlohmann_json::nlohmann_json
)

lucent_add_test(lucent_workers_tests
    SOURCES
        tests/protocol_test.cpp
        tests/channel_test.cpp
        tests/worker_launch_test.cpp
        tests/job_output_test.cpp
        tests/worker_server_test.cpp
        tests/worker_group_test.cpp
        tests/workers_test_support.hpp
    DEPS
        Lucent::workers
        nlohmann_json::nlohmann_json  # tests build and mutate raw protocol documents
    LABELS
        workers unit
)
if(TARGET lucent_workers_tests)
    target_include_directories(lucent_workers_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_SOURCE_DIR}/src/raw/tests")
    # The worker-group tests (worker_group_test.cpp) run the real lucent-worker.exe
    # (apps/lucent-worker, configured after this directory: the target is resolved when generating).
    target_compile_definitions(lucent_workers_tests PRIVATE LUCENT_WORKER_EXE="$<TARGET_FILE:lucent-worker>")
    add_dependencies(lucent_workers_tests lucent-worker)
endif()
