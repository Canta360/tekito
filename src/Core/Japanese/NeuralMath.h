#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Small helpers the Japanese models share: int8 dot products (AVX2 or SSSE3
// when the CPU has them), a vector exp sum, and NFKC.
namespace tekito::japanese {

enum class Simd { None, Ssse3, Avx2 };
[[nodiscard]] Simd CurrentSimd() noexcept;

// The sum of exp(x[i] - shift), x[i] <= shift.
[[nodiscard]] float SumExp(const float* x, std::size_t n, float shift) noexcept;

// Each of `rows` rows of `matrix` (n int8 each, n a multiple of 16) dotted
// with `vector`, into `out`; values within +-127.
void DotRows(const std::int8_t* matrix, std::size_t rows, std::size_t n, const std::int8_t* vector,
             std::int32_t* out) noexcept;

// `values` as int8 and the scale that gives them back.
float Quantize(const std::vector<float>& values, std::vector<std::int8_t>& out);

[[nodiscard]] inline float Sigmoid(float x) noexcept { return 1.0f / (1.0f + std::exp(-x)); }

// `text` in Unicode normalization form KC (full-width letters half-width).
[[nodiscard]] std::wstring Nfkc(std::wstring_view text);

}  // namespace tekito::japanese
