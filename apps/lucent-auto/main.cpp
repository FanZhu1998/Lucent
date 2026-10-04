// lucent-auto: Natural v1 automatic settings with related-shot consistency over RAW files and
// folders, writing per-item JSON, previews, an evaluation sheet and a blind preference sheet
// (developer tool, not shipped; see auto_tool.hpp for the usage and the output layout).
//
// Ctrl+C / Ctrl+Break cancel cooperatively (the outputs written so far stay; nothing is
// overwritten). Exit status: 0 ok, 1 usage error, 2 failure.

#include <atomic>
#include <cstdio>
#include <exception>
#include <new>
#include <string>
#include <variant>
#include <vector>

#include <windows.h>

#include <lucent/core/cancellation.hpp>

#include "auto_tool.hpp"

namespace {

std::atomic<lucent::core::CancellationSource*> g_cancel{nullptr};

BOOL WINAPI on_console_event(DWORD event) {
    lucent::core::CancellationSource* source = g_cancel.load();
    if (source == nullptr) {
        return FALSE;
    }
    switch (event) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
            source->cancel();
            return TRUE;
        default:
            return FALSE;
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    try {
        std::vector<std::wstring> args;
        for (int i = 1; i < argc; ++i) {
            args.emplace_back(argv[i]);
        }
        auto parsed = lucent::auto_tool::parse_arguments(args);
        if (auto* message = std::get_if<std::string>(&parsed)) {
            std::fprintf(stderr, "lucent-auto: %s\n%s", message->c_str(), lucent::auto_tool::usage().c_str());
            return 1;
        }
        const auto& arguments = std::get<lucent::auto_tool::Arguments>(parsed);
        if (arguments.help) {
            std::fputs(lucent::auto_tool::usage().c_str(), stdout);
            return 0;
        }
        // Never destroyed: the console handler may still read it after run() returns.
        auto* cancel = new lucent::core::CancellationSource();
        g_cancel.store(cancel);
        ::SetConsoleCtrlHandler(on_console_event, TRUE);
        const int status = lucent::auto_tool::run(arguments, cancel->token());
        ::SetConsoleCtrlHandler(on_console_event, FALSE);
        return status;
    } catch (const std::bad_alloc&) {
        std::fputs("lucent-auto: out of memory\n", stderr);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "lucent-auto: unexpected exception: %s\n", e.what());
    } catch (...) {
        std::fputs("lucent-auto: unexpected exception\n", stderr);
    }
    return 2;
}
