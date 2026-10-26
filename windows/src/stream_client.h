// Connects to the phone, runs the handshake, receives and decodes frames on a worker thread.
#pragma once

#include <winsock2.h>
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

#include "h264_decoder.h"

// Messages posted to the notify window. wParam is the session id from Session(); ignore
// messages whose id does not match, they come from an earlier connection.
constexpr UINT WM_APP_CONNECTED = WM_APP + 1;     // read StreamSize()
constexpr UINT WM_APP_DISCONNECTED = WM_APP + 2;  // read LastError()
constexpr UINT WM_APP_FRAME = WM_APP + 3;         // call TakeFrame()
constexpr UINT WM_APP_STATUS = WM_APP + 4;        // read Status()

struct ConnectOptions {
    std::wstring host;
    int port = 4747;
    bool usb = false;
    int width = 1280;
    int height = 720;
    int fps = 30;
    int camera = 0xFF;  // proto::kCameraKeep: use the phone's choice
};

class StreamClient {
public:
    StreamClient() = default;
    ~StreamClient();

    void Start(const ConnectOptions& options, HWND notify);
    void Stop();
    bool Running() const { return thread_.joinable(); }

    void SendSwitchCamera(int camera);
    void SendResolution(int width, int height, int fps);

    // Swaps the newest decoded frame into |frame|. Returns false if there is none.
    bool TakeFrame(Frame* frame);

    std::wstring LastError();
    std::wstring Status();
    WPARAM Session() const { return static_cast<WPARAM>(session_.load()); }
    int StreamWidth() const { return reply_width_; }
    int StreamHeight() const { return reply_height_; }
    int StreamFps() const { return reply_fps_; }

    // Frames and bytes received since the last call.
    void TakeCounters(int* frames, int64_t* bytes);

private:
    void Run(ConnectOptions options);
    bool Connect(const ConnectOptions& options);
    bool RecvAll(uint8_t* buf, size_t len);
    void SendControl(uint8_t type, const uint8_t* payload, uint16_t len);
    void Fail(const std::wstring& message);
    void SetStatus(const std::wstring& status);

    std::thread thread_;
    HWND notify_ = nullptr;
    std::atomic<SOCKET> socket_{INVALID_SOCKET};
    std::atomic<bool> stopping_{false};
    std::mutex send_mutex_;

    std::mutex frame_mutex_;
    Frame latest_;
    bool has_frame_ = false;
    std::atomic<bool> frame_posted_{false};

    std::mutex text_mutex_;
    std::wstring error_;
    std::wstring status_;
    std::wstring recv_error_;  // only touched by the worker thread
    std::atomic<int> session_{0};
    std::atomic<int> reply_width_{0};
    std::atomic<int> reply_height_{0};
    std::atomic<int> reply_fps_{0};

    std::atomic<int> frames_{0};
    std::atomic<int64_t> bytes_{0};
};
