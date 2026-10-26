// The app side of the virtual webcam: installing it (elevated) and feeding it frames.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

#include "h264_decoder.h"

namespace vcam {

// True when this build can create virtual cameras (Windows 11 SDK, MSVC).
bool Supported();
// True when the camera DLL is registered for the Frame Server.
bool Installed();
// True when Windows currently lists the "fyewebcam" camera. It can be registered yet missing, for
// example after a restart, and then needs installing again.
bool Present();

// Run from an elevated instance of the app ("--install-vcam" / "--uninstall-vcam").
bool Install(std::wstring* error);
bool Uninstall(std::wstring* error);

// Starts this exe elevated with |arg| and waits. Returns the process exit code, or -1 if the user
// declined the UAC prompt or it could not start.
int RunElevated(const wchar_t* arg);

// Copies frames into the shared memory the camera DLL reads. Cheap when no app uses the camera.
class Writer {
public:
    ~Writer();
    void Write(const Frame& frame);
    // True while a camera app is reading frames.
    bool Active() const { return view_ != nullptr; }

private:
    bool Open();

    HANDLE mapping_ = nullptr;
    void* view_ = nullptr;
    uint64_t last_attempt_ = 0;
};

}  // namespace vcam
