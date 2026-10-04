// lucent-worker.exe: Lucent's isolated processing worker (W7; blueprint sections 4 and 7). Started
// only by lucent_workers' WorkerHost with the command line of worker_launch.hpp; it speaks the
// protocol of protocol.hpp over two inherited pipes and runs DevelopJobRunner jobs one at a time.
//
// It is a GUI-subsystem program without a window, so starting it never shows a console. Failures
// never open a dialog: Windows error boxes are disabled, and an unhandled exception, an abort, an
// invalid CRT parameter or heap corruption end the process at once with a distinctive exit code
// (the exception code, or 0xC0000409 / 0xC0000417), which the host reports as a crash. The worker
// ends by itself when its host closes the pipe or exits (worker_server.hpp).

#include <csignal>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include <windows.h>
#include <shellapi.h>

#include <lucent/raw/decoder_registry.hpp>
#include <lucent/workers/channel.hpp>
#include <lucent/workers/worker_launch.hpp>
#include <lucent/workers/worker_server.hpp>

#include "develop_job.hpp"

namespace {

constexpr UINT kAbortExitCode = 0xC0000409;            // STATUS_STACK_BUFFER_OVERRUN (fail fast)
constexpr UINT kInvalidParameterExitCode = 0xC0000417;  // STATUS_INVALID_CRUNTIME_PARAMETER

LONG WINAPI end_on_unhandled_exception(EXCEPTION_POINTERS* info) {
    const DWORD code = info != nullptr && info->ExceptionRecord != nullptr ? info->ExceptionRecord->ExceptionCode
                                                                          : kAbortExitCode;
    ::TerminateProcess(::GetCurrentProcess(), code);
    return EXCEPTION_EXECUTE_HANDLER;
}

void end_on_abort(int) { ::TerminateProcess(::GetCurrentProcess(), kAbortExitCode); }

void end_on_invalid_parameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
    ::TerminateProcess(::GetCurrentProcess(), kInvalidParameterExitCode);
}

void harden_process() {
    ::SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    ::SetUnhandledExceptionFilter(end_on_unhandled_exception);
    ::HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    std::signal(SIGABRT, end_on_abort);
    _set_invalid_parameter_handler(end_on_invalid_parameter);
}

std::vector<std::wstring> arguments() {
    std::vector<std::wstring> out;
    int count = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &count);
    if (argv == nullptr) {
        return out;
    }
    for (int i = 1; i < count; ++i) {
        out.emplace_back(argv[i]);
    }
    ::LocalFree(argv);
    return out;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    using lucent::workers::WorkerExitCode;
    harden_process();

    auto launch = lucent::workers::parse_worker_arguments(arguments());
    if (!launch) {
        return static_cast<int>(WorkerExitCode::Usage);
    }
    lucent::workers::MessageChannel channel(reinterpret_cast<HANDLE>(launch->request_handle),
                                            reinterpret_cast<HANDLE>(launch->response_handle),
                                            lucent::workers::Direction::HostToWorker);
    auto registry = lucent::raw::create_default_registry();
    if (!registry) {
        return static_cast<int>(WorkerExitCode::Internal);
    }
    lucent::worker_app::DevelopJobRunner runner(
        std::make_shared<const lucent::raw::DecoderRegistry>(std::move(*registry)));

    lucent::workers::WorkerServerOptions options;
    options.hooks = launch->hooks;
    options.parent_process = reinterpret_cast<HANDLE>(launch->parent_handle);
    const WorkerExitCode code = lucent::workers::run_worker_server(channel, runner, options);
    if (options.parent_process != nullptr) {
        ::CloseHandle(options.parent_process);
    }
    return static_cast<int>(code);
}
