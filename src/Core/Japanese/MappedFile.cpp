#include "Core/Japanese/MappedFile.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace tekito::japanese {

MappedFile::~MappedFile() {
    Close();
}

#if defined(_WIN32)

bool MappedFile::Open(const std::filesystem::path& path) noexcept {
    Close();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
        static_cast<unsigned long long>(size.QuadPart) > SIZE_MAX) {
        CloseHandle(file);
        return false;
    }
    HANDLE mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    const void* view = mapping ? MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0) : nullptr;
    if (!view) {
        if (mapping) CloseHandle(mapping);
        CloseHandle(file);
        return false;
    }
    file_ = file;
    mapping_ = mapping;
    data_ = static_cast<const std::uint8_t*>(view);
    size_ = static_cast<std::size_t>(size.QuadPart);
    return true;
}

void MappedFile::Close() noexcept {
    if (data_) UnmapViewOfFile(data_);
    if (mapping_) CloseHandle(static_cast<HANDLE>(mapping_));
    if (file_) CloseHandle(static_cast<HANDLE>(file_));
    data_ = nullptr;
    size_ = 0;
    mapping_ = nullptr;
    file_ = nullptr;
}

#else

bool MappedFile::Open(const std::filesystem::path& path) noexcept {
    Close();
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    struct stat info {};
    if (fstat(fd, &info) != 0 || info.st_size <= 0) {
        ::close(fd);
        return false;
    }
    void* view = mmap(nullptr, static_cast<std::size_t>(info.st_size), PROT_READ, MAP_SHARED, fd, 0);
    ::close(fd);
    if (view == MAP_FAILED) return false;
    data_ = static_cast<const std::uint8_t*>(view);
    size_ = static_cast<std::size_t>(info.st_size);
    return true;
}

void MappedFile::Close() noexcept {
    if (data_) munmap(const_cast<std::uint8_t*>(data_), size_);
    data_ = nullptr;
    size_ = 0;
}

#endif

}  // namespace tekito::japanese
