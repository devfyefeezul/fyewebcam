#include "h264_decoder.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>

#include <algorithm>
#include <cstring>

namespace {

// CLSID_CMSH264DecoderMFT, declared here to avoid depending on wmcodecdspuuid.lib.
const CLSID kH264DecoderClsid = {0x62CE7E72, 0x4C71, 0x4d20, {0xB1, 0x5D, 0x45, 0x28, 0x31, 0xA8, 0x7D, 0x9D}};
// MF_LOW_LATENCY
const GUID kLowLatency = {0x9c27891a, 0xed7a, 0x40e1, {0x88, 0xe8, 0xb2, 0x27, 0x27, 0xa0, 0x24, 0xee}};

inline uint8_t Clamp(int v) { return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v)); }

// BT.601 limited range NV12 to BGRA.
void Nv12ToBgra(const uint8_t* y_plane, const uint8_t* uv_plane, int stride, int width, int height, uint8_t* dst) {
    for (int y = 0; y < height; ++y) {
        const uint8_t* yrow = y_plane + y * stride;
        const uint8_t* uvrow = uv_plane + (y / 2) * stride;
        uint8_t* out = dst + static_cast<size_t>(y) * width * 4;
        for (int x = 0; x < width; ++x) {
            int c = (yrow[x] - 16) * 298;
            int d = uvrow[x & ~1] - 128;
            int e = uvrow[(x & ~1) + 1] - 128;
            out[0] = Clamp((c + 516 * d + 128) >> 8);
            out[1] = Clamp((c - 100 * d - 208 * e + 128) >> 8);
            out[2] = Clamp((c + 409 * e + 128) >> 8);
            out[3] = 255;
            out += 4;
        }
    }
}

}  // namespace

H264Decoder::~H264Decoder() { Close(); }

void H264Decoder::Close() {
    if (mft_) {
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }
    mft_.Reset();
    out_width_ = out_height_ = out_stride_ = 0;
}

bool H264Decoder::Open(int width, int height, int fps) {
    Close();
    if (FAILED(CoCreateInstance(kH264DecoderClsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(mft_.Put())))) {
        return false;
    }
    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(mft_->GetAttributes(attrs.Put())) && attrs) {
        attrs->SetUINT32(kLowLatency, TRUE);
    }

    ComPtr<IMFMediaType> in;
    if (FAILED(MFCreateMediaType(in.Put()))) return false;
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, fps > 0 ? fps : 30, 1);
    if (FAILED(mft_->SetInputType(0, in.Get(), 0))) return false;
    if (!SetOutputType()) return false;

    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return true;
}

bool H264Decoder::SetOutputType() {
    for (DWORD i = 0;; ++i) {
        ComPtr<IMFMediaType> type;
        if (FAILED(mft_->GetOutputAvailableType(0, i, type.Put()))) return false;
        GUID subtype{};
        if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype)) || subtype != MFVideoFormat_NV12) continue;
        if (FAILED(mft_->SetOutputType(0, type.Get(), 0))) return false;

        UINT32 w = 0, h = 0;
        MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h);
        out_width_ = static_cast<int>(w);
        out_height_ = static_cast<int>(h);
        UINT32 stride = 0;
        if (FAILED(type->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride)) || static_cast<int>(stride) <= 0) {
            stride = w;
        }
        out_stride_ = static_cast<int>(stride);

        // The decoder pads to 16-pixel rows (1080 becomes 1088); the aperture holds the visible size.
        display_width_ = out_width_;
        display_height_ = out_height_;
        MFVideoArea aperture{};
        if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&aperture),
                                    sizeof(aperture), nullptr)) &&
            aperture.Area.cx > 0 && aperture.Area.cy > 0) {
            display_width_ = std::min<int>(aperture.Area.cx, out_width_);
            display_height_ = std::min<int>(aperture.Area.cy, out_height_);
        }
        return true;
    }
}

bool H264Decoder::Decode(const uint8_t* data, size_t size, int64_t pts_us, Frame* out) {
    if (!mft_ || size == 0) return false;

    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(MFCreateMemoryBuffer(static_cast<DWORD>(size), buffer.Put()))) return false;
    BYTE* dst = nullptr;
    buffer->Lock(&dst, nullptr, nullptr);
    std::memcpy(dst, data, size);
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(size));

    ComPtr<IMFSample> sample;
    if (FAILED(MFCreateSample(sample.Put()))) return false;
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(pts_us * 10);

    bool got = false;
    HRESULT hr = mft_->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        got = Drain(out);
        hr = mft_->ProcessInput(0, sample.Get(), 0);
    }
    if (FAILED(hr)) return got;
    return Drain(out) || got;
}

bool H264Decoder::Drain(Frame* out) {
    bool got = false;
    for (;;) {
        MFT_OUTPUT_STREAM_INFO info{};
        if (FAILED(mft_->GetOutputStreamInfo(0, &info))) return got;
        const bool mft_allocates =
            (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;

        ComPtr<IMFSample> own_sample;
        if (!mft_allocates) {
            ComPtr<IMFMediaBuffer> buf;
            if (FAILED(MFCreateMemoryBuffer(info.cbSize, buf.Put()))) return got;
            if (FAILED(MFCreateSample(own_sample.Put()))) return got;
            own_sample->AddBuffer(buf.Get());
        }

        MFT_OUTPUT_DATA_BUFFER output{};
        output.pSample = own_sample.Get();
        DWORD status = 0;
        HRESULT hr = mft_->ProcessOutput(0, 1, &output, &status);
        if (output.pEvents) output.pEvents->Release();

        if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return got;
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
            if (!SetOutputType()) return got;
            continue;
        }
        if (FAILED(hr)) return got;

        if (output.pSample) {
            got = CopyToBgra(output.pSample, out) || got;
            if (mft_allocates) output.pSample->Release();
        }
    }
}

bool H264Decoder::CopyToBgra(IMFSample* sample, Frame* out) {
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(buffer.Put()))) return false;

    BYTE* data = nullptr;
    LONG pitch = out_stride_;
    ComPtr<IMF2DBuffer> buffer2d;
    bool locked2d = false;
    if (SUCCEEDED(buffer->QueryInterface(IID_PPV_ARGS(buffer2d.Put())))) {
        locked2d = SUCCEEDED(buffer2d->Lock2D(&data, &pitch));
    }
    DWORD length = 0;
    if (!locked2d && FAILED(buffer->Lock(&data, nullptr, &length))) return false;
    if (pitch <= 0) pitch = out_width_;

    out->width = display_width_;
    out->height = display_height_;
    out->bgra.resize(static_cast<size_t>(out->width) * out->height * 4);
    const uint8_t* uv = data + static_cast<size_t>(pitch) * out_height_;
    Nv12ToBgra(data, uv, pitch, out->width, out->height, out->bgra.data());

    if (locked2d) {
        buffer2d->Unlock2D();
    } else {
        buffer->Unlock();
    }
    return true;
}
