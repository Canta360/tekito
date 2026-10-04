#include "Core/Japanese/JapaneseDictionary.h"

#include "Core/Japanese/KanaText.h"

namespace tekito::japanese {
namespace {

template <typename T>
T ReadAt(const std::uint8_t* base, std::size_t offset) noexcept {
    T value;
    std::memcpy(&value, base + offset, sizeof(T));
    return value;
}

bool Within(std::uint64_t offset, std::uint64_t bytes, std::uint64_t size) noexcept {
    return offset <= size && bytes <= size - offset;
}

}  // namespace

bool JapaneseDictionary::Open(const std::filesystem::path& file) noexcept {
    keyCount_ = 0;
    if (!file_.Open(file)) return false;
    const std::uint8_t* p = file_.Data();
    const std::size_t size = file_.Size();
    constexpr std::size_t kHeader = 64;
    if (size < kHeader || std::memcmp(p, "TKJD", 4) != 0) return false;
    const auto field = [&](int i) { return ReadAt<std::uint32_t>(p, 4 + 4 * static_cast<std::size_t>(i)); };
    if (field(0) != 1 || field(10) != size) return false;
    const std::uint32_t keyCount = field(1);
    const std::uint32_t charCount = field(3);
    const std::uint32_t reverseOffset = field(4);
    const std::uint32_t indexOffset = field(6);
    const std::uint32_t recordsOffset = field(7);
    const std::uint32_t surfacesOffset = field(8);
    const std::uint32_t surfaceUnits = field(9);
    const std::uint32_t unknownId = field(11);
    if (keyCount == 0 || charCount == 0 || charCount > 255 || unknownId > 0xFFFF ||
        !Within(reverseOffset, 65536, size) ||
        !Within(indexOffset, 4ull * (keyCount + 1ull), size) ||
        !Within(surfacesOffset, 2ull * surfaceUnits, size) || recordsOffset > surfacesOffset) {
        return false;
    }
    const std::uint32_t recordsEnd = ReadAt<std::uint32_t>(p, indexOffset + 4ull * keyCount);
    if (!Within(recordsOffset, recordsEnd, surfacesOffset)) return false;

    reverse_ = p + reverseOffset;
    const std::uint32_t charsOffset = field(5);
    if (!Within(charsOffset, 2ull * charCount, size)) return false;
    chars_ = p + charsOffset;
    charCount_ = charCount;
    index_ = p + indexOffset;
    records_ = p + recordsOffset;
    recordsSize_ = recordsEnd;
    surfaces_ = p + surfacesOffset;
    surfaceUnits_ = surfaceUnits;
    unknownId_ = static_cast<std::uint16_t>(unknownId);
    keyCount_ = keyCount;
    return true;
}

ReadingCodes JapaneseDictionary::Encode(std::wstring_view reading) const {
    ReadingCodes codes(reading.size(), 0);
    if (!reverse_) return codes;
    for (std::size_t i = 0; i < reading.size(); ++i) {
        codes[i] = reverse_[static_cast<std::uint16_t>(reading[i])];
    }
    return codes;
}

std::uint32_t JapaneseDictionary::RecordOffset(std::uint32_t record) const noexcept {
    return Read<std::uint32_t>(index_ + 4ull * record);
}

ReadingCodesView JapaneseDictionary::Key(std::uint32_t record) const noexcept {
    if (record >= keyCount_) return {};
    const std::uint32_t at = RecordOffset(record);
    if (at >= recordsSize_) return {};
    const std::uint32_t length = records_[at];
    if (at + 1ull + length > recordsSize_) return {};
    return {records_ + at + 1, length};
}

std::optional<std::uint32_t> JapaneseDictionary::Find(ReadingCodesView codes) const {
    std::optional<std::uint32_t> result;
    CommonPrefixSearch(codes, [&](std::size_t length, std::uint32_t record) {
        if (length == codes.size()) result = record;
    });
    return result;
}

std::pair<std::uint32_t, std::uint32_t> JapaneseDictionary::PrefixRange(ReadingCodesView codes) const {
    std::uint32_t lo = 0;
    std::uint32_t hi = keyCount_;
    for (std::size_t depth = 0; depth < codes.size() && lo < hi; ++depth) {
        const std::uint8_t code = codes[depth];
        if (code == 0) return {0, 0};
        const auto codeAt = [&](std::uint32_t record) -> int {
            const auto key = Key(record);
            return depth < key.size() ? key[depth] : -1;
        };
        std::uint32_t first = lo, last = hi;
        while (first < last) {
            const std::uint32_t middle = first + (last - first) / 2;
            if (codeAt(middle) < code) first = middle + 1; else last = middle;
        }
        std::uint32_t end = first, limit = hi;
        while (end < limit) {
            const std::uint32_t middle = end + (limit - end) / 2;
            if (codeAt(middle) <= code) end = middle + 1; else limit = middle;
        }
        lo = first;
        hi = end;
    }
    return {lo, lo < hi ? hi : lo};
}

std::wstring JapaneseDictionary::KeyText(std::uint32_t record) const {
    std::wstring text;
    for (const std::uint8_t code : Key(record)) {
        if (code == 0 || code > charCount_) return {};
        text += static_cast<wchar_t>(Read<std::uint16_t>(chars_ + 2ull * (code - 1)));
    }
    return text;
}

std::wstring JapaneseDictionary::Surface(const DictionaryWord& word, std::wstring_view reading) const {
    switch (word.surface) {
    case DictionaryWord::Surface::Reading:
        return std::wstring(reading);
    case DictionaryWord::Surface::Katakana:
        return ToKatakana(reading);
    case DictionaryWord::Surface::Pool:
        break;
    }
    if (word.surfaceOffset >= surfaceUnits_) return std::wstring(reading);
    const std::uint16_t length = Read<std::uint16_t>(surfaces_ + 2ull * word.surfaceOffset);
    if (word.surfaceOffset + 1ull + length > surfaceUnits_) return std::wstring(reading);
    std::wstring out(length, L'\0');
    std::memcpy(out.data(), surfaces_ + 2ull * word.surfaceOffset + 2, 2ull * length);
    return out;
}

std::size_t JapaneseDictionary::PoolSurfaceLength(const DictionaryWord& word) const noexcept {
    if (word.surface != DictionaryWord::Surface::Pool || word.surfaceOffset >= surfaceUnits_) return 0;
    const std::uint16_t length = Read<std::uint16_t>(surfaces_ + 2ull * word.surfaceOffset);
    return word.surfaceOffset + 1ull + length > surfaceUnits_ ? 0 : length;
}

wchar_t JapaneseDictionary::PoolSurfaceChar(const DictionaryWord& word, std::size_t i) const noexcept {
    if (i >= PoolSurfaceLength(word)) return 0;
    return static_cast<wchar_t>(Read<std::uint16_t>(surfaces_ + 2ull * (word.surfaceOffset + 1 + i)));
}

bool ConnectionMatrix::Open(const std::filesystem::path& file) noexcept {
    size_ = 0;
    if (!file_.Open(file)) return false;
    const std::uint8_t* p = file_.Data();
    const std::size_t size = file_.Size();
    constexpr std::size_t kHeader = 40;
    if (size < kHeader || std::memcmp(p, "TKJM", 4) != 0) return false;
    const auto field = [&](int i) { return ReadAt<std::uint32_t>(p, 4 + 4 * static_cast<std::size_t>(i)); };
    const std::uint32_t version = field(0);
    const std::uint32_t matrixSize = field(1);
    const std::uint32_t format = field(2);
    const std::uint32_t scale = field(3);
    const std::uint32_t classCount = field(4);
    const std::uint32_t costsOffset = field(5);
    const std::uint32_t classesOffset = field(6);
    const std::uint32_t boundariesOffset = field(7);
    if (version != 1 || format != 1 || field(8) != size || matrixSize == 0 || matrixSize > 0xFFFF ||
        scale == 0 || classCount == 0) {
        return false;
    }
    const std::uint32_t rowBytes = (matrixSize + 7) / 8;
    if (!Within(costsOffset, 1ull * matrixSize * matrixSize, size) ||
        !Within(classesOffset, 2ull * matrixSize, size) ||
        !Within(boundariesOffset, 1ull * classCount * rowBytes, size)) {
        return false;
    }
    costs_ = p + costsOffset;
    classes_ = p + classesOffset;
    boundaries_ = p + boundariesOffset;
    scale_ = scale;
    classCount_ = classCount;
    rowBytes_ = rowBytes;
    size_ = matrixSize;
    return true;
}

int ConnectionMatrix::Cost(std::uint16_t previousRight, std::uint16_t nextLeft) const noexcept {
    if (previousRight >= size_ || nextLeft >= size_) return kUnknownCost;
    return costs_[1ull * previousRight * size_ + nextLeft] * static_cast<int>(scale_);
}

bool ConnectionMatrix::IsBoundary(std::uint16_t previousRight, std::uint16_t nextLeft) const noexcept {
    if (previousRight >= size_ || nextLeft >= size_) return true;
    const std::uint16_t rowClass = ReadAt<std::uint16_t>(classes_, 2ull * previousRight);
    if (rowClass >= classCount_) return true;
    return ((boundaries_[1ull * rowClass * rowBytes_ + nextLeft / 8] >> (nextLeft % 8)) & 1) != 0;
}

}  // namespace tekito::japanese
