#include "background_blur.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#include <cstdlib>
#endif

#include <onnxruntime_c_api.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace {

constexpr int kModelSize = 256;  // the model takes and returns 256 x 256
constexpr int kScale = 4;        // the background is blurred at 1/4 size

// Turns the model's soft person probability into blend weight: below 0.3 is background, above 0.7
// is the person, with a smooth ramp in between.
inline float PersonAlpha(float p) {
    float a = (p - 0.3f) * 2.5f;
    a = a < 0 ? 0 : (a > 1 ? 1 : a);
    return a * a * (3 - 2 * a);
}

inline int Clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Box blur over rows of 4-channel pixels, clamping at the edges.
void BoxRows(const float* src, float* dst, int w, int h, int r) {
    const float inv = 1.0f / static_cast<float>(2 * r + 1);
    for (int y = 0; y < h; ++y) {
        const float* s = src + static_cast<size_t>(y) * w * 4;
        float* d = dst + static_cast<size_t>(y) * w * 4;
        float acc[4] = {0, 0, 0, 0};
        for (int k = -r; k <= r; ++k) {
            const float* p = s + Clamp(k, 0, w - 1) * 4;
            for (int c = 0; c < 4; ++c) acc[c] += p[c];
        }
        for (int x = 0; x < w; ++x) {
            for (int c = 0; c < 4; ++c) d[x * 4 + c] = acc[c] * inv;
            const float* out = s + Clamp(x - r, 0, w - 1) * 4;
            const float* in = s + Clamp(x + r + 1, 0, w - 1) * 4;
            for (int c = 0; c < 4; ++c) acc[c] += in[c] - out[c];
        }
    }
}

// Same down the columns.
void BoxColumns(const float* src, float* dst, int w, int h, int r) {
    const float inv = 1.0f / static_cast<float>(2 * r + 1);
    const size_t stride = static_cast<size_t>(w) * 4;
    for (int x = 0; x < w; ++x) {
        const float* s = src + x * 4;
        float* d = dst + x * 4;
        float acc[4] = {0, 0, 0, 0};
        for (int k = -r; k <= r; ++k) {
            const float* p = s + Clamp(k, 0, h - 1) * stride;
            for (int c = 0; c < 4; ++c) acc[c] += p[c];
        }
        for (int y = 0; y < h; ++y) {
            for (int c = 0; c < 4; ++c) d[y * stride + c] = acc[c] * inv;
            const float* out = s + Clamp(y - r, 0, h - 1) * stride;
            const float* in = s + Clamp(y + r + 1, 0, h - 1) * stride;
            for (int c = 0; c < 4; ++c) acc[c] += in[c] - out[c];
        }
    }
}

// Bilinear sample positions for resampling |src_n| samples onto |dst_n| (pixel centres aligned).
struct Taps {
    std::vector<int> i0, i1;
    std::vector<float> f;
};

void MakeTaps(int dst_n, int src_n, Taps* t) {
    t->i0.resize(dst_n);
    t->i1.resize(dst_n);
    t->f.resize(dst_n);
    const float scale = static_cast<float>(src_n) / static_cast<float>(dst_n);
    for (int i = 0; i < dst_n; ++i) {
        float s = (static_cast<float>(i) + 0.5f) * scale - 0.5f;
        s = std::max(0.0f, std::min(s, static_cast<float>(src_n - 1)));
        const int a = static_cast<int>(s);
        t->i0[i] = a;
        t->i1[i] = std::min(a + 1, src_n - 1);
        t->f[i] = s - static_cast<float>(a);
    }
}

}  // namespace

BackgroundBlur::~BackgroundBlur() {
    if (api_) {
        if (session_) api_->ReleaseSession(session_);
        if (memory_) api_->ReleaseMemoryInfo(memory_);
        if (env_) api_->ReleaseEnv(env_);
    }
    // The runtime library stays loaded: unloading it while its thread pool winds down is not safe.
}

bool BackgroundBlur::Init(const void* model, size_t model_size, std::string* error) {
    using GetApiBaseFn = const OrtApiBase*(ORT_API_CALL*)();
    GetApiBaseFn get_api_base = nullptr;
#ifdef _WIN32
    // Load the copy next to the exe by full path, not whatever onnxruntime.dll Windows finds first.
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        *error = "Couldn't find the app folder.";
        return false;
    }
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) slash[1] = L'\0';
    if (wcslen(path) + 20 >= MAX_PATH) {
        *error = "The app folder path is too long.";
        return false;
    }
    wcscat_s(path, L"onnxruntime.dll");
    HMODULE lib = LoadLibraryExW(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!lib) {
        *error = "onnxruntime.dll is missing from the app folder.";
        return false;
    }
    library_ = lib;
    get_api_base = reinterpret_cast<GetApiBaseFn>(reinterpret_cast<void*>(GetProcAddress(lib, "OrtGetApiBase")));
#else
    const char* lib_path = std::getenv("FYE_ORT_LIB");
    library_ = dlopen(lib_path ? lib_path : "libonnxruntime.so", RTLD_NOW);
    if (!library_) {
        *error = "Couldn't load libonnxruntime.so.";
        return false;
    }
    get_api_base = reinterpret_cast<GetApiBaseFn>(dlsym(library_, "OrtGetApiBase"));
#endif
    if (!get_api_base) {
        *error = "onnxruntime.dll is not a usable ONNX Runtime.";
        return false;
    }
    api_ = get_api_base()->GetApi(ORT_API_VERSION);
    if (!api_) {
        *error = "onnxruntime.dll is too old.";
        return false;
    }

    auto ok = [&](OrtStatus* status) {
        if (!status) return true;
        *error = api_->GetErrorMessage(status);
        api_->ReleaseStatus(status);
        return false;
    };
    if (!ok(api_->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "fyewebcam", &env_))) return false;
    OrtSessionOptions* options = nullptr;
    if (!ok(api_->CreateSessionOptions(&options))) return false;
    bool good = ok(api_->SetIntraOpNumThreads(options, 2)) &&
                ok(api_->SetSessionGraphOptimizationLevel(options, ORT_ENABLE_ALL));
    OrtSession* session = nullptr;
    if (good) good = ok(api_->CreateSessionFromArray(env_, model, model_size, options, &session));
    api_->ReleaseSessionOptions(options);
    if (!good) return false;

    OrtAllocator* allocator = nullptr;
    char* in_name = nullptr;
    char* out_name = nullptr;
    good = ok(api_->GetAllocatorWithDefaultOptions(&allocator)) &&
           ok(api_->SessionGetInputName(session, 0, allocator, &in_name)) &&
           ok(api_->SessionGetOutputName(session, 0, allocator, &out_name)) &&
           ok(api_->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &memory_));
    if (in_name) input_name_ = in_name;
    if (out_name) output_name_ = out_name;
    if (allocator) {
        if (in_name) api_->ReleaseStatus(api_->AllocatorFree(allocator, in_name));
        if (out_name) api_->ReleaseStatus(api_->AllocatorFree(allocator, out_name));
    }
    if (!good) {
        api_->ReleaseSession(session);
        return false;
    }
    session_ = session;
    input_.assign(static_cast<size_t>(kModelSize) * kModelSize * 3, 0.0f);
    return true;
}

void BackgroundBlur::Apply(uint8_t* bgra, int width, int height, int strength) {
    if (!session_ || width < kScale * 2 || height < kScale * 2) return;
    const auto start = std::chrono::steady_clock::now();
    if (Segment(bgra, width, height)) {
        BuildBackground(bgra, width, height, std::max(1, std::min(strength, 10)));
        Composite(bgra, width, height);
    }
    last_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

bool BackgroundBlur::Segment(const uint8_t* bgra, int width, int height) {
    // Letterbox the frame into the square model input, keeping its proportions.
    const float scale = std::min(static_cast<float>(kModelSize) / width, static_cast<float>(kModelSize) / height);
    const int iw = std::max(1, std::min(kModelSize, static_cast<int>(width * scale + 0.5f)));
    const int ih = std::max(1, std::min(kModelSize, static_cast<int>(height * scale + 0.5f)));
    const int ox = (kModelSize - iw) / 2;
    const int oy = (kModelSize - ih) / 2;

    std::fill(input_.begin(), input_.end(), 0.0f);
    constexpr float kInv255 = 1.0f / 255.0f;
    for (int y = 0; y < ih; ++y) {
        const int sy = std::min(height - 1, static_cast<int>((y + 0.5f) * height / ih));
        const uint8_t* row = bgra + static_cast<size_t>(sy) * width * 4;
        float* dst = input_.data() + (static_cast<size_t>(oy + y) * kModelSize + ox) * 3;
        for (int x = 0; x < iw; ++x) {
            const int sx = std::min(width - 1, static_cast<int>((x + 0.5f) * width / iw));
            const uint8_t* p = row + sx * 4;
            dst[x * 3 + 0] = p[2] * kInv255;  // R
            dst[x * 3 + 1] = p[1] * kInv255;  // G
            dst[x * 3 + 2] = p[0] * kInv255;  // B
        }
    }

    const int64_t shape[4] = {1, kModelSize, kModelSize, 3};
    OrtValue* in = nullptr;
    OrtValue* out = nullptr;
    OrtStatus* status = api_->CreateTensorWithDataAsOrtValue(memory_, input_.data(), input_.size() * sizeof(float),
                                                             shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in);
    const char* in_names[] = {input_name_.c_str()};
    const char* out_names[] = {output_name_.c_str()};
    if (!status) status = api_->Run(session_, nullptr, in_names, &in, 1, out_names, 1, &out);
    float* result = nullptr;
    if (!status) status = api_->GetTensorMutableData(out, reinterpret_cast<void**>(&result));
    const bool good = status == nullptr && result != nullptr;
    if (status) api_->ReleaseStatus(status);

    if (good) {
        // Blend with the previous mask to calm frame-to-frame flicker at the edges.
        const bool blend = mask_w_ == iw && mask_h_ == ih;
        mask_.resize(static_cast<size_t>(iw) * ih);
        for (int y = 0; y < ih; ++y) {
            const float* src = result + static_cast<size_t>(oy + y) * kModelSize + ox;
            float* dst = mask_.data() + static_cast<size_t>(y) * iw;
            for (int x = 0; x < iw; ++x) dst[x] = blend ? 0.35f * dst[x] + 0.65f * src[x] : src[x];
        }
        mask_w_ = iw;
        mask_h_ = ih;
    }
    if (out) api_->ReleaseValue(out);
    if (in) api_->ReleaseValue(in);
    return good;
}

float BackgroundBlur::MaskAt(float mx, float my) const {
    mx = std::max(0.0f, std::min(mx, static_cast<float>(mask_w_ - 1)));
    my = std::max(0.0f, std::min(my, static_cast<float>(mask_h_ - 1)));
    const int x0 = static_cast<int>(mx);
    const int y0 = static_cast<int>(my);
    const int x1 = std::min(x0 + 1, mask_w_ - 1);
    const int y1 = std::min(y0 + 1, mask_h_ - 1);
    const float fx = mx - x0;
    const float fy = my - y0;
    const float* r0 = mask_.data() + static_cast<size_t>(y0) * mask_w_;
    const float* r1 = mask_.data() + static_cast<size_t>(y1) * mask_w_;
    const float top = r0[x0] + (r0[x1] - r0[x0]) * fx;
    const float bottom = r1[x0] + (r1[x1] - r1[x0]) * fx;
    return top + (bottom - top) * fy;
}

void BackgroundBlur::BuildBackground(const uint8_t* bgra, int width, int height, int strength) {
    small_w_ = (width + kScale - 1) / kScale;
    small_h_ = (height + kScale - 1) / kScale;
    const size_t count = static_cast<size_t>(small_w_) * small_h_ * 4;
    small_.resize(count);
    scratch_.resize(count);

    // Average each kScale x kScale block, weighted by how much of it is background.
    const float mask_sx = static_cast<float>(mask_w_) / width;
    const float mask_sy = static_cast<float>(mask_h_) / height;
    for (int j = 0; j < small_h_; ++j) {
        const int y0 = j * kScale;
        const int y1 = std::min(height, y0 + kScale);
        for (int i = 0; i < small_w_; ++i) {
            const int x0 = i * kScale;
            const int x1 = std::min(width, x0 + kScale);
            float b = 0, g = 0, r = 0;
            for (int y = y0; y < y1; ++y) {
                const uint8_t* p = bgra + (static_cast<size_t>(y) * width + x0) * 4;
                for (int x = x0; x < x1; ++x, p += 4) {
                    b += p[0];
                    g += p[1];
                    r += p[2];
                }
            }
            const float n = static_cast<float>((y1 - y0) * (x1 - x0));
            const float cx = (x0 + x1) * 0.5f * mask_sx - 0.5f;
            const float cy = (y0 + y1) * 0.5f * mask_sy - 0.5f;
            const float w = std::max(0.02f, 1.0f - PersonAlpha(MaskAt(cx, cy)));
            float* s = small_.data() + (static_cast<size_t>(j) * small_w_ + i) * 4;
            s[0] = b / n * w;
            s[1] = g / n * w;
            s[2] = r / n * w;
            s[3] = w;
        }
    }

    // Two box passes each way approximate a Gaussian.
    const int radius = strength * 2;
    for (int pass = 0; pass < 2; ++pass) {
        BoxRows(small_.data(), scratch_.data(), small_w_, small_h_, radius);
        BoxColumns(scratch_.data(), small_.data(), small_w_, small_h_, radius);
    }
}

void BackgroundBlur::Composite(uint8_t* bgra, int width, int height) {
    // Turn the blurred premultiplied background into plain colours once, at the small size.
    const size_t small_count = static_cast<size_t>(small_w_) * small_h_;
    for (size_t i = 0; i < small_count; ++i) {
        float* c = small_.data() + i * 4;
        const float inv_w = 1.0f / std::max(c[3], 1e-4f);
        c[0] *= inv_w;
        c[1] *= inv_w;
        c[2] *= inv_w;
    }

    Taps mask_x, mask_y, bg_x, bg_y;
    MakeTaps(width, mask_w_, &mask_x);
    MakeTaps(height, mask_h_, &mask_y);
    MakeTaps(width, small_w_, &bg_x);
    MakeTaps(height, small_h_, &bg_y);

    // Each output row first interpolates between two source rows, then along the row.
    std::vector<float> mask_row(mask_w_);
    std::vector<float> bg_row(static_cast<size_t>(small_w_) * 3);
    for (int y = 0; y < height; ++y) {
        const float* m0 = mask_.data() + static_cast<size_t>(mask_y.i0[y]) * mask_w_;
        const float* m1 = mask_.data() + static_cast<size_t>(mask_y.i1[y]) * mask_w_;
        const float mfy = mask_y.f[y];
        for (int i = 0; i < mask_w_; ++i) mask_row[i] = m0[i] + (m1[i] - m0[i]) * mfy;

        bool row_has_background = false;
        const float* s0 = small_.data() + static_cast<size_t>(bg_y.i0[y]) * small_w_ * 4;
        const float* s1 = small_.data() + static_cast<size_t>(bg_y.i1[y]) * small_w_ * 4;
        const float bfy = bg_y.f[y];
        uint8_t* p = bgra + static_cast<size_t>(y) * width * 4;
        for (int x = 0; x < width; ++x, p += 4) {
            const int ma = mask_x.i0[x];
            const float a = PersonAlpha(mask_row[ma] + (mask_row[mask_x.i1[x]] - mask_row[ma]) * mask_x.f[x]);
            if (a >= 0.999f) continue;  // the person: keep as is
            if (!row_has_background) {
                row_has_background = true;
                for (int i = 0; i < small_w_; ++i) {
                    for (int k = 0; k < 3; ++k) {
                        bg_row[i * 3 + k] = s0[i * 4 + k] + (s1[i * 4 + k] - s0[i * 4 + k]) * bfy;
                    }
                }
            }
            const float* c0 = bg_row.data() + bg_x.i0[x] * 3;
            const float* c1 = bg_row.data() + bg_x.i1[x] * 3;
            const float fx = bg_x.f[x];
            for (int k = 0; k < 3; ++k) {
                const float bg = c0[k] + (c1[k] - c0[k]) * fx;
                const float v = bg + (p[k] - bg) * a;
                p[k] = static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v + 0.5f));
            }
        }
    }
}
