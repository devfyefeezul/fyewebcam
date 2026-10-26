// Media Foundation media source for the fyewebcam virtual camera (Windows 11, MFCreateVirtualCamera).
// The Camera Frame Server loads this DLL and pulls frames from it; frames come from fyewebcam.exe
// through shared memory (see ../src/shared_frame.h).
#pragma once

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>

#include <atomic>
#include <mutex>

#include "../src/com_ptr.h"

// IKsControl, declared here so the DLL doesn't need the DirectShow/KS proxy headers.
MIDL_INTERFACE("28F54685-06FD-11D2-B27A-00A0C9223196")
IKsControl : public IUnknown {
public:
    virtual HRESULT STDMETHODCALLTYPE KsProperty(void* property, ULONG property_length, void* data, ULONG data_length,
                                                 ULONG* bytes_returned) = 0;
    virtual HRESULT STDMETHODCALLTYPE KsMethod(void* method, ULONG method_length, void* data, ULONG data_length,
                                               ULONG* bytes_returned) = 0;
    virtual HRESULT STDMETHODCALLTYPE KsEvent(void* event, ULONG event_length, void* data, ULONG data_length,
                                              ULONG* bytes_returned) = 0;
};

class MediaSource;

class FrameReader {
public:
    ~FrameReader();
    // Fills an NV12 picture of width x height (pitch bytes per row) from the latest frame the app
    // wrote, scaled to fit. Draws a dark placeholder when the app isn't sending frames.
    void Fill(uint8_t* dst, LONG pitch, UINT32 width, UINT32 height);

private:
    bool EnsureMapping();

    HANDLE mapping_ = nullptr;
    void* view_ = nullptr;
};

class MediaStream final : public IMFMediaStream2, public IKsControl {
public:
    MediaStream(MediaSource* source, DWORD id);
    HRESULT Initialize();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IMFMediaEventGenerator
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override;
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override;
    STDMETHODIMP QueueEvent(MediaEventType type, REFGUID ext_type, HRESULT status, const PROPVARIANT* value) override;

    // IMFMediaStream
    STDMETHODIMP GetMediaSource(IMFMediaSource** source) override;
    STDMETHODIMP GetStreamDescriptor(IMFStreamDescriptor** descriptor) override;
    STDMETHODIMP RequestSample(IUnknown* token) override;

    // IMFMediaStream2
    STDMETHODIMP SetStreamState(MF_STREAM_STATE state) override;
    STDMETHODIMP GetStreamState(MF_STREAM_STATE* state) override;

    // IKsControl
    STDMETHODIMP KsProperty(void*, ULONG, void*, ULONG, ULONG*) override;
    STDMETHODIMP KsMethod(void*, ULONG, void*, ULONG, ULONG*) override;
    STDMETHODIMP KsEvent(void*, ULONG, void*, ULONG, ULONG*) override;

    HRESULT Start(IMFMediaType* type);
    HRESULT Stop();
    void Shutdown();
    HRESULT SetAllocator(IUnknown* allocator);
    IMFStreamDescriptor* Descriptor() const { return descriptor_.Get(); }
    IMFAttributes* Attributes() const { return attributes_.Get(); }

private:
    ~MediaStream();

    std::atomic<ULONG> refs_{1};
    std::mutex lock_;
    MediaSource* source_;  // weak: the source owns the stream
    DWORD id_;
    ComPtr<IMFMediaEventQueue> queue_;
    ComPtr<IMFStreamDescriptor> descriptor_;
    ComPtr<IMFAttributes> attributes_;
    ComPtr<IMFVideoSampleAllocatorEx> allocator_;
    MF_STREAM_STATE state_ = MF_STREAM_STATE_STOPPED;
    UINT32 width_ = 0;
    UINT32 height_ = 0;
    UINT64 frame_duration_ = 333333;
    LONGLONG start_time_ = 0;
    FrameReader reader_;
    bool shut_down_ = false;
};

class MediaSource final : public IMFMediaSourceEx, public IMFGetService, public IKsControl,
                          public IMFSampleAllocatorControl {
public:
    MediaSource();
    HRESULT Initialize(IMFAttributes* attributes);

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;

    // IMFMediaEventGenerator
    STDMETHODIMP BeginGetEvent(IMFAsyncCallback* callback, IUnknown* state) override;
    STDMETHODIMP EndGetEvent(IMFAsyncResult* result, IMFMediaEvent** event) override;
    STDMETHODIMP GetEvent(DWORD flags, IMFMediaEvent** event) override;
    STDMETHODIMP QueueEvent(MediaEventType type, REFGUID ext_type, HRESULT status, const PROPVARIANT* value) override;

    // IMFMediaSource
    STDMETHODIMP GetCharacteristics(DWORD* characteristics) override;
    STDMETHODIMP CreatePresentationDescriptor(IMFPresentationDescriptor** descriptor) override;
    STDMETHODIMP Start(IMFPresentationDescriptor* descriptor, const GUID* time_format, const PROPVARIANT* start) override;
    STDMETHODIMP Stop() override;
    STDMETHODIMP Pause() override;
    STDMETHODIMP Shutdown() override;

    // IMFMediaSourceEx
    STDMETHODIMP GetSourceAttributes(IMFAttributes** attributes) override;
    STDMETHODIMP GetStreamAttributes(DWORD stream_id, IMFAttributes** attributes) override;
    STDMETHODIMP SetD3DManager(IUnknown* manager) override;

    // IMFGetService
    STDMETHODIMP GetService(REFGUID service, REFIID riid, LPVOID* object) override;

    // IKsControl
    STDMETHODIMP KsProperty(void*, ULONG, void*, ULONG, ULONG*) override;
    STDMETHODIMP KsMethod(void*, ULONG, void*, ULONG, ULONG*) override;
    STDMETHODIMP KsEvent(void*, ULONG, void*, ULONG, ULONG*) override;

    // IMFSampleAllocatorControl
    STDMETHODIMP SetDefaultAllocator(DWORD stream_id, IUnknown* allocator) override;
    STDMETHODIMP GetAllocatorUsage(DWORD stream_id, DWORD* input_stream_id, MFSampleAllocatorUsage* usage) override;

private:
    ~MediaSource();
    HRESULT CheckShutdown() const { return shut_down_ ? MF_E_SHUTDOWN : S_OK; }

    std::atomic<ULONG> refs_{1};
    std::mutex lock_;
    ComPtr<IMFMediaEventQueue> queue_;
    ComPtr<IMFAttributes> attributes_;
    ComPtr<IMFPresentationDescriptor> descriptor_;
    MediaStream* stream_ = nullptr;  // owned
    bool stream_announced_ = false;
    bool shut_down_ = false;
};
