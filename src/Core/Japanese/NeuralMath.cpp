#include "Core/Japanese/NeuralMath.h"

#include "Core/Utf8.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <intrin.h>
#else
#include <cpuid.h>
#include <unicode/normalizer2.h>
#include <unicode/unistr.h>
#endif
#include <immintrin.h>

#include <algorithm>
#include <cmath>

// GCC and Clang compile intrinsics only in functions built for them (MSVC
// takes them anywhere); each kernel is its own function, so the rest stays
// plain x86-64.
#if defined(_MSC_VER)
#define TEKITO_TARGET(isa)
#else
#define TEKITO_TARGET(isa) __attribute__((target(isa)))
#endif

namespace tekito::japanese {
namespace {

#if defined(_MSC_VER)
void CpuId(int info[4], int leaf) noexcept { __cpuidex(info, leaf, 0); }
unsigned long long XGetBv() noexcept { return _xgetbv(0); }
#else
void CpuId(int info[4], int leaf) noexcept {
    unsigned a = 0, b = 0, c = 0, d = 0;
    __cpuid_count(static_cast<unsigned>(leaf), 0, a, b, c, d);
    info[0] = static_cast<int>(a);
    info[1] = static_cast<int>(b);
    info[2] = static_cast<int>(c);
    info[3] = static_cast<int>(d);
}
TEKITO_TARGET("xsave") unsigned long long XGetBv() noexcept { return _xgetbv(0); }
#endif

Simd DetectSimd() noexcept {
    int info[4]{};
    CpuId(info, 1);
    const bool ssse3 = (info[2] & (1 << 9)) != 0;
    // AVX2 needs the OS to save the YMM registers too.
    const bool osSaves = (info[2] & (1 << 27)) != 0 && (XGetBv() & 6) == 6;
    CpuId(info, 7);
    if (osSaves && (info[1] & (1 << 5)) != 0) return Simd::Avx2;
    return ssse3 ? Simd::Ssse3 : Simd::None;
}

}  // namespace

Simd CurrentSimd() noexcept {
    static const Simd simd = DetectSimd();
    return simd;
}

// The sum of exp(x[i] - shift), x[i] <= shift; eight at a time with AVX2
// (Cephes' expf: e^x = 2^n e^r, r within ln 2 / 2, by a polynomial).
namespace {

// The AVX2 part of SumExp: eight at a time, from `i` on; `i` is left where
// it stopped.
TEKITO_TARGET("avx2") float SumExpAvx2(const float* x, std::size_t n, float shift, std::size_t& i) noexcept {
    float total = 0;
    const __m256 lowest = _mm256_set1_ps(-87.0f), log2e = _mm256_set1_ps(1.44269504088896341f);
    const __m256 c1 = _mm256_set1_ps(0.693359375f), c2 = _mm256_set1_ps(-2.12194440e-4f);
    const __m256 p0 = _mm256_set1_ps(1.9875691500e-4f), p1 = _mm256_set1_ps(1.3981999507e-3f);
    const __m256 p2 = _mm256_set1_ps(8.3334519073e-3f), p3 = _mm256_set1_ps(4.1665795894e-2f);
    const __m256 p4 = _mm256_set1_ps(1.6666665459e-1f), p5 = _mm256_set1_ps(5.0000001201e-1f);
    const __m256 one = _mm256_set1_ps(1.0f), half = _mm256_set1_ps(0.5f), shiftBy = _mm256_set1_ps(shift);
    __m256 sum = _mm256_setzero_ps();
    for (; i + 8 <= n; i += 8) {
        __m256 v = _mm256_max_ps(_mm256_sub_ps(_mm256_loadu_ps(x + i), shiftBy), lowest);
        const __m256 k = _mm256_floor_ps(_mm256_add_ps(_mm256_mul_ps(v, log2e), half));
        v = _mm256_sub_ps(_mm256_sub_ps(v, _mm256_mul_ps(k, c1)), _mm256_mul_ps(k, c2));
        __m256 y = _mm256_add_ps(_mm256_mul_ps(p0, v), p1);
        y = _mm256_add_ps(_mm256_mul_ps(y, v), p2);
        y = _mm256_add_ps(_mm256_mul_ps(y, v), p3);
        y = _mm256_add_ps(_mm256_mul_ps(y, v), p4);
        y = _mm256_add_ps(_mm256_mul_ps(y, v), p5);
        y = _mm256_add_ps(_mm256_add_ps(_mm256_mul_ps(_mm256_mul_ps(y, v), v), v), one);
        const __m256i exponent = _mm256_slli_epi32(_mm256_add_epi32(_mm256_cvttps_epi32(k), _mm256_set1_epi32(127)), 23);
        sum = _mm256_add_ps(sum, _mm256_mul_ps(y, _mm256_castsi256_ps(exponent)));
    }
    alignas(32) float lanes[8];
    _mm256_store_ps(lanes, sum);
    for (const float lane : lanes) total += lane;
    return total;
}

}  // namespace

float SumExp(const float* x, std::size_t n, float shift) noexcept {
    std::size_t i = 0;
    float total = 0;
    if (CurrentSimd() == Simd::Avx2) total = SumExpAvx2(x, n, shift, i);
    for (; i < n; ++i) total += std::exp(x[i] - shift);
    return total;
}

// Each of `rows` rows of `matrix` (n int8 each) dotted with `vector`, into
// `out`. n is a multiple of 16; |values| <= 127, so a pair of products
// fits in int16. maddubs multiplies unsigned by signed: |vector| by each
// row with the vector's signs.
namespace {

TEKITO_TARGET("avx2")
void DotRowsAvx2(const std::int8_t* matrix, std::size_t rows, std::size_t n, const std::int8_t* vector,
                 std::int32_t* out) noexcept {
    const __m256i ones = _mm256_set1_epi16(1);
    for (std::size_t r = 0; r < rows; ++r) {
        const std::int8_t* row = matrix + r * n;
        __m256i sum = _mm256_setzero_si256();
        for (std::size_t i = 0; i < n; i += 32) {
            const __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(vector + i));
            const __m256i w = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(row + i));
            const __m256i pairs = _mm256_maddubs_epi16(_mm256_abs_epi8(v), _mm256_sign_epi8(w, v));
            sum = _mm256_add_epi32(sum, _mm256_madd_epi16(pairs, ones));
        }
        __m128i half = _mm_add_epi32(_mm256_castsi256_si128(sum), _mm256_extracti128_si256(sum, 1));
        half = _mm_add_epi32(half, _mm_shuffle_epi32(half, _MM_SHUFFLE(1, 0, 3, 2)));
        half = _mm_add_epi32(half, _mm_shuffle_epi32(half, _MM_SHUFFLE(2, 3, 0, 1)));
        out[r] = _mm_cvtsi128_si32(half);
    }
}

TEKITO_TARGET("ssse3")
void DotRowsSsse3(const std::int8_t* matrix, std::size_t rows, std::size_t n, const std::int8_t* vector,
                  std::int32_t* out) noexcept {
    const __m128i ones = _mm_set1_epi16(1);
    for (std::size_t r = 0; r < rows; ++r) {
        const std::int8_t* row = matrix + r * n;
        __m128i sum = _mm_setzero_si128();
        for (std::size_t i = 0; i < n; i += 16) {
            const __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(vector + i));
            const __m128i w = _mm_loadu_si128(reinterpret_cast<const __m128i*>(row + i));
            const __m128i pairs = _mm_maddubs_epi16(_mm_abs_epi8(v), _mm_sign_epi8(w, v));
            sum = _mm_add_epi32(sum, _mm_madd_epi16(pairs, ones));
        }
        sum = _mm_add_epi32(sum, _mm_shuffle_epi32(sum, _MM_SHUFFLE(1, 0, 3, 2)));
        sum = _mm_add_epi32(sum, _mm_shuffle_epi32(sum, _MM_SHUFFLE(2, 3, 0, 1)));
        out[r] = _mm_cvtsi128_si32(sum);
    }
}

}  // namespace

void DotRows(const std::int8_t* matrix, std::size_t rows, std::size_t n, const std::int8_t* vector,
             std::int32_t* out) noexcept {
    const Simd simd = CurrentSimd();
    if (simd == Simd::Avx2 && n % 32 == 0) {
        DotRowsAvx2(matrix, rows, n, vector, out);
        return;
    }
    if (simd != Simd::None) {
        DotRowsSsse3(matrix, rows, n, vector, out);
        return;
    }
    for (std::size_t r = 0; r < rows; ++r) {
        std::int32_t sum = 0;
        for (std::size_t i = 0; i < n; ++i) sum += static_cast<std::int32_t>(matrix[r * n + i]) * vector[i];
        out[r] = sum;
    }
}

// `values` as int8 and the scale that gives them back.
float Quantize(const std::vector<float>& values, std::vector<std::int8_t>& out) {
    float largest = 0;
    for (const float v : values) largest = std::max(largest, std::abs(v));
    const float scale = largest > 0 ? largest / 127.0f : 1.0f;
    out.resize(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        out[i] = static_cast<std::int8_t>(std::clamp(std::lround(values[i] / scale), -127L, 127L));
    }
    return scale;
}

std::wstring Nfkc(std::wstring_view text) {
    std::wstring nfkc;
    if (text.empty()) return nfkc;
#if defined(_WIN32)
    int size = NormalizeString(NormalizationKC, text.data(), static_cast<int>(text.size()), nullptr, 0);
    for (int attempt = 0; attempt < 4 && size > 0; ++attempt) {
        nfkc.resize(static_cast<std::size_t>(size));
        size = NormalizeString(NormalizationKC, text.data(), static_cast<int>(text.size()), nfkc.data(), size);
        if (size > 0) {
            nfkc.resize(static_cast<std::size_t>(size));
            return nfkc;
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) break;
        size = -size;
    }
    return std::wstring(text);
#else
    UErrorCode status = U_ZERO_ERROR;
    const icu::Normalizer2* normalizer = icu::Normalizer2::getNFKCInstance(status);
    if (U_FAILURE(status) || !normalizer) return std::wstring(text);
    const std::string bytes = utf8::Encode(text);
    const icu::UnicodeString normalized =
        normalizer->normalize(icu::UnicodeString::fromUTF8(icu::StringPiece(bytes.data(), static_cast<int32_t>(bytes.size()))), status);
    if (U_FAILURE(status)) return std::wstring(text);
    std::string out;
    normalized.toUTF8String(out);
    return utf8::DecodeLenient(out);
#endif
}

}  // namespace tekito::japanese
