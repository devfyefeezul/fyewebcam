#include "vcam.h"

#include <mfapi.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cstring>

#include "shared_frame.h"

#if defined(_MSC_VER) && __has_include(<mfvirtualcamera.h>)
#include <mfvirtualcamera.h>
#define FYE_HAVE_VCAM 1
#endif

namespace vcam {

namespace {

#ifdef FYE_HAVE_VCAM
const wchar_t kFriendlyName[] = L"fyewebcam";
const wchar_t kDllName[] = L"fyewebcam_vcam.dll";

std::wstring ExeDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring path(buf, n);
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

// The Frame Server runs as LocalService and can't read the user's profile, so the DLL lives
// under Program Files.
std::wstring InstallDir() {
    PWSTR folder = nullptr;
    std::wstring dir = L"C:\\Program Files";
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramFiles, 0, nullptr, &folder))) dir = folder;
    CoTaskMemFree(folder);
    return dir + L"\\fyewebcam";
}

std::wstring HrText(const wchar_t* what, HRESULT hr) {
    wchar_t buf[160];
    swprintf(buf, 160, L"%ls failed (0x%08lX).", what, static_cast<unsigned long>(hr));
    return buf;
}

using RegisterFn = HRESULT(STDAPICALLTYPE*)();

HRESULT CallDllExport(const std::wstring& dll, const char* name) {
    HMODULE module = LoadLibraryW(dll.c_str());
    if (!module) return HRESULT_FROM_WIN32(GetLastError());
    auto fn = reinterpret_cast<RegisterFn>(reinterpret_cast<void*>(GetProcAddress(module, name)));
    HRESULT hr = fn ? fn() : HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    FreeLibrary(module);
    return hr;
}

using CreateFn = HRESULT(STDAPICALLTYPE*)(MFVirtualCameraType, MFVirtualCameraLifetime, MFVirtualCameraAccess,
                                           LPCWSTR, LPCWSTR, const GUID*, ULONG, IMFVirtualCamera**);

// Loaded at run time so the app still starts on Windows 10.
HRESULT CreateCamera(IMFVirtualCamera** camera) {
    HMODULE module = LoadLibraryW(L"mfsensorgroup.dll");
    if (!module) return HRESULT_FROM_WIN32(GetLastError());
    auto create = reinterpret_cast<CreateFn>(reinterpret_cast<void*>(GetProcAddress(module, "MFCreateVirtualCamera")));
    if (!create) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    return create(MFVirtualCameraType_SoftwareCameraSource, MFVirtualCameraLifetime_System,
                  MFVirtualCameraAccess_AllUsers, kFriendlyName, shared_frame::kSourceClsidString, nullptr, 0, camera);
}
#endif

}  // namespace

bool Supported() {
#ifdef FYE_HAVE_VCAM
    return GetModuleHandleW(L"mfsensorgroup.dll") != nullptr || LoadLibraryW(L"mfsensorgroup.dll") != nullptr;
#else
    return false;
#endif
}

bool Installed() {
    std::wstring key = std::wstring(L"Software\\Classes\\CLSID\\") + shared_frame::kSourceClsidString;
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, key.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
    RegCloseKey(h);
    return true;
}

bool Install(std::wstring* error) {
#ifdef FYE_HAVE_VCAM
    const std::wstring dir = InstallDir();
    const std::wstring dst = dir + L"\\" + kDllName;
    const std::wstring src = ExeDir() + L"\\" + kDllName;
    CreateDirectoryW(dir.c_str(), nullptr);
    if (_wcsicmp(src.c_str(), dst.c_str()) != 0 && !CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
        // The Frame Server may have the old DLL loaded: move it aside and copy again.
        std::wstring old = dst + L".old";
        DeleteFileW(old.c_str());
        MoveFileExW(dst.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
        MoveFileExW(old.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
            *error = L"Could not copy " + std::wstring(kDllName) + L" to " + dir + L".";
            return false;
        }
    }
    HRESULT hr = CallDllExport(dst, "DllRegisterServer");
    if (FAILED(hr)) {
        *error = HrText(L"Registering the camera", hr);
        return false;
    }

    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    IMFVirtualCamera* camera = nullptr;
    hr = CreateCamera(&camera);
    if (SUCCEEDED(hr)) hr = camera->Start(nullptr);
    if (camera) camera->Release();
    MFShutdown();
    if (FAILED(hr)) {
        *error = HrText(L"Creating the virtual camera", hr);
        return false;
    }
    return true;
#else
    *error = L"This build has no virtual webcam support (needs the Windows 11 SDK).";
    return false;
#endif
}

bool Uninstall(std::wstring* error) {
#ifdef FYE_HAVE_VCAM
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    IMFVirtualCamera* camera = nullptr;
    HRESULT hr = CreateCamera(&camera);
    if (SUCCEEDED(hr)) hr = camera->Remove();
    if (camera) camera->Release();
    MFShutdown();
    if (FAILED(hr)) {
        *error = HrText(L"Removing the virtual camera", hr);
        return false;
    }
    const std::wstring dst = InstallDir() + L"\\" + kDllName;
    CallDllExport(dst, "DllUnregisterServer");
    if (!DeleteFileW(dst.c_str())) MoveFileExW(dst.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    return true;
#else
    *error = L"This build has no virtual webcam support.";
    return false;
#endif
}

int RunElevated(const wchar_t* arg) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = exe;
    info.lpParameters = arg;
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info) || !info.hProcess) return -1;
    WaitForSingleObject(info.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hProcess);
    return static_cast<int>(code);
}

Writer::~Writer() {
    if (view_) UnmapViewOfFile(view_);
    if (mapping_) CloseHandle(mapping_);
}

bool Writer::Open() {
    if (view_) return true;
    // The camera DLL creates the mapping when an app starts using the camera; check once a second.
    const uint64_t now = GetTickCount64();
    if (now - last_attempt_ < 1000) return false;
    last_attempt_ = now;
    mapping_ = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, shared_frame::kMappingName);
    if (!mapping_) return false;
    view_ = MapViewOfFile(mapping_, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, shared_frame::kMappingSize);
    if (!view_) {
        CloseHandle(mapping_);
        mapping_ = nullptr;
        return false;
    }
    return true;
}

void Writer::Write(const Frame& frame) {
    if (frame.width <= 0 || frame.height <= 0) return;
    if (static_cast<uint64_t>(frame.width) * frame.height > shared_frame::kMaxPixels) return;
    if (!Open()) return;
    auto* header = static_cast<shared_frame::Header*>(view_);
    header->sequence = header->sequence + 1;  // odd: writing
    MemoryBarrier();
    header->width = static_cast<uint32_t>(frame.width);
    header->height = static_cast<uint32_t>(frame.height);
    std::memcpy(shared_frame::Pixels(view_), frame.bgra.data(), frame.bgra.size());
    header->write_time = static_cast<int64_t>(GetTickCount64());
    header->magic = shared_frame::kMagic;
    MemoryBarrier();
    header->sequence = header->sequence + 1;  // even: done
}

}  // namespace vcam
