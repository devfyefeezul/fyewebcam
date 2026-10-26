// Small append-only log for the camera DLL, which runs inside the Frame Server service where
// there's no console. Written to C:\ProgramData\fyewebcam\vcam.log (the installer makes that
// folder writable for the service), capped at about 1 MB.
#pragma once

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace vlog {

inline void Write(const char* format, ...) {
    static std::mutex lock;
    std::lock_guard<std::mutex> guard(lock);

    char path[MAX_PATH];
    DWORD n = GetEnvironmentVariableA("ProgramData", path, MAX_PATH);
    if (n == 0 || n > MAX_PATH - 32) return;
    strcat_s(path, "\\fyewebcam\\vcam.log");

    FILE* f = nullptr;
    if (fopen_s(&f, path, "a") != 0 || !f) return;
    fseek(f, 0, SEEK_END);
    if (ftell(f) > (1 << 20)) {
        fclose(f);
        if (fopen_s(&f, path, "w") != 0 || !f) return;
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(f, "%02d:%02d:%02d.%03d [%lu] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentProcessId());
    va_list args;
    va_start(args, format);
    vfprintf(f, format, args);
    va_end(args);
    fputc('\n', f);
    fclose(f);
}

}  // namespace vlog
