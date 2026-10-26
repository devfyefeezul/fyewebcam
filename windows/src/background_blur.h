// Background blur: a person-segmentation model (MediaPipe selfie segmenter, run with ONNX Runtime)
// finds the person in each frame and everything else is blurred.
//
// ONNX Runtime is loaded at run time from onnxruntime.dll next to the exe, so the app still runs
// (without blur) when it is missing. Portable apart from that loading, so it can be tried out on
// Linux too.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct OrtApi;
struct OrtEnv;
struct OrtSession;
struct OrtMemoryInfo;

class BackgroundBlur {
public:
    BackgroundBlur() = default;
    ~BackgroundBlur();
    BackgroundBlur(const BackgroundBlur&) = delete;
    BackgroundBlur& operator=(const BackgroundBlur&) = delete;

    // Loads ONNX Runtime and the model (an in-memory .onnx file). On failure returns false and sets
    // |error| to a short reason.
    bool Init(const void* model, size_t model_size, std::string* error);
    bool Ready() const { return session_ != nullptr; }

    // Blurs the background of a top-down BGRA picture in place. |strength| is 1 (light) to 10.
    void Apply(uint8_t* bgra, int width, int height, int strength);

    // Time the last Apply took, in milliseconds.
    double LastMs() const { return last_ms_; }

private:
    bool Segment(const uint8_t* bgra, int width, int height);
    void BuildBackground(const uint8_t* bgra, int width, int height, int strength);
    void Composite(uint8_t* bgra, int width, int height);
    float MaskAt(float mx, float my) const;

    void* library_ = nullptr;
    const OrtApi* api_ = nullptr;
    OrtEnv* env_ = nullptr;
    OrtSession* session_ = nullptr;
    OrtMemoryInfo* memory_ = nullptr;
    std::string input_name_;
    std::string output_name_;

    std::vector<float> input_;  // model input, 256 x 256 RGB in [0, 1]

    // Person mask over the part of the model input the frame occupies (the frame is letterboxed
    // into the square model input to keep its proportions).
    std::vector<float> mask_;
    int mask_w_ = 0;
    int mask_h_ = 0;

    // Blurred background at 1/kScale size: premultiplied BGR plus weight (1 - person), so the
    // person's colours don't bleed into the background around them.
    std::vector<float> small_;
    std::vector<float> scratch_;
    int small_w_ = 0;
    int small_h_ = 0;

    double last_ms_ = 0;
};
