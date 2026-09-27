#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>

namespace tekito::japanese {

// A read-only file mapped into memory. Every process that maps the same file
// shares its pages, which matters for data an input method loads into every
// application.
class MappedFile final {
public:
    MappedFile() = default;
    ~MappedFile();
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    bool Open(const std::filesystem::path& path) noexcept;
    void Close() noexcept;

    [[nodiscard]] const std::uint8_t* Data() const noexcept { return data_; }
    [[nodiscard]] std::size_t Size() const noexcept { return size_; }

private:
    const std::uint8_t* data_{nullptr};
    std::size_t size_{0};
    void* file_{nullptr};
    void* mapping_{nullptr};
};

}  // namespace tekito::japanese
