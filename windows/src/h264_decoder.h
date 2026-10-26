// H.264 (Annex B) to BGRA decoding with the Media Foundation H.264 decoder.
#pragma once

#include <mftransform.h>

#include <cstdint>
#include <vector>

#include "com_ptr.h"

struct Frame {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> bgra;  // width * height * 4, top-down rows
};

class H264Decoder {
public:
    H264Decoder() = default;
    ~H264Decoder();

    // Creates the decoder; the size is a hint until the stream's SPS arrives. Call again after the
    // stream restarts.
    bool Open(int width, int height, int fps);
    void Close();

    // Feeds one access unit. Returns true and fills |out| when a picture is ready.
    bool Decode(const uint8_t* data, size_t size, int64_t pts_us, Frame* out);

private:
    bool SetOutputType();
    bool Drain(Frame* out);
    bool CopyToBgra(IMFSample* sample, Frame* out);

    ComPtr<IMFTransform> mft_;
    int out_width_ = 0;
    int out_height_ = 0;
    int out_stride_ = 0;
    int display_width_ = 0;
    int display_height_ = 0;
};
