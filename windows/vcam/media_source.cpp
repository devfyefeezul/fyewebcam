#include "media_source.h"

#include <mferror.h>
#include <sddl.h>

#include <algorithm>
#include <cstring>

#include "../src/shared_frame.h"

namespace {

// PINNAME_VIDEO_CAPTURE from ksmedia.h.
const GUID kPinVideoCapture = {0xfb6c4281, 0x0353, 0x11d1, {0x90, 0x5f, 0x00, 0x00, 0xc0, 0xcc, 0x16, 0xba}};

constexpr DWORD kStreamId = 0;
constexpr UINT32 kFps = 30;

struct Size {
    UINT32 width;
    UINT32 height;
};
// The first one is the default.
const Size kSizes[] = {{1280, 720}, {1920, 1080}, {640, 360}};

HRESULT CreateVideoType(UINT32 width, UINT32 height, IMFMediaType** out) {
    ComPtr<IMFMediaType> type;
    HRESULT hr = MFCreateMediaType(type.Put());
    if (FAILED(hr)) return hr;
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, kFps, 1);
    MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    type->SetUINT32(MF_MT_DEFAULT_STRIDE, width);
    type->SetUINT32(MF_MT_SAMPLE_SIZE, width * height * 3 / 2);
    *out = type.Detach();
    return S_OK;
}

inline uint8_t Clamp(int v) { return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v)); }

}  // namespace

// ---------------------------------------------------------------- FrameReader

FrameReader::~FrameReader() {
    if (view_) UnmapViewOfFile(view_);
    if (mapping_) CloseHandle(mapping_);
}

bool FrameReader::EnsureMapping() {
    if (view_) return true;
    // System, LocalService (the Frame Server), admins: full access; interactive users (the app):
    // read and write.
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;SY)(A;;GA;;;LS)(A;;GA;;;BA)(A;;GRGW;;;IU)",
                                                              SDDL_REVISION_1, &sd, nullptr)) {
        return false;
    }
    SECURITY_ATTRIBUTES sa{sizeof(sa), sd, FALSE};
    mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE, 0, shared_frame::kMappingSize,
                                  shared_frame::kMappingName);
    LocalFree(sd);
    if (!mapping_) return false;
    view_ = MapViewOfFile(mapping_, FILE_MAP_ALL_ACCESS, 0, 0, shared_frame::kMappingSize);
    if (!view_) {
        CloseHandle(mapping_);
        mapping_ = nullptr;
        return false;
    }
    return true;
}

void FrameReader::Fill(uint8_t* dst, LONG pitch, UINT32 width, UINT32 height) {
    uint8_t* y_plane = dst;
    uint8_t* uv_plane = dst + static_cast<size_t>(pitch) * height;

    auto* header = EnsureMapping() ? static_cast<shared_frame::Header*>(view_) : nullptr;
    const uint32_t seq = header ? header->sequence : 0;
    MemoryBarrier();
    const bool live = header && header->magic == shared_frame::kMagic && (seq & 1) == 0 && header->width > 0 &&
                      header->height > 0 && header->width * header->height <= shared_frame::kMaxPixels &&
                      static_cast<int64_t>(GetTickCount64()) - header->write_time < 2000;
    if (!live) {
        // Placeholder: dark grey.
        for (UINT32 y = 0; y < height; ++y) memset(y_plane + static_cast<size_t>(y) * pitch, 40, width);
        for (UINT32 y = 0; y < height / 2; ++y) memset(uv_plane + static_cast<size_t>(y) * pitch, 128, width);
        return;
    }

    const UINT32 sw = header->width;
    const UINT32 sh = header->height;
    const uint8_t* src = shared_frame::Pixels(view_);

    // Fit the source inside the output, keeping its aspect ratio, with black bars.
    UINT32 fw = width;
    UINT32 fh = static_cast<UINT32>(static_cast<uint64_t>(width) * sh / sw);
    if (fh > height) {
        fh = height;
        fw = static_cast<UINT32>(static_cast<uint64_t>(height) * sw / sh);
    }
    fw &= ~1u;
    fh &= ~1u;
    const UINT32 ox = ((width - fw) / 2) & ~1u;
    const UINT32 oy = ((height - fh) / 2) & ~1u;

    for (UINT32 y = 0; y < height; ++y) memset(y_plane + static_cast<size_t>(y) * pitch, 16, width);
    for (UINT32 y = 0; y < height / 2; ++y) memset(uv_plane + static_cast<size_t>(y) * pitch, 128, width);

    // Nearest-neighbour scale and BT.601 limited-range RGB to NV12.
    for (UINT32 y = 0; y < fh; ++y) {
        const uint8_t* srow = src + static_cast<size_t>(y * sh / fh) * sw * 4;
        uint8_t* yrow = y_plane + static_cast<size_t>(oy + y) * pitch + ox;
        uint8_t* uvrow = (y & 1) == 0 ? uv_plane + static_cast<size_t>((oy + y) / 2) * pitch + ox : nullptr;
        for (UINT32 x = 0; x < fw; ++x) {
            const uint8_t* p = srow + static_cast<size_t>(x * sw / fw) * 4;
            const int b = p[0], g = p[1], r = p[2];
            yrow[x] = Clamp(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
            if (uvrow && (x & 1) == 0) {
                uvrow[x] = Clamp(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
                uvrow[x + 1] = Clamp(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
            }
        }
    }
    // If the writer started a new frame while we copied, this one may tear slightly. That's
    // acceptable for a webcam feed and avoids holding up the Frame Server.
}

// ---------------------------------------------------------------- MediaStream

MediaStream::MediaStream(MediaSource* source, DWORD id) : source_(source), id_(id) {}

MediaStream::~MediaStream() = default;

HRESULT MediaStream::Initialize() {
    HRESULT hr = MFCreateEventQueue(queue_.Put());
    if (FAILED(hr)) return hr;

    IMFMediaType* types[ARRAYSIZE(kSizes)] = {};
    for (size_t i = 0; i < ARRAYSIZE(kSizes); ++i) {
        hr = CreateVideoType(kSizes[i].width, kSizes[i].height, &types[i]);
        if (FAILED(hr)) break;
    }
    if (SUCCEEDED(hr)) hr = MFCreateStreamDescriptor(id_, ARRAYSIZE(kSizes), types, descriptor_.Put());
    if (SUCCEEDED(hr)) {
        ComPtr<IMFMediaTypeHandler> handler;
        hr = descriptor_->GetMediaTypeHandler(handler.Put());
        if (SUCCEEDED(hr)) hr = handler->SetCurrentMediaType(types[0]);
    }
    for (IMFMediaType* t : types) {
        if (t) t->Release();
    }
    if (FAILED(hr)) return hr;

    // Attributes the Frame Server looks for on a camera stream.
    hr = MFCreateAttributes(attributes_.Put(), 8);
    if (FAILED(hr)) return hr;
    for (IMFAttributes* a : {attributes_.Get(), static_cast<IMFAttributes*>(descriptor_.Get())}) {
        a->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, kPinVideoCapture);
        a->SetUINT32(MF_DEVICESTREAM_STREAM_ID, id_);
        a->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
        a->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, MFFrameSourceTypes_Color);
    }
    return S_OK;
}

STDMETHODIMP MediaStream::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == __uuidof(IMFMediaEventGenerator) || riid == __uuidof(IMFMediaStream) ||
        riid == __uuidof(IMFMediaStream2)) {
        *ppv = static_cast<IMFMediaStream2*>(this);
    } else if (riid == __uuidof(IKsControl)) {
        *ppv = static_cast<IKsControl*>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) MediaStream::AddRef() { return ++refs_; }

STDMETHODIMP_(ULONG) MediaStream::Release() {
    ULONG n = --refs_;
    if (n == 0) delete this;
    return n;
}

STDMETHODIMP MediaStream::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) {
    std::lock_guard<std::mutex> lock(lock_);
    if (shut_down_) return MF_E_SHUTDOWN;
    return queue_->BeginGetEvent(callback, state);
}

STDMETHODIMP MediaStream::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) {
    std::lock_guard<std::mutex> lock(lock_);
    if (shut_down_) return MF_E_SHUTDOWN;
    return queue_->EndGetEvent(result, event);
}

STDMETHODIMP MediaStream::GetEvent(DWORD flags, IMFMediaEvent** event) {
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard<std::mutex> lock(lock_);
        if (shut_down_) return MF_E_SHUTDOWN;
        queue.Set(queue_.Get());
    }
    return queue->GetEvent(flags, event);
}

STDMETHODIMP MediaStream::QueueEvent(MediaEventType type, REFGUID ext_type, HRESULT status, const PROPVARIANT* value) {
    std::lock_guard<std::mutex> lock(lock_);
    if (shut_down_) return MF_E_SHUTDOWN;
    return queue_->QueueEventParamVar(type, ext_type, status, value);
}

STDMETHODIMP MediaStream::GetMediaSource(IMFMediaSource** source) {
    if (!source) return E_POINTER;
    std::lock_guard<std::mutex> lock(lock_);
    if (shut_down_) return MF_E_SHUTDOWN;
    return source_->QueryInterface(IID_PPV_ARGS(source));
}

STDMETHODIMP MediaStream::GetStreamDescriptor(IMFStreamDescriptor** descriptor) {
    if (!descriptor) return E_POINTER;
    std::lock_guard<std::mutex> lock(lock_);
    if (shut_down_) return MF_E_SHUTDOWN;
    *descriptor = descriptor_.Get();
    (*descriptor)->AddRef();
    return S_OK;
}

STDMETHODIMP MediaStream::RequestSample(IUnknown* token) {
    std::lock_guard<std::mutex> lock(lock_);
    if (shut_down_) return MF_E_SHUTDOWN;
    if (state_ != MF_STREAM_STATE_RUNNING) return MF_E_INVALIDREQUEST;

    ComPtr<IMFSample> sample;
    HRESULT hr;
    if (allocator_) {
        hr = allocator_->AllocateSample(sample.Put());
    } else {
        ComPtr<IMFMediaBuffer> buffer;
        hr = MFCreate2DMediaBuffer(width_, height_, MFVideoFormat_NV12.Data1, FALSE, buffer.Put());
        if (SUCCEEDED(hr)) hr = MFCreateSample(sample.Put());
        if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer.Get());
    }
    if (FAILED(hr)) return hr;

    ComPtr<IMFMediaBuffer> buffer;
    hr = sample->GetBufferByIndex(0, buffer.Put());
    if (FAILED(hr)) return hr;

    ComPtr<IMF2DBuffer2> buffer2d;
    if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(buffer2d.Put())))) {
        BYTE* scanline0 = nullptr;
        LONG pitch = 0;
        BYTE* start = nullptr;
        DWORD length = 0;
        hr = buffer2d->Lock2DSize(MF2DBuffer_LockFlags_Write, &scanline0, &pitch, &start, &length);
        if (FAILED(hr)) return hr;
        reader_.Fill(scanline0, pitch, width_, height_);
        buffer2d->Unlock2D();
    } else {
        BYTE* data = nullptr;
        DWORD max_length = 0;
        hr = buffer->Lock(&data, &max_length, nullptr);
        if (FAILED(hr)) return hr;
        reader_.Fill(data, static_cast<LONG>(width_), width_, height_);
        buffer->Unlock();
        buffer->SetCurrentLength(width_ * height_ * 3 / 2);
    }

    const LONGLONG now = MFGetSystemTime();
    sample->SetSampleTime(now - start_time_);
    sample->SetSampleDuration(static_cast<LONGLONG>(frame_duration_));
    if (token) sample->SetUnknown(MFSampleExtension_Token, token);
    return queue_->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.Get());
}

STDMETHODIMP MediaStream::SetStreamState(MF_STREAM_STATE state) {
    std::lock_guard<std::mutex> lock(lock_);
    if (shut_down_) return MF_E_SHUTDOWN;
    state_ = state;
    return S_OK;
}

STDMETHODIMP MediaStream::GetStreamState(MF_STREAM_STATE* state) {
    if (!state) return E_POINTER;
    std::lock_guard<std::mutex> lock(lock_);
    if (shut_down_) return MF_E_SHUTDOWN;
    *state = state_;
    return S_OK;
}

STDMETHODIMP MediaStream::KsProperty(void*, ULONG, void*, ULONG, ULONG*) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
STDMETHODIMP MediaStream::KsMethod(void*, ULONG, void*, ULONG, ULONG*) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
STDMETHODIMP MediaStream::KsEvent(void*, ULONG, void*, ULONG, ULONG*) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

HRESULT MediaStream::Start(IMFMediaType* type) {
    std::lock_guard<std::mutex> lock(lock_);
    if (shut_down_) return MF_E_SHUTDOWN;
    HRESULT hr = MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &width_, &height_);
    if (FAILED(hr)) return hr;
    UINT32 num = kFps, den = 1;
    MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &num, &den);
    frame_duration_ = num ? 10000000ull * den / num : 333333;
    if (allocator_) {
        hr = allocator_->InitializeSampleAllocator(10, type);
        if (FAILED(hr)) return hr;
    }
    start_time_ = MFGetSystemTime();
    state_ = MF_STREAM_STATE_RUNNING;
    return queue_->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, nullptr);
}

HRESULT MediaStream::Stop() {
    std::lock_guard<std::mutex> lock(lock_);
    if (shut_down_) return MF_E_SHUTDOWN;
    if (allocator_) allocator_->UninitializeSampleAllocator();
    state_ = MF_STREAM_STATE_STOPPED;
    return queue_->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
}

void MediaStream::Shutdown() {
    std::lock_guard<std::mutex> lock(lock_);
    if (shut_down_) return;
    shut_down_ = true;
    if (queue_) queue_->Shutdown();
    if (allocator_) allocator_->UninitializeSampleAllocator();
    allocator_.Reset();
}

HRESULT MediaStream::SetAllocator(IUnknown* allocator) {
    std::lock_guard<std::mutex> lock(lock_);
    if (shut_down_) return MF_E_SHUTDOWN;
    allocator_.Reset();
    if (!allocator) return S_OK;
    return allocator->QueryInterface(IID_PPV_ARGS(allocator_.Put()));
}

// ---------------------------------------------------------------- MediaSource

MediaSource::MediaSource() = default;

MediaSource::~MediaSource() {
    if (stream_) stream_->Release();
}

HRESULT MediaSource::Initialize(IMFAttributes* attributes) {
    HRESULT hr = MFCreateEventQueue(queue_.Put());
    if (FAILED(hr)) return hr;
    hr = MFCreateAttributes(attributes_.Put(), 4);
    if (FAILED(hr)) return hr;
    if (attributes) attributes->CopyAllItems(attributes_.Get());

    stream_ = new MediaStream(this, kStreamId);
    hr = stream_->Initialize();
    if (FAILED(hr)) return hr;

    IMFStreamDescriptor* descriptors[] = {stream_->Descriptor()};
    hr = MFCreatePresentationDescriptor(1, descriptors, descriptor_.Put());
    if (FAILED(hr)) return hr;
    return descriptor_->SelectStream(0);
}

STDMETHODIMP MediaSource::QueryInterface(REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    if (riid == IID_IUnknown || riid == __uuidof(IMFMediaEventGenerator) || riid == __uuidof(IMFMediaSource) ||
        riid == __uuidof(IMFMediaSourceEx)) {
        *ppv = static_cast<IMFMediaSourceEx*>(this);
    } else if (riid == __uuidof(IMFGetService)) {
        *ppv = static_cast<IMFGetService*>(this);
    } else if (riid == __uuidof(IKsControl)) {
        *ppv = static_cast<IKsControl*>(this);
    } else if (riid == __uuidof(IMFSampleAllocatorControl)) {
        *ppv = static_cast<IMFSampleAllocatorControl*>(this);
    } else {
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) MediaSource::AddRef() { return ++refs_; }

STDMETHODIMP_(ULONG) MediaSource::Release() {
    ULONG n = --refs_;
    if (n == 0) delete this;
    return n;
}

STDMETHODIMP MediaSource::BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) {
    std::lock_guard<std::mutex> lock(lock_);
    if (shut_down_) return MF_E_SHUTDOWN;
    return queue_->BeginGetEvent(callback, state);
}

STDMETHODIMP MediaSource::EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) {
    std::lock_guard<std::mutex> lock(lock_);
    if (shut_down_) return MF_E_SHUTDOWN;
    return queue_->EndGetEvent(result, event);
}

STDMETHODIMP MediaSource::GetEvent(DWORD flags, IMFMediaEvent** event) {
    ComPtr<IMFMediaEventQueue> queue;
    {
        std::lock_guard<std::mutex> lock(lock_);
        if (shut_down_) return MF_E_SHUTDOWN;
        queue.Set(queue_.Get());
    }
    return queue->GetEvent(flags, event);
}

STDMETHODIMP MediaSource::QueueEvent(MediaEventType type, REFGUID ext_type, HRESULT status, const PROPVARIANT* value) {
    std::lock_guard<std::mutex> lock(lock_);
    if (shut_down_) return MF_E_SHUTDOWN;
    return queue_->QueueEventParamVar(type, ext_type, status, value);
}

STDMETHODIMP MediaSource::GetCharacteristics(DWORD* characteristics) {
    if (!characteristics) return E_POINTER;
    std::lock_guard<std::mutex> lock(lock_);
    if (shut_down_) return MF_E_SHUTDOWN;
    *characteristics = MFMEDIASOURCE_IS_LIVE;
    return S_OK;
}

STDMETHODIMP MediaSource::CreatePresentationDescriptor(IMFPresentationDescriptor** descriptor) {
    if (!descriptor) return E_POINTER;
    std::lock_guard<std::mutex> lock(lock_);
    if (shut_down_) return MF_E_SHUTDOWN;
    return descriptor_->Clone(descriptor);
}

STDMETHODIMP MediaSource::Start(IMFPresentationDescriptor* descriptor, const GUID* time_format,
                                const PROPVARIANT* start) {
    if (!descriptor) return E_INVALIDARG;
    if (time_format && *time_format != GUID_NULL) return MF_E_UNSUPPORTED_TIME_FORMAT;
    std::lock_guard<std::mutex> lock(lock_);
    if (shut_down_) return MF_E_SHUTDOWN;

    DWORD count = 0;
    HRESULT hr = descriptor->GetStreamDescriptorCount(&count);
    if (FAILED(hr)) return hr;
    for (DWORD i = 0; i < count; ++i) {
        BOOL selected = FALSE;
        ComPtr<IMFStreamDescriptor> sd;
        hr = descriptor->GetStreamDescriptorByIndex(i, &selected, sd.Put());
        if (FAILED(hr)) return hr;
        DWORD id = 0;
        sd->GetStreamIdentifier(&id);
        if (id != kStreamId) continue;
        if (!selected) {
            stream_->Stop();
            continue;
        }
        ComPtr<IMFMediaTypeHandler> handler;
        ComPtr<IMFMediaType> type;
        hr = sd->GetMediaTypeHandler(handler.Put());
        if (SUCCEEDED(hr)) hr = handler->GetCurrentMediaType(type.Put());
        if (FAILED(hr)) return hr;

        hr = queue_->QueueEventParamUnk(stream_announced_ ? MEUpdatedStream : MENewStream, GUID_NULL, S_OK,
                                        static_cast<IMFMediaStream2*>(stream_));
        if (FAILED(hr)) return hr;
        stream_announced_ = true;
        hr = stream_->Start(type.Get());
        if (FAILED(hr)) return hr;
    }

    PROPVARIANT time;
    PropVariantInit(&time);
    time.vt = VT_I8;
    time.hVal.QuadPart = MFGetSystemTime();
    hr = queue_->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, start && start->vt != VT_EMPTY ? start : &time);
    return hr;
}

STDMETHODIMP MediaSource::Stop() {
    std::lock_guard<std::mutex> lock(lock_);
    if (shut_down_) return MF_E_SHUTDOWN;
    stream_->Stop();
    return queue_->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, nullptr);
}

STDMETHODIMP MediaSource::Pause() { return MF_E_INVALID_STATE_TRANSITION; }

STDMETHODIMP MediaSource::Shutdown() {
    std::lock_guard<std::mutex> lock(lock_);
    if (shut_down_) return MF_E_SHUTDOWN;
    shut_down_ = true;
    if (stream_) stream_->Shutdown();
    if (queue_) queue_->Shutdown();
    return S_OK;
}

STDMETHODIMP MediaSource::GetSourceAttributes(IMFAttributes** attributes) {
    if (!attributes) return E_POINTER;
    std::lock_guard<std::mutex> lock(lock_);
    if (shut_down_) return MF_E_SHUTDOWN;
    *attributes = attributes_.Get();
    (*attributes)->AddRef();
    return S_OK;
}

STDMETHODIMP MediaSource::GetStreamAttributes(DWORD stream_id, IMFAttributes** attributes) {
    if (!attributes) return E_POINTER;
    std::lock_guard<std::mutex> lock(lock_);
    if (shut_down_) return MF_E_SHUTDOWN;
    if (stream_id != kStreamId) return MF_E_INVALIDSTREAMNUMBER;
    *attributes = stream_->Attributes();
    (*attributes)->AddRef();
    return S_OK;
}

STDMETHODIMP MediaSource::SetD3DManager(IUnknown*) {
    // Frames are produced on the CPU, so the D3D manager isn't needed.
    return S_OK;
}

STDMETHODIMP MediaSource::GetService(REFGUID, REFIID, LPVOID* object) {
    if (object) *object = nullptr;
    return MF_E_UNSUPPORTED_SERVICE;
}

STDMETHODIMP MediaSource::KsProperty(void*, ULONG, void*, ULONG, ULONG*) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
STDMETHODIMP MediaSource::KsMethod(void*, ULONG, void*, ULONG, ULONG*) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
STDMETHODIMP MediaSource::KsEvent(void*, ULONG, void*, ULONG, ULONG*) {
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

STDMETHODIMP MediaSource::SetDefaultAllocator(DWORD stream_id, IUnknown* allocator) {
    std::lock_guard<std::mutex> lock(lock_);
    if (shut_down_) return MF_E_SHUTDOWN;
    if (stream_id != kStreamId) return MF_E_INVALIDSTREAMNUMBER;
    return stream_->SetAllocator(allocator);
}

STDMETHODIMP MediaSource::GetAllocatorUsage(DWORD stream_id, DWORD* input_stream_id, MFSampleAllocatorUsage* usage) {
    if (!input_stream_id || !usage) return E_POINTER;
    std::lock_guard<std::mutex> lock(lock_);
    if (shut_down_) return MF_E_SHUTDOWN;
    if (stream_id != kStreamId) return MF_E_INVALIDSTREAMNUMBER;
    *input_stream_id = stream_id;
    *usage = MFSampleAllocatorUsage_UsesProvidedAllocator;
    return S_OK;
}
