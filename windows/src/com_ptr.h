// Minimal COM smart pointer, so the client builds with both MSVC and MinGW.
#pragma once

#include <unknwn.h>

template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ~ComPtr() { Reset(); }

    T* Get() const { return p_; }
    T* operator->() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }

    // Releases the current pointer and returns the address for an out-parameter.
    T** Put() {
        Reset();
        return &p_;
    }
    void** PutVoid() { return reinterpret_cast<void**>(Put()); }

    void Reset() {
        if (p_) {
            p_->Release();
            p_ = nullptr;
        }
    }

private:
    T* p_ = nullptr;
};
