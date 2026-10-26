#include "adb.h"

#include <windows.h>

#include <vector>

namespace {

bool FileExists(const std::wstring& path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring path(buf, n);
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

// Looks next to the exe first (the release zip bundles platform-tools), then the Android SDK,
// then PATH.
std::wstring FindAdb() {
    const std::wstring dir = ExeDir();
    for (const std::wstring& p : {dir + L"\\platform-tools\\adb.exe", dir + L"\\adb.exe"}) {
        if (FileExists(p)) return p;
    }
    wchar_t local[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        std::wstring sdk = std::wstring(local) + L"\\Android\\Sdk\\platform-tools\\adb.exe";
        if (FileExists(sdk)) return sdk;
    }
    wchar_t found[MAX_PATH];
    if (SearchPathW(nullptr, L"adb.exe", nullptr, MAX_PATH, found, nullptr) > 0) return found;
    return L"";
}

}  // namespace

bool AdbForward(int port, std::wstring* error) {
    const std::wstring adb = FindAdb();
    if (adb.empty()) {
        *error = L"adb.exe not found. Put platform-tools next to fyewebcam.exe or install the Android SDK.";
        return false;
    }
    std::wstring cmd = L"\"" + adb + L"\" forward tcp:" + std::to_wstring(port) + L" tcp:" + std::to_wstring(port);
    std::vector<wchar_t> cmdline(cmd.begin(), cmd.end());
    cmdline.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si,
                        &pi)) {
        *error = L"Could not start adb.exe.";
        return false;
    }
    // The first run also starts the adb server, which can take a few seconds.
    DWORD wait = WaitForSingleObject(pi.hProcess, 20000);
    DWORD code = 1;
    if (wait == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    if (wait != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (code != 0) {
        *error = L"adb could not reach the phone. Plug it in and allow USB debugging.";
        return false;
    }
    return true;
}
