#pragma once

#include <utility>

namespace tekito::tsf {

template <typename T>
class ComPtr final {
public:
    ComPtr() = default;
    explicit ComPtr(T* value) : value_(value) {}
    ~ComPtr() { Reset(); }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    ComPtr(ComPtr&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            Reset();
            value_ = std::exchange(other.value_, nullptr);
        }
        return *this;
    }

    T* Get() const noexcept { return value_; }
    T** Put() noexcept {
        Reset();
        return &value_;
    }
    T* operator->() const noexcept { return value_; }
    explicit operator bool() const noexcept { return value_ != nullptr; }

    T* Detach() noexcept { return std::exchange(value_, nullptr); }
    void Attach(T* value) noexcept {
        Reset();
        value_ = value;
    }
    void Reset() noexcept {
        if (value_) {
            value_->Release();
            value_ = nullptr;
        }
    }

private:
    T* value_{nullptr};
};

}  // namespace tekito::tsf
