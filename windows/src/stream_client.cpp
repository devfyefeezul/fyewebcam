#include "stream_client.h"

#include <ws2tcpip.h>

#include <utility>
#include <vector>

#include "adb.h"
#include "protocol.h"

namespace {

std::string Narrow(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

}  // namespace

StreamClient::~StreamClient() { Stop(); }

void StreamClient::Start(const ConnectOptions& options, HWND notify) {
    Stop();
    notify_ = notify;
    session_++;
    stopping_ = false;
    has_frame_ = false;
    frame_posted_ = false;
    {
        std::lock_guard<std::mutex> lock(text_mutex_);
        error_.clear();
    }
    thread_ = std::thread(&StreamClient::Run, this, options);
}

void StreamClient::Stop() {
    stopping_ = true;
    SOCKET s = socket_.exchange(INVALID_SOCKET);
    if (s != INVALID_SOCKET) {
        uint8_t bye[3] = {proto::kCtrlBye, 0, 0};
        send(s, reinterpret_cast<const char*>(bye), sizeof(bye), 0);
        shutdown(s, SD_BOTH);
        closesocket(s);
    }
    if (thread_.joinable()) thread_.join();
}

void StreamClient::SendSwitchCamera(int camera) {
    uint8_t payload[1] = {static_cast<uint8_t>(camera)};
    SendControl(proto::kCtrlSwitchCamera, payload, 1);
}

void StreamClient::SendResolution(int width, int height, int fps) {
    uint8_t payload[5];
    proto::PutU16(payload, static_cast<uint16_t>(width));
    proto::PutU16(payload + 2, static_cast<uint16_t>(height));
    payload[4] = static_cast<uint8_t>(fps);
    SendControl(proto::kCtrlSetResolution, payload, 5);
}

void StreamClient::SendControl(uint8_t type, const uint8_t* payload, uint16_t len) {
    std::vector<uint8_t> msg(3 + len);
    msg[0] = type;
    proto::PutU16(msg.data() + 1, len);
    if (len) memcpy(msg.data() + 3, payload, len);
    std::lock_guard<std::mutex> lock(send_mutex_);
    SOCKET s = socket_;
    if (s != INVALID_SOCKET) send(s, reinterpret_cast<const char*>(msg.data()), static_cast<int>(msg.size()), 0);
}

bool StreamClient::TakeFrame(Frame* frame) {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    frame_posted_ = false;
    if (!has_frame_) return false;
    std::swap(*frame, latest_);
    has_frame_ = false;
    return true;
}

std::wstring StreamClient::LastError() {
    std::lock_guard<std::mutex> lock(text_mutex_);
    return error_;
}

std::wstring StreamClient::Status() {
    std::lock_guard<std::mutex> lock(text_mutex_);
    return status_;
}

void StreamClient::TakeCounters(int* frames, int64_t* bytes) {
    *frames = frames_.exchange(0);
    *bytes = bytes_.exchange(0);
}

void StreamClient::Fail(const std::wstring& message) {
    {
        std::lock_guard<std::mutex> lock(text_mutex_);
        error_ = stopping_ ? L"" : message;
    }
    PostMessageW(notify_, WM_APP_DISCONNECTED, Session(), 0);
}

void StreamClient::SetStatus(const std::wstring& status) {
    {
        std::lock_guard<std::mutex> lock(text_mutex_);
        status_ = status;
    }
    PostMessageW(notify_, WM_APP_STATUS, Session(), 0);
}

bool StreamClient::RecvAll(uint8_t* buf, size_t len) {
    size_t got = 0;
    while (got < len) {
        SOCKET s = socket_;
        if (s == INVALID_SOCKET) return false;
        int n = recv(s, reinterpret_cast<char*>(buf + got), static_cast<int>(len - got), 0);
        if (n <= 0) return false;
        got += static_cast<size_t>(n);
    }
    return true;
}

bool StreamClient::Connect(const ConnectOptions& options) {
    std::string host = options.usb ? "127.0.0.1" : Narrow(options.host);
    std::string port = std::to_string(options.port);
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &result) != 0 || !result) {
        Fail(L"Could not find that address. Check the IP shown on the phone.");
        return false;
    }
    SOCKET s = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (s == INVALID_SOCKET) {
        freeaddrinfo(result);
        Fail(L"Could not open a network socket.");
        return false;
    }
    socket_ = s;
    int rc = connect(s, result->ai_addr, static_cast<int>(result->ai_addrlen));
    freeaddrinfo(result);
    if (rc != 0) {
        Fail(L"Could not connect. Is the phone streaming and on the same network?");
        return false;
    }
    BOOL nodelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
    DWORD timeout_ms = 8000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));
    int rcvbuf = 1 << 20;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));
    return true;
}

void StreamClient::Run(ConnectOptions options) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    if (options.usb) {
        SetStatus(L"Setting up USB...");
        std::wstring err;
        if (!AdbForward(options.port, &err)) {
            Fail(err);
            CoUninitialize();
            return;
        }
    }
    SetStatus(L"Connecting...");
    if (!Connect(options)) {
        CoUninitialize();
        return;
    }

    uint8_t hello[proto::kHandshakeSize];
    proto::WriteHello({static_cast<uint16_t>(options.width), static_cast<uint16_t>(options.height),
                       static_cast<uint8_t>(options.fps), static_cast<uint8_t>(options.camera)},
                      hello);
    uint8_t reply_bytes[proto::kHandshakeSize];
    proto::Reply reply{};
    {
        std::lock_guard<std::mutex> lock(send_mutex_);
        send(socket_, reinterpret_cast<const char*>(hello), sizeof(hello), 0);
    }
    if (!RecvAll(reply_bytes, sizeof(reply_bytes)) || !proto::ParseReply(reply_bytes, &reply)) {
        Fail(L"The phone did not answer. Make sure fyewebcam is streaming on the phone.");
        CoUninitialize();
        return;
    }
    if (reply.status != proto::kStatusOk || reply.codec != proto::kCodecH264) {
        Fail(L"The phone could not start its camera.");
        CoUninitialize();
        return;
    }
    reply_width_ = reply.width;
    reply_height_ = reply.height;
    reply_fps_ = reply.fps;
    PostMessageW(notify_, WM_APP_CONNECTED, Session(), 0);

    H264Decoder decoder;
    bool decoder_open = false;
    int stream_w = reply.width;
    int stream_h = reply.height;
    std::vector<uint8_t> config;
    std::vector<uint8_t> payload;
    std::vector<uint8_t> unit;
    Frame decoded;
    bool waiting_for_key = true;

    for (;;) {
        uint8_t header_bytes[proto::kFrameHeaderSize];
        if (!RecvAll(header_bytes, sizeof(header_bytes))) break;
        proto::FrameHeader header = proto::ParseFrameHeader(header_bytes);
        if (header.length > 16 * 1024 * 1024) break;
        payload.resize(header.length);
        if (!RecvAll(payload.data(), payload.size())) break;
        bytes_ += static_cast<int64_t>(proto::kFrameHeaderSize + header.length);

        if (header.type == proto::kFrameConfig) {
            // A new config means the encoder restarted (new size or camera): start a new decoder.
            config = payload;
            decoder_open = false;
            waiting_for_key = true;
            continue;
        }
        if (waiting_for_key && header.type != proto::kFrameKey) continue;

        const uint8_t* data = payload.data();
        size_t size = payload.size();
        if (header.type == proto::kFrameKey) {
            if (!decoder_open) {
                if (!decoder.Open(stream_w, stream_h, reply.fps)) {
                    Fail(L"Could not start the H.264 decoder.");
                    break;
                }
                decoder_open = true;
            }
            // Send SPS/PPS with every keyframe so the decoder can always resync.
            unit.assign(config.begin(), config.end());
            unit.insert(unit.end(), payload.begin(), payload.end());
            data = unit.data();
            size = unit.size();
            waiting_for_key = false;
        }
        if (!decoder_open) continue;

        if (decoder.Decode(data, size, static_cast<int64_t>(header.pts_us), &decoded)) {
            frames_++;
            {
                std::lock_guard<std::mutex> lock(frame_mutex_);
                std::swap(latest_, decoded);
                has_frame_ = true;
            }
            if (!frame_posted_.exchange(true)) PostMessageW(notify_, WM_APP_FRAME, Session(), 0);
        }
    }

    decoder.Close();
    SOCKET s = socket_.exchange(INVALID_SOCKET);
    if (s != INVALID_SOCKET) closesocket(s);
    Fail(L"Connection to the phone was lost.");
    CoUninitialize();
}
