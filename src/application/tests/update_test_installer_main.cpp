// A stand-in for Lucent Setup in the update tests (ADR 0022): the update service downloads this
// program from the loopback server, verifies it and starts it as the installer. It writes its own
// command line (UTF-16LE) into the file named by LUCENT_TEST_INSTALLER_MARKER, which the test reads,
// and exits; without that variable it does nothing and exits with 2.

#include <cwchar>
#include <string>

#include <windows.h>

int wmain() {
    wchar_t marker[32768] = {};
    const DWORD length = GetEnvironmentVariableW(L"LUCENT_TEST_INSTALLER_MARKER", marker, 32768);
    if (length == 0 || length >= 32768) {
        return 2;
    }
    // Written under a temporary name and renamed, so the test never reads a half-written file.
    const std::wstring temporary = std::wstring(marker) + L".tmp";
    const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return 3;
    }
    const wchar_t* command_line = GetCommandLineW();
    const DWORD bytes = static_cast<DWORD>(std::wcslen(command_line) * sizeof(wchar_t));
    DWORD written = 0;
    const BOOL wrote = WriteFile(file, command_line, bytes, &written, nullptr);
    CloseHandle(file);
    if (!wrote || written != bytes) {
        return 4;
    }
    return MoveFileExW(temporary.c_str(), marker, MOVEFILE_REPLACE_EXISTING) ? 0 : 5;
}
